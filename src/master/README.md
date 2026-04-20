# Master Server (`sfsmaster`) -- Architectural Reference

The master server is the central metadata authority in a SaunaFS cluster. It
maintains the entire filesystem namespace (inodes, directories, edges, xattrs,
ACLs, quotas, locks) in memory and coordinates chunk placement across
chunkservers. The binary produced from this directory is **`sfsmaster`**.

A single codebase supports two runtime **personalities**:

- **Master** -- the active metadata server that accepts client mutations.
- **Shadow** -- a hot-standby that replays changelogs from the master and can
  be promoted at any time.

The personality is selected at startup via configuration and can only transition
from Shadow to Master (never the reverse). Conditional compilation guards
(`METARESTORE`, `METALOGGER`) also allow parts of this code to be linked into
the `sfsmetarestore` and `sfsmetalogger` binaries.

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
| `kv_*`                | KV store (FoundationDB) integration              |
| `task_manager*`       | Async task execution framework                   |
| `locks*`              | POSIX and flock advisory file locking            |
| `acl_storage*`        | Deduplicated RichACL storage                     |
| `exports*`            | Client mount permissions and IP-based rules      |
| `topology*`           | Network topology for data-locality placement     |
| `personality*`        | Master / Shadow personality management           |
| `chartsdata*`         | Monitoring / charting data collection            |

A key organizational convention is the **interface / implementation split**:
public interfaces live in `*_interface.h` files as pure virtual classes (e.g.
`IFilesystemOperations`, `IFilesystemNodeOperations`, `IMetadataBackend`,
`IKVConnector`), while default in-memory implementations use a `*Base` suffix
or reside in the corresponding `.cc` file.

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

Client session state is split across a manager-owned in-memory view and a
metadata-backed per-file view:

- the session manager owns `Session` objects and handles creation, lookup,
  timeout, persistence, and startup reconstruction,
- each `Session` keeps mount-level session state plus an in-memory set of
  currently open file inodes,
- each `FSNodeFile` keeps the reverse mapping, `sessionIds`, inside filesystem
  metadata.

This split is intentional: some code needs to answer "what files does this
session hold open?", while other code needs to answer "what sessions still
hold this file open?". The manager layer isolates lifecycle logic from
persistence, so different backends can keep the same session semantics while
storing the durable state differently.

The manager is attached during startup:

- The master init sequence attaches the session manager before session state is
  loaded.

`matoclserv_sessions.cc` is a thin dispatcher layer: session-related wrappers
forward to the active session manager with `sassert(...)` guards. Any
dispatcher call before the backend is attached fails an assertion instead of
falling back silently.

### Session Data Model

`Session` represents one client mount or meta-session known to the master.
Its durable data is the information needed to recognize and restore a
reconnectable mount: session identity, peer identity, mount root and access
limits, UID/GID remapping, and hourly operation stats. Runtime-only state
includes live connection state, transient connection details, caches, and the
in-memory open-file set.

`newSession` is not serialized directly. Instead, persistence treats
reconnectable sessions as the durable subset, and loaded sessions are
recreated in that reconnectable state.

The lifecycle-related fields have distinct roles:

- `newSession` acts as a small state flag used both to decide whether the
  session is persisted and to select timeout policy.
- `connections` is the in-memory connection counter updated by registration,
  reconnect, and disconnect handling.
- `disconnectedTimestamp` is cleared on reconnect/use, set when the last
  connection drops, and initialized to `eventloop_time()` for sessions restored
  from `sessions.sfs`.

### Session ID Allocation and Changelog

Session IDs come from a metadata-owned counter rather than from `sessions.sfs`.
Allocating a new session consumes the current counter value, records the
allocation in the changelog, and advances the counter. Because the same
logical event is replayed during restore/shadow processing, session ID
assignment stays consistent across snapshots and changelog replay.

### Client Lifecycle

The client lifecycle is:

- **Create** -- Reconnectable sessions are allocated, filled with their stored
  identity and limits, and persisted.
- **Reconnect** -- An existing session record is reused, its disconnected
  state is cleared, and its connection count is incremented.
- **Disconnect** -- The connection count is decremented; once the last
  connection drops, the session becomes eligible for timeout-based cleanup.
- **Explicit delete / timeout cleanup** -- Both trigger teardown of the
  session's open files and locks. Timeout cleanup also reaps expired session
  records.

The session file is not rewritten on every open or close. It is updated when
reconnectable sessions are created and when periodic stats persistence runs.

### Two Open-File Indexes

The implementation keeps two synchronized indexes because different subsystems
need opposite lookup directions:

