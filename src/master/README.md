# Master Server (`leil-master`) -- Architectural Reference

The master server is the central metadata authority in a LeilFS cluster. It
maintains the entire filesystem namespace (inodes, directories, edges, xattrs,
ACLs, quotas, locks) in memory and coordinates chunk placement across
chunkservers. The binary produced from this directory is **`leil-master`**.

A single codebase supports two runtime **personalities**:

- **Master** -- the active metadata server that accepts client mutations.
- **Shadow** -- a hot-standby that replays changelogs from the master and can
  be promoted at any time.

The personality is selected at startup via configuration and can only transition
from Shadow to Master (never the reverse). Conditional compilation guards
(`METARESTORE`, `METALOGGER`) also allow parts of this code to be linked into
the `leil-metarestore` and `leil-metalogger` binaries.

## Code Organization

The directory is flat (no subdirectories). Files are grouped by **filename
prefix**, and each group corresponds to a logical subsystem:

| Prefix / group        | Subsystem                                        |
|-----------------------|--------------------------------------------------|
| `filesystem_*`        | Core metadata model, operations, and maintenance |
| `matoclserv*`         | Client (FUSE mount) protocol server              |
| `matocsserv*`         | Chunkserver protocol server                      |
| `matomlserv*`         | Metalogger / shadow protocol server              |
| `matontserv*`         | Notifier (inotify-like) protocol server          |
| `masterconn*`         | Shadow-to-master replication connection          |
| `metadata_backend_*`  | Metadata persistence (load / save / dump)        |
| `metadata_loader*`    | Section-based async metadata loading             |
| `metadata_dumper_*`   | Periodic metadata dumping                        |
| `chunks*`             | Chunk lifecycle and replication tracking         |
| `changelog*`          | Write-ahead log (WAL)                            |
| `restore*`            | Changelog replay (apply entries to metadata)     |
| `hstring*`            | Efficient string (filename) storage              |
| `kv_*`                | KV store integration                             |
| `task_manager*`       | Async task execution framework                   |
| `locks*`              | POSIX and flock advisory file locking            |
| `acl_storage*`        | Deduplicated RichACL storage                     |
| `exports*`            | Client mount permissions and IP-based rules      |
| `topology*`           | Network topology for data-locality placement     |
| `personality*`        | Master / Shadow personality management           |
| `chartsdata*`         | Monitoring / charting data collection            |

A key organizational convention is the **interface / implementation split**:
public interfaces live in `*_interface.h` files as pure virtual classes (e.g.
`IFilesystemOperations`, `IFilesystemNodeOperations`, `IChunkOperations`,
`IMetadataBackend`, `IKVConnector`), while default in-memory implementations use
a `*Base` suffix or reside in the corresponding `.cc` file.

## Core Data Model

Core namespace metadata lives in a **`FilesystemMetadata`** instance addressed
through the global pointer `gMetadata` (declared in
`filesystem_metadata.h`). Chunk metadata is managed separately in `chunks.cc`
(`gChunksMetadata`). Key `gMetadata` members are:

- **`nodeHash`** -- a fixed-size hash table (4M buckets) mapping inode IDs to
  `FSNode*` pointers. This is the primary index for all filesystem objects.
- **`root`** -- pointer to the root `FSNodeDirectory`.
- **`trash` / `reserved`** -- containers for deleted files (awaiting trashtime
  expiry) and files held open by clients after unlinking.
- **`inodePool`** (`IdPoolDetainer`) -- inode ID allocation with a reuse delay
  (currently derived from compile-time constant `SFS_INODE_REUSE_DELAY`) to
  prevent stale file handles (for example, NFS handles) from hitting recycled
  inodes.
- **`aclStorage`** -- reference-counted, deduplicated RichACL store.
- **`xattrInodeHash` / `xattrDataHash`** -- extended attribute storage.
- **`quotaDatabase`** -- per-user/group/directory quota tracking.
- **`flockLocks` / `posixLocks`** -- advisory file lock state.
- **`taskManager`** -- the async task execution engine (see below).
- **`metadataVersion`** -- monotonically increasing version counter, bumped on
  every metadata mutation.
- **Signals** -- `nodeChangedSignal`, `edgeChangedSignal`, `edgeRemovedSignal`
  emitted on metadata changes; these are extension points for observers such as
  KV connectors.

### Node Type Hierarchy

`FSNode` (64 bytes) is the base class for all filesystem objects. Concrete
subtypes:

| Class             | Type                | Extra fields                         |
|-------------------|---------------------|--------------------------------------|
| `FSNodeFile`      | file, trash, reserved | `length`, `chunks[]`, `sessionIds[]`|
| `FSNodeDirectory` | directory           | `entries` (SkipList), `stats`, `nlink`, `lowerCaseEntries` |
| `FSNodeSymlink`   | symlink             | `path`, `path_length`                |
| `FSNodeDevice`    | block/char device   | `rdev`                               |

Every node stores a `parents` compact vector of `(inode_t, Handle*)` pairs,
providing reverse links from child to parent(s) (files can have multiple
parents via hard links).

Directory entries use a **SkipList** keyed by hashed name handles, with an
optional parallel `lowerCaseEntries` SkipList for case-insensitive
filesystems.

Each `FSNodeDirectory` maintains an aggregate `StatsRecord` (inodes, dirs,
files, links, chunks, length, size, realsize) that is incrementally propagated
up the tree on every mutation, enabling O(1) `dirinfo` queries at any level.

## Filesystem Operations

Operations are organized in a two-layer interface hierarchy:

1. **`IFilesystemOperations`** (`filesystem_operations_interface.h`) --
   high-level POSIX-like API: `lookup`, `mknod`, `mkdir`, `unlink`, `rmdir`,
   `rename`, `link`, `symlink`, `setAttr`, `readdir`, `writeChunk`,
   `readChunk`, quota management, lock operations, etc. The global instance
   is `gFSOperations`.

2. **`IFilesystemNodeOperations`** (`filesystem_node_operations_interface.h`)
   -- lower-level node CRUD: `createNode`, `link`, `unlink`, `removeEdge`,
   `purge`, `getPath`, stats propagation, ACL inheritance, etc.

`IFilesystemOperations` delegates to `IFilesystemNodeOperations` via its
`nodeOperations()` accessor. Both have in-memory default implementations
(`FilesystemOperationsBase`, `FilesystemNodeOperationsBase`) that operate
directly on `gMetadata`.

The **`FsContext`** (`fs_context.h`) carries per-operation state: personality,
session data, uid/gid, timestamps. The **`FilesystemOperationContext`**
(`filesystem_operation_context.h`) carries optional transaction handles; in the
current in-memory implementation these are unused (see the KV Store section
below for context).

## Chunk Management

Chunk state is managed in `chunks.h` / `chunks.cc`. Key responsibilities:

- **Chunk lifecycle** -- creation, version bumps, deletion, truncation.
- **Location tracking** -- which chunkserver holds which chunk and version,
  reported via `chunk_server_has_chunk()`.
- **Replication decisions** -- tracking under-goal / over-goal chunks and
  issuing recover/remove operations.
- **Operation callbacks** -- `chunk_got_create_status`,
  `chunk_got_replicate_status`, etc., process async results from chunkservers.
- **ID generation** -- `gChunkIdGenerator` (an `IIdGeneratorWithState`).
- **Server selection** -- `get_servers_for_new_chunk.*` implements the
  algorithm for picking chunkservers considering labels, weights, disk usage,
  and load balancing.

Files reference chunks through `FSNodeFile::chunks`, a vector of chunk IDs
indexed by chunk index (offset / chunk_size).

The `gChunkChangedSignal` notifies observers when chunk metadata changes.

Chunk operations are reached through the **`IChunkOperations`** interface
(`chunk_operations_interface.h`). `ChunkOperationsBase` is the in-memory default,
forwarding to the `chunks.cc` free functions above; the master binds
`gChunkOperations` to `ChunkOperationsInMemory` (an empty leaf over `Base`). This
is the seam a KV-backed build overrides with `ChunkOperationsKV` to persist chunk
refcount/version to a KV store.

## Network Servers

The master communicates with five types of peers. The naming convention
`mato*serv` means "**ma**ster **to** *X* **serv**ice":

| Module         | Peer            | Role |
|----------------|-----------------|------|
| `matoclserv`   | FUSE clients    | Handles all client filesystem requests (the largest module). Manages delayed chunk operations and session state. |
| `matocsserv`   | Chunkservers    | Sends chunk create/delete/replicate/truncate/set-version commands. Tracks disk usage, labels, and connection state. |
| `matomlserv`   | Metaloggers & shadows | Broadcasts changelog entries and metadata snapshots for replication. |
| `matontserv`   | Notifier clients | Broadcasts changelog events for inotify-like functionality. |
| `masterconn`   | Active master   | Used only when running in **Shadow** personality. Receives changelogs and metadata from the active master. |

## Client Sessions

Client session lifecycle is routed through `ISessionManager`. The file-backed
master binds `SessionManagerFile`; KV builds can bind a different manager
without changing the shared `Session` runtime object or the `matoclserv`
protocol handlers.

`matoclserv_sessions.cc` is the dispatcher layer. Its wrappers forward to the
active manager and assert if they are called before startup attaches one.