- `Session::openFilesSet` is the in-memory, per-session view of currently open
  files.
- `FSNodeFile::sessionIds` is the durable, per-file view of which sessions
  still hold that file open.

Normal open, close, and session-teardown paths update both sides. In the
normal master open path, file-side membership is updated first and the
in-memory session set is updated only on success. The file-side index is the
durable one: it survives restart, drives reserved/trash file lifetime, and is
used to rebuild the in-memory per-session view during startup.

### `acquire()` / `release()` Semantics

At the file-operation level, acquire adds session membership to the file node,
while release removes it. If a reserved file no longer has any recorded
openers, it can be deleted immediately. Lock cleanup is tied to session
teardown rather than to a standalone release.

Because reserved-file deletion is keyed off `FSNodeFile::sessionIds.empty()`,
reserved-file lifetime depends on the file-side index, not on the session file
alone.

### Persistence and Restart

The current master path spreads durable session-related state across two
stores:

- **`metadata.sfs`** stores `gMetadata->nextSessionId()` and the per-file
  `FSNodeFile::sessionIds` vectors.
- **`sessions.sfs`** stores reconnectable session metadata and hourly stats.

`sessions.sfs` intentionally does **not** store:

- `openFilesSet`
- lock state
- `connections`
- `disconnectedTimestamp`
- `groupsCache`
- file-side `sessionIds`
- transient connection details such as the current live socket attachment

Startup reconstruction happens in two stages:

1. Reconnectable sessions are loaded from `sessions.sfs` and start in a
   disconnected state.
2. Metadata loading rebuilds each session's in-memory open-file set from the
   persisted file-side opener membership, creating compatibility-only
   synthetic sessions if file-side membership exists without a matching session
   record.

This split answers the key restart questions:

- **How does reconnect survive restart?**
  Session identity and mount-level access limits come back from `sessions.sfs`.
- **How does open-file state survive restart?**
  File opener membership comes back from `FSNodeFile::sessionIds` in metadata.
- **Why do reserved files depend on session state?**
  Reserved/trash lifecycle logic consults the file-side `sessionIds` vector to
  decide whether a detached file must stay alive.

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
  Used by shadow masters during synchronization and by `sfsmetarestore` for
  offline recovery.

## KV Store Backend (FoundationDB)

An alternative to file-based metadata storage, designed for distributed
metadata:

- **`IKVConnector`** (`kv_connector_interface.h`) -- interface for KV store
  operations and event handlers for metadata changes (`onNodeChanged`,
  `onEdgeChanged`, `onEdgeRemoved`, `onDetainedAdded`, etc.).
- **`kv_connector_fdb.*`** -- FoundationDB concrete implementation
  (conditionally compiled). This integration is currently not wired into the
  default master initialization path in `init.h`.
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

| Operation | Schedule | Purpose |
|-----------|----------|---------|
| **File integrity test** (`fs_periodic_file_test` / `fs_background_file_test`) | Every second (timer) + every loop (background) | Scans the entire node hash table in a configurable cycle time (`FILE_TEST_LOOP_MIN_TIME`, default 3600s). For each file node, checks chunk availability and copy counts. For each directory, validates parent-child pointer consistency. Builds a `gDefectiveNodes` map of inodes with unavailable chunks, under-goal chunks, or structural errors. |
| **Background task processing** (`fs_background_task_manager_work`) | Every loop | Drives the `TaskManager`, processing a batch of tasks (snapshots, recursive removes, goal/trashtime changes) per iteration. |
| **Checksum recalculation** (`fs_background_checksum_recalculation_a_bit`) | Every loop | Incrementally recalculates metadata checksums (nodes, xattrs, chunks) in the background, progressing through steps at a speed limit per iteration. |
| **Trash cleanup** (`fs_periodic_emptytrash`) | Every 100ms | Purges expired trash entries whose deletion timestamp has passed. |
| **Reserved file cleanup** (`fs_periodic_emptyreserved`) | Configurable period | Releases reserved files (deleted-but-still-open files) whose sessions are no longer active. |
| **Chunk maintenance** (in `chunks.cc`) | Periodic | Handles chunk replication, deletion of excess copies, and rebalancing across chunkservers. |

## Initialization Sequence

Startup is orchestrated by ordered `RunTab` arrays in `init.h`. The sequence
is dependency-ordered -- comments in the source mark critical orderings:

```
1.  prometheus_init          -- Optional Prometheus metrics endpoint
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
  `gFSOperations`, `gMetadataBackend`, `gInodeIdGenerator`, and
  `gChunkIdGenerator` are global `std::unique_ptr`s. They are initialized
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