For the file-backed master, `SessionManagerFile` keeps the complete in-memory
session catalog and persists reconnectable session records to `sessions.sfs`.
The loader accepts the current session-file header only; old `MFS`-branded and
pre-1.6.4 session files are rejected at startup instead of being interpreted by
compatibility branches. `storeSessions()` continues to write the current format.

Session durability is split between the session file and metadata:

- `sessions.sfs` stores reconnectable mount identity, access limits, UID/GID
  mapping, and hourly operation stats.
- `metadata.sfs` stores the session-id counter and each file's
  `FSNodeFile::sessionIds` list.
- `Session::openFilesSet` is an in-memory, per-session index rebuilt on startup
  from the file-side `sessionIds` metadata.

That split gives the master both lookup directions: "which files does this
session hold?" through `openFilesSet`, and "which sessions hold this file?"
through `FSNodeFile::sessionIds`. The file-side list is the durable authority
for reserved-file lifetime and purge decisions.

The lifecycle remains the traditional master lifecycle: create, reconnect,
disconnect, explicit delete, and timeout cleanup. Session ids are still
allocated through `gFSOperations->newSessionId()`, which records the allocation
in the changelog for the file-backed master path.

Explicit delete and timeout cleanup both call `matocl_close_files()` to release
open files and locks. The helper reports whether teardown completed; timeout
cleanup erases a session only after successful teardown, so a failed transactional
backend commit can keep the session managed for a later retry.

## Metadata Persistence

Metadata durability is achieved through two complementary mechanisms:

### Full Metadata Snapshots

- **`IMetadataBackend`** (`metadata_backend_interface.h`) -- interface for
  loading and saving complete metadata. Methods: `init()`, `loadall()`,
  `store_fd()`, `commit_metadata_dump()`, `emergency_saves()`.
- **`MetadataBackendFile`** (`metadata_backend_file.*`) -- the file-based
  implementation. Stores metadata in `metadata.sfs` with section-based format.
  Rotates previous copies on save.
- **`MetadataLoader`** (`metadata_loader.h`) -- reads metadata sections from
  memory-mapped files, supporting async section loading.
- **`IMetadataDumper` / `MetadataDumperFile`** -- handles periodic metadata
  dumps (foreground or background). The dumper can run in a forked child
  process to avoid blocking the event loop.

### Changelog (Write-Ahead Log)

- **`changelog.*`** -- incremental WAL. Each mutation is recorded as a text
  entry in the format `<version>: <ts>|<COMMAND>(arg1,arg2,...)`.
- Changelogs are rotated (configurable `BACK_LOGS`), flushed after each write
  (configurable), and broadcast to metaloggers/shadows via `matomlserv`.
- **`restore.*`** -- replays changelog entries to reconstruct metadata state.
  Used by shadow masters during synchronization and by `leil-metarestore` for
  offline recovery.

## KV Store Backend

An alternative to file-based metadata storage, designed for distributed
metadata:

- **`IKVConnector`** (`kv_connector_interface.h`) -- interface for KV store
  operations and event handlers for metadata changes (`onNodeChanged`,
  `onEdgeChanged`, `onEdgeRemoved`, `onDetainedAdded`, etc.).
- **`kv_connector_fdb.*`** -- concrete KV store implementation (conditionally
  compiled). This integration is currently not wired into the default master
  initialization path in `init.h`.
- **`kv_common_keys.h`** -- defines key prefix conventions for all metadata
  sections in the KV store: `NODE_`, `EDGE_`, `FREE_`, `CHNK_`, `XATR_`,
  `ACLS_`, `QUOT_`, `FLCK_`, plus reverse indexes (`DIR_PARENT_`, `PARENT_`,
  `DIR_NODES_COUNT_`, `DIR_STATS_`).
- `FilesystemOperationContext` carries optional transaction handles. In the
  current in-memory master implementation these are empty, and they serve as an
  extension point for transactional backends.

## Task Manager and Async Tasks

Long-running or recursive operations are decomposed into small incremental
tasks to avoid blocking the single-threaded event loop:

- **`TaskManager`** (`task_manager.h`) -- generic job/task execution system.
  A **Job** is a named unit of work containing an ordered list of **Tasks**.
  The manager round-robins across jobs, executing a bounded batch of tasks per
  event loop iteration.
- **`SnapshotTask`** (`snapshot_task.h`) -- splits filesystem snapshot
  (clone) operations into per-inode tasks.
- **`RecursiveRemoveTask`** (`recursive_remove_task.h`) -- recursive directory
  deletion.
- **`SetGoalTask`** / **`SetTrashtimeTask`** -- recursive goal or trashtime
  changes across a subtree.
- **`DeferredMetadataDumpTask`** (`deferred_metadata_dump_task.h`) --
  post-failover metadata dump.

Jobs support cancellation and completion callbacks.

## Periodic Operations

Several maintenance routines run on timer-driven or per-loop schedules,
registered in `fs_periodic_master_init()` (see `filesystem_periodic.cc`):

- **File integrity test** (`fs_periodic_file_test` / `fs_background_file_test`):
  runs every second (timer) and every loop (background). Scans the entire node
  hash table in a configurable cycle time (`FILE_TEST_LOOP_MIN_TIME`, default
  3600s). For each file node, checks chunk availability and copy counts. For
  each directory, validates parent-child pointer consistency. Builds a
  `gDefectiveNodes` map of inodes with unavailable chunks, under-goal chunks,
  or structural errors.
- **Background task processing** (`fs_background_task_manager_work`):
  runs every loop. Drives the `TaskManager`, processing a batch of tasks
  (snapshots, recursive removes, goal/trashtime changes) per iteration.
- **Checksum recalculation** (`fs_background_checksum_recalculation_a_bit`):
  runs every loop. Incrementally recalculates metadata checksums (nodes,
  xattrs, chunks) in the background, progressing through steps at a speed
  limit per iteration.
- **Trash cleanup** (`fs_periodic_emptytrash`):
  runs every 100ms. Purges expired trash entries whose deletion timestamp has
  passed.
- **Reserved file cleanup** (`fs_periodic_emptyreserved`):
  runs on a configurable period in ms
  (`EMPTY_RESERVED_FILES_PERIOD_MSECONDS`), where `0` disables it.
  Force-releases reserved files (deleted-but-still-open files) from all owning
  sessions, even if sessions are still active; enabling it can disrupt clients
  that still hold those files.
- **Chunk maintenance** (in `chunks.cc`):
  runs periodically. Handles chunk replication, deletion of excess copies, and
  rebalancing across chunkservers.

## Initialization Sequence

Startup is orchestrated by ordered `RunTab` arrays in `init.h`. The sequence
is dependency-ordered -- comments in the source mark critical orderings:

```
2.  hstorage_init            -- String storage backend (must be first)
3.  personality_init         -- Set master/shadow personality (must be second)
4.  rnd_init                 -- Random number generator
5.  dcm_init                 -- Data cache manager (before fs_init and matoclserv)
6.  matoclserv_sessions_init -- Load persisted sessions (before fs_init)
7.  exports_init             -- Client mount/export permission rules
8.  topology_init            -- Network topology configuration
9.  metadata_backend_init    -- Initialize MetadataBackendFile + inode ID generator
10. fs_init                  -- Core filesystem: load metadata, register periodic ops
11. chartsdata_init          -- Monitoring charts data collection
12. masterconn_init          -- Shadow's connection to active master
13. matomlserv_init          -- Metalogger/shadow communication
14. matocsserv_init          -- Chunkserver communication
15. matontserv_init          -- Notifier communication
16. matoclserv_network_init  -- Client network init (last -- opens for business)
```

Client connections are accepted only after all other subsystems are ready.

## Key Design Patterns

- **Strategy / interface-based extensibility** -- all major subsystems are
  behind pure virtual interfaces (`IFilesystemOperations`,
  `IFilesystemNodeOperations`, `IMetadataBackend`, `IKVConnector`,
  `hstorage::Storage`), allowing alternative implementations to be plugged in.
- **Observer / signal pattern** -- `Signal<>` objects on `FilesystemMetadata`
  and in the chunk subsystem notify listeners about metadata changes without
  coupling producers to consumers.
- **Global process state** -- core state is exposed through global variables:
  `gMetadata` is a raw pointer (`FilesystemMetadata *`), while
  `gFSOperations`, `gChunkOperations`, `gMetadataBackend`, `gInodeIdGenerator`,
  and `gChunkIdGenerator` are global `std::unique_ptr`s. They are initialized
  during startup and then used process-wide.
- **Conditional compilation** -- `METARESTORE` and `METALOGGER` preprocessor
  guards exclude master-only or tool-only code paths, allowing the same source
  files to be linked into different binaries.
- **Incremental stat propagation** -- directory `StatsRecord` values are
  maintained incrementally on every mutation and propagated up to root,
  avoiding expensive tree traversals for `dirinfo` queries.
- **Cooperative multitasking** -- the master runs a single-threaded event loop.
  Long operations (snapshots, recursive removes, checksum recalculation) are
  split into small batches via `TaskManager` and `eventloop_make_next_poll_nonblocking()`
  to maintain responsiveness.
