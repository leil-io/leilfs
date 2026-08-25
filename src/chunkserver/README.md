# Chunkserver (`leil-chunkserver`) -- Architectural Reference

The chunkserver is a storage node in a LeilFS cluster. It stores chunk data on
local disks, serves read/write requests from clients and other chunkservers,
and coordinates with the master server to maintain chunk availability and
replication. The binary produced from this directory is **`leil-chunkserver`**.

Chunks are fixed-size pieces of files (up to 64 MiB, subdivided into 64 KiB
blocks). The chunkserver is responsible for their on-disk lifecycle: creation,
reading, writing, versioning, deletion, integrity verification, and
replication.

## Code Organization

The directory has two layers plus a plugin build area:

```
src/chunkserver/
├── chunkserver-common/     # Core abstractions, interfaces, and shared types
├── plugins/                # CMake entrypoint for plugin subdirs when present
└── *.{h,cc}                # Top-level implementation modules
```

Files in the top-level directory are grouped by subsystem:

| File / group                      | Subsystem                                     |
|-----------------------------------|-----------------------------------------------|
| `init.h`                          | Initialization sequence (`RunTab` tables)     |
| `hddspacemgr.*`                   | Central chunk lifecycle and disk I/O manager  |
| `network_main_thread.*`           | Accept loop; spawns network workers           |
| `network_worker_thread.*`         | Per-worker poll loop; manages connections     |
| `chunkserver_entry.*`             | Per-connection state machine (client I/O)     |
| `masterconn.*`                    | Master connection event-loop glue             |
| `master_connection.*`             | MasterConn class: protocol, registration, ops |
| `bgjobs.*`                        | Background job pool system                    |
| `chunk_high_level_ops.*`          | High-level Read/Write/GetBlocks operations    |
| `chunk_replicator.*`              | Cross-chunkserver chunk replication           |
| `chunk_file_creator.*`            | RAII helper for safe chunk creation           |
| `chunk_filename_parser.*`         | Parse chunk filenames to extract metadata     |
| `io_buffers.*`                    | Aligned I/O buffers for moving data between network and disk        |
| `buffers_pool.h`                  | Thread-safe buffer (`OutputBuffer`, `InputBuffer`, `ChunkCopyBuffer`) recycling pool     |
| `slice_recovery_planner.h`        | EC/XOR slice recovery planning                |
| `g_limiters.*`                    | Singleton for replication bandwidth limiter   |
| `replication_bandwidth_limiter.*` | I/O throttling for replication                |
| `hdd_readahead.*`                 | Read-behind/read-ahead configuration          |
| `network_stats.*`                 | Atomic network I/O statistics counters        |
| `chartsdata.*`                    | Monitoring/charting data collection           |

Files in `chunkserver-common/` provide core abstractions:

| File / group                         | Subsystem                                     |
|--------------------------------------|-----------------------------------------------|
| `chunk_interface.h`                  | `IChunk` -- abstract chunk interface          |
| `disk_interface.h`                   | `IDisk` -- abstract disk interface            |
| `disk_manager_interface.h`           | `IDiskManager` -- disk selection strategy     |
| `chunk_with_fd.*` / `disk_with_fd.*` | File-descriptor-based base classes            |
| `cmr_chunk.*` / `cmr_disk.*`         | CMR (Conventional Magnetic Recording) impls   |
| `disk_chunks.*`                      | Per-disk chunk collection with test tracking  |
| `disk_utils.*`                       | Shared constants, `LockFile`, configuration   |
| `chunk_signature.*`                  | On-disk signature (`SAUC 1.0`)                |
| `block_compression.*`                | Per-block Zstd/LZ4 compression for disk plugins |
| `chunk_map.h`                        | Global `ChunkMap` (chunkId+type -> IChunk)    |
| `subfolder.h`                        | Directory hashing (256 subfolders)            |
| `open_chunk.h`                       | RAII guard for open chunk FDs                 |
| `indexed_resource_pool.h`            | LRU pool for caching open file descriptors    |
| `hdd_stats.*`                        | Atomic per-disk I/O statistics                |
| `hdd_utils.*`                        | Lock/release chunks, report damage            |
| `chunk_trash_manager.*`              | Optional soft-delete to `.trash.bin/`         |
| `global_shared_resources.h`          | Core chunk/disk shared state declarations     |
| `plugin_manager.*`                   | Dynamic `.so` loading via Boost.DLL           |
| `iplugin.h`                          | Root plugin interface                         |
| `disk_plugin.*`                      | Base class for disk plugins                   |
| `default_disk_manager.*`             | Default chunk placement / disk reload policy  |
| `memory_manager.*`                   | Periodic `malloc_trim` for memory management  |
| `iostat.h`                           | Per-disk I/O load from `/proc/diskstats`      |

## Core Data Model

### Chunk Representation

Chunks follow an inheritance hierarchy:

```
IChunk (abstract interface)
  └── FDChunk (file-descriptor based: metaFD, dataFD, version, blocks, state)
        └── CmrChunk (CMR-specific: header layout, rename logic)
```

- **`IChunk`** -- pure virtual interface defining all chunk operations:
  filename generation, header management, block/CRC offsets, state
  transitions, owner disk reference, reference counting, and a condition
  variable for thread synchronization.
- **`FDChunk`** -- file-descriptor-based base class. Stores
  `metaFD_`, `dataFD_`, `id_`, `version_`, `blocks_`, `type_`
  (`ChunkPartType` supporting standard/XOR/EC), `state_`, `refCount_`, and
  `indexInDisk_`.
- **`CmrChunk`** -- built-in chunk implementation for CMR disks. Each chunk is
  stored in two files: `.met` keeps metadata and `.dat` keeps payload data.
  The `.met` file starts with a reserved 1 KiB signature area and then stores
  the per-block CRC table. For standard chunks, that metadata header is 5 KiB.
  For XOR and EC parts, the header is padded up to the device I/O block size.
- **`ChunkSignature`** -- on-disk signature `SAUC 1.0` + chunkId + version +
  chunkType. Virtual to allow plugins to extend.

Chunks are stored in a global **`ChunkMap`**: an `unordered_map` keyed by
`(chunkId, chunkType)` mapping to `unique_ptr<IChunk>`, protected by
`gChunksMapMutex`.

### Chunk States

Chunks transition through the following states:

```
Available -> Locked -> Available    (normal I/O cycle)
Locked -> Deleted                   (normal delete path)
Locked -> ToBeDeleted -> Deleted    (deferred delete path)
```

When a thread wants to lock an already-locked chunk, it waits on the chunk's
`CondVarWithWaitCount`. Condition variables are recycled via `gFreeCondVars` (see
`hddChunkFindOrCreatePlusLock` function).

### Disk Abstraction

Disks follow a parallel inheritance hierarchy:

```
IDisk (abstract interface)
  └── FDDisk (path management, space tracking, lock files, CRC I/O)
        └── CmrDisk (POSIX I/O, subfolder creation, punch holes)
```

- **`IDisk`** -- interface for disk operations: scan state management, chunk
  CRUD, I/O primitives (create, open, pread, write blocks), space tracking,
  and serialization of disk info to the master.
- **`FDDisk`** -- implements path management (metaPath/dataPath), space
  accounting (availableSpace, totalSpace, leaveFreeSpace), lock file creation,
  CRC read/write, and fsync.
- **`CmrDisk`** -- CMR-specific: creates 256 subfolders (`chunks00`–
  `chunksFF`), POSIX I/O via pread/pwrite, punch holes for sparse data, and
  sequential read-ahead via `posix_fadvise`.

### Disk Manager Strategy

```
IDiskManager (interface)
  └── DefaultDiskManager (default placement / reload strategy)
```

- **`IDiskManager`** -- strategy interface: `getDiskForNewChunk()`,
  `getDiskForGC()`, `getChunkToTest()`, `reloadDisksFromCfg()`.
- **`DefaultDiskManager`** -- parses `hdd.cfg`, creates built-in `CmrDisk`
  instances and any plugin-provided disk types, and chooses disks for several
  background tasks. New chunks are placed with a carry-based heuristic derived
  from available space. Garbage collection uses round-robin selection only for
  eligible zoned disks; this applies only to zoned-disk plugins, not to the built-in
  `CmrDisk` path. Periodic CRC testing walks the disk list sequentially.

### Per-Disk Chunk Tracking (`DiskChunks`)

Each disk maintains a `DiskChunks` instance: a vector of `IChunk*` with
**constant-time insert, remove, and next-test selection**. The vector is split
by `firstUntestedChunk_`: entries before that index were already checked in the
current CRC-test pass, and entries from that index onward are still waiting to
be checked. After a disk scan, the chunkserver calls `shuffle()` and resets the
boundary so the next verification pass runs in randomized order.

### Key Global Resources

Core chunk/disk shared state is declared in `global_shared_resources.h`:

- **`gChunksMap`** / **`gChunksMapMutex`** -- the central registry of all chunks.
- **`gDisks`** / **`gDisksMutex`** -- all disk instances.
- **`gDiskManager`** -- the active disk manager.
- **`gOpenChunks`** -- an `IndexedResourcePool<OpenChunk>` for FD caching with
  LRU eviction.
- **`gIoStat`** -- per-disk I/O statistics from `/proc/diskstats`.
- Configuration flags include `gPerformFsync`, `gCheckCrcWhenWriting`,
  `gAdviseNoCache`, and `gPunchHolesInFiles`.

## HDD Space Manager

`hddspacemgr.{h,cc}` is the central module coordinating chunk lifecycle and
disk I/O. Key responsibilities:

- **Initialization** -- `initDiskManager()` creates the `DefaultDiskManager`;
  `loadPlugins()` loads `.so` plugins via `PluginManager`; `hddInit()` reads
  `hdd.cfg`, creates/reloads disks, and prepares scan/trash state;
  `hddLateInit()` spawns background threads that perform scanning/testing.
- **Chunk I/O** -- `hddOpen`/`hddClose`, `hddRead`,
  `hddChunkWriteBlock`/`hddChunkWriteFullBlocks`, `hddPrefetchBlocks`.
- **Buffered-read consistency** -- keeps already-replied write buffers for a
  chunk and overlays their contents onto later reads until the corresponding
  disk writes finish.
- **Chunk Operations** -- `hddInternalCreate`, `hddInternalDelete`,
  `hddInternalUpdateVersion`, `hddDuplicate`, `hddDuplicateTruncate`,
  `hddTruncate`.
- **Reporting** -- `hddGetDamagedChunks`, `hddGetLostChunks`,
  `hddGetNewChunks`, `hddForeachChunkInBulks`, `hddGetTotalSpace`.
- **Registry** -- `hddChunkFindAndLock`, `hddChunkFindOrCreatePlusLock`,
  `hddDeleteChunkFromRegistry`.

### Background Threads

The HDD space manager spawns long-lived background threads:

- **Disk management thread** -- drives disk refresh, scan-state transitions, and
  spawns per-disk scan threads when a disk needs scanning.
- **Per-disk scan threads** -- scan subfolders, read chunk signatures, and
  register chunks in `gChunksMap`.
- **Chunk tester thread** -- performs periodic CRC verification of stored chunks.
- **Free-resources thread** -- releases cached open chunks and old I/O buffers,
  collects trash garbage, and finalizes delayed disk removals.
- **Client-reported test thread** -- re-tests chunks queued after a client
  reports corruption.

## Network Architecture

The chunkserver uses a hybrid network model: the process-wide main event loop
handles the listening socket and other registered components, while a pool of
worker threads runs independent poll-based loops for client chunk I/O.

### Main Network Thread

`network_main_thread.{h,cc}` binds a listening socket and spawns `N`
`NetworkWorkerThread` instances (default 4). It does not run a separate
standalone poll loop; instead, it registers listening-socket callbacks in the
shared event loop. Incoming connections are accepted there and **distributed
round-robin** to workers. This module also reloads runtime settings such as
`WRITE_BUFFERING_SIZE_MB`, read/write job sizing, buffer-pool limits,
replication timeouts, bandwidth limits, and readahead settings, and hosts the
global `ChunkReplicator` instance (`gReplicator`).

### Network Worker Thread

`network_worker_thread.{h,cc}` -- each worker runs a **poll-based event loop**
managing a list of `ChunkserverEntry` connections. Each worker owns a
`ClientJobPool` (background HDD job pool) with a configurable number of HDD
worker threads (default 16 per network worker). The main loop prepares poll
descriptors, calls `poll()`, services I/O events, and processes completed
background jobs. On each loop iteration it also advances buffered writes via
`ChunkserverEntry::everyLoopUpdateWrite()`, which retires completed sealed
writes and gives the newest write operation another chance to fast-reply
buffered blocks.

### ChunkserverEntry (Connection State Machine)

`chunkserver_entry.{h,cc}` implements a **per-connection state machine** (see
`ChunkserverEntry::State`):

```
Idle -> GetBlock -> Idle              (get chunk blocks request sequence)
Idle -> Read                          (client read request)
Idle -> WriteLast                     (write, no forwarding chain)
Idle -> Connecting -> WriteInit -> WriteForward  (write with forwarding)
Idle -> WriteInit -> WriteForward     (write, reused fwd connection)

Read -> Idle                                 (read high level operation success, pool not full)
WriteLast / WriteForward -> Idle             (current write sealed for the client; buffered writes may still drain, pool not full)

(any I/O state) -> IOFinish                              (network error, IO job error, job pool full)
                      |
                      +----> Close                       (after sending pending packets)
                               |
                               +--> CloseWait -> Closed  (if pending jobs)
                               |
                               +--> Closed               (if no pending jobs)
```

A single connection can own multiple `WriteHighLevelOp` instances. The newest
one receives incoming `WRITE_DATA` packets; older ones must already be sealed by
`WRITE_END` yet still own buffered `InputBuffer`s waiting for `job_write()`
completion. Returning to `Idle` therefore means the current write sequence has
finished from the client's perspective, not necessarily that every buffered
block is already on disk.

Each connection operates in one of two modes: `Header` (reading packet header)
or `Data` (reading packet body). For write operations, the chunkserver supports
**write forwarding**: it creates a pipeline to the next chunkserver in the
chain via a `fwdSocket`, forwarding data for high-throughput multi-replica
writes.

The entry contains `ReadHighLevelOp`, `WriteHighLevelOp`, and
`GetBlocksHighLevelOp` for structured I/O management. Network packets use
`PacketStruct` for management and `OutputBuffer` for building read replies without
extra copy of block data from contiguous aligned data retrieved from the disks.

## Master Connection

### Initialization (`masterconn.*`)

`masterconn_init_threads()` creates the shared master `MasterJobPool` (default
10 workers) and the replication job pool (also a `MasterJobPool`, default 5
workers).
`masterconn_init()` creates the singleton `MasterConn` and registers it in the
event loop (reconnect, poll descriptors, serve, reload).

### MasterConn Protocol (`master_connection.*`)

The `MasterConn` class manages the full lifecycle of the connection to the
master server:

- **Connection states** -- `FREE` -> `CONNECTING` -> `HANDSHAKE` (if TLS) ->
  `CONNECTED` -> `KILL` (on error, triggers reconnect).
- **Registration sequence** -- `kUnregistered` -> `kRegistrationRequested` ->
  `kHostRegistered` -> `kChunksRegistered` for modern masters, with an
  old-master compatibility path that skips `kRegistrationRequested`. After host
  registration, the chunkserver bulk-reports chunks and space first, then sends
  label and configuration.
- **TLS support** -- optional TLS handshake on connect.
- **Chunk operations** -- the master dispatches chunk operations to the
  chunkserver: create/delete/version/duplicate/truncate/duplicate-truncate,
  plus lock-augmented variants and replication. Most operations use
  `jobPool_`; replication uses the separate `replicationJobPool_`; `lockChunk`
  / `unlockChunk` manipulate chunk-lock bookkeeping rather than behaving like
  ordinary queued disk jobs.
- **Callbacks** -- `sauJobFinished` / `sauJobFinishedAndLock` send operation
  results back to the master.

## Background Jobs System

`bgjobs.{h,cc}` implements a base job-pool abstraction with two derived pool
types. At runtime the chunkserver uses pools for three reasons: client pools in
network workers, one master pool, and one replication pool. The listener (network
workers, main network thread) wakeup is pipe-based and job dispatch uses a
producer-consumer queue:

```
JobPool (base: thread pool + PCQueue)
  ├── MasterJobPool (master operations + chunk lock coordination)
  └── ClientJobPool (client I/O + read/write priority switching)
```

- **`JobPool`** -- core thread pool with priority queues. Jobs have a
  `JobCallback` and `ProcessJobCallback`. Status notifications use pipe FDs.
  Supports: `addJob`, `disableJob`, `changeCallback`,
  `processCompletedJobs`.
- **`MasterJobPool`** -- extends `JobPool` with **chunk locking**:
  `startChunkLock`, `enforceChunkLock`, `endChunkLock`, `eraseChunkLock`.
  Lock-sensitive operations (Delete, ChangeVersion, Duplicate, Truncate,
  DuplicateTruncate) are deferred when the chunk is locked by a client write.
  Create/Replicate jobs are queued normally. When chunkserver-side write
  buffering fast-replies a write, the lock remains held until close/metadata
  sync so late disk failures can still be reported back through
  `endChunkLock()`.
- **`ClientJobPool`** -- extends `JobPool` with **I/O priority**: supports
  `Fifo` and `Switch` modes. In `Switch` mode, it alternates between
  preferring read and write jobs to prevent starvation.

Convenience functions (`job_open`, `job_close`, `job_read`, `job_write`,
`job_replicate`, `job_create`, ...) wrap the pool API for each specific operation.

## High-Level Chunk Operations

`chunk_high_level_ops.{h,cc}` defines structured I/O operations that tie
multi-step work to a `ChunkserverEntry`:

```
HighLevelOp (base)
  ├── GetBlocksHighLevelOp  -- retrieve block count
  ├── ReadHighLevelOp       -- multi-block parallel reads
  └── WriteHighLevelOp      -- multi-block writes with forwarding
```

- **`ReadHighLevelOp`** -- supports **parallel HDD read jobs** (configurable
  `maxParallelHddReadJobs` and `maxBlocksPerHddReadJob`), issuing concurrent
  reads until all requested data is served.
- **`WriteHighLevelOp`** -- manages the full write lifecycle: starts with a
  `job_open()`, collects `WRITE_DATA` blocks into pooled `InputBuffer`s,
  forwards them if needed, batches them into `job_write()` calls, and closes
  the chunk afterward. With write buffering enabled, it may fast-reply
  non-standard slices once the data is buffered in memory, the chunk lock is
  active, and global buffer budget is available. After a successful
  `WRITE_END`, the operation becomes sealed; it may still exist until all
  buffered writes finish and the lock can be released.

All operations track `pendingDelayedJobs_` and support `delayedClose()` for
clean resource release. The base class also keeps track of the parent
`ChunkserverEntry` instance to apply state changes, enqueue packets and access
its worker pool.

### Chunkserver-Side Write Buffering

When `WRITE_BUFFERING_SIZE_MB` is nonzero, `WriteHighLevelOp` may acknowledge
some `WRITE_DATA` packets for non-standard slices before the corresponding disk
write finishes. This fast-reply path is only used after the initial
`job_open()` completes, while chunkserver-side chunk locking is active
(`USE_CHUNKSERVER_SIDE_CHUNK_LOCK` on the master), and only if enough global
write-buffer budget remains available.

Acknowledged-but-not-yet-flushed data stays in `InputBuffer`s owned by the
sealed write operation. `hddspacemgr` keeps references to those buffers so
later overlapping reads can be patched from memory until `job_write()`
finishes. Deferred write errors are not sent back to the client after an early
success reply; instead, the final status is carried to the master when the
chunk lock is released after close and metadata sync.

## Replication Subsystem

### ChunkReplicator

`chunk_replicator.{h,cc}` -- connects to source chunkservers via
`ChunkConnector` and replicates chunk data. The `replicate()` method takes a
`ChunkFileCreator` and a list of `ChunkTypeWithAddress` sources. It performs
direct read requests to other chunkservers. For erasure-coded chunks, it uses
`SliceRecoveryPlanner`. The global instance is `gReplicator`.

### ChunkFileCreator

`chunk_file_creator.{h,cc}` -- RAII helper for safe chunk creation:
`create()` -> `write()` -> `commit()`. If `commit()` is never called, the
destructor deletes the partially-created chunk, ensuring no corrupt data
remains on disk.

### SliceRecoveryPlanner

`slice_recovery_planner.h` -- plans recovery of EC/XOR slice parts using three
strategies:

1. Direct read of a matching data or parity part.
2. Read whole chunk data via `ChunkReadPlanner` + `BlockConverter`.
3. Parity recovery using XOR/EC parity calculators.

### Bandwidth Limiting

- **`ReplicationBandwidthLimiter`** -- wraps `ioLimiting::Group` to throttle
  replication I/O. Configured via `REPLICATION_BANDWIDTH_LIMIT_KBPS`.
- **`g_limiters.h`** -- singleton accessor: `replicationBandwidthLimiter()`.

## Plugin System

The chunkserver supports dynamically-loaded disk plugins for non-standard
storage hardware.

### Plugin Hierarchy

```
IPlugin (root interface: name, version, initialize)
  └── DiskPlugin (extends: prefix, createDisk, cleanup)
```

- **`IPlugin`** (`iplugin.h`) -- root interface.
- **`DiskPlugin`** (`disk_plugin.h`) -- extends with: `prefix()`,
  `createDisk()`, `cleanup()`. Version-checked against `SAUNAFS_VERSHEX`.
- **`PluginManager`** (`plugin_manager.h`) -- loads `.so` files from a
  configured directory via `boost::dll::import_alias`. `hdd.cfg` lines with a
  recognized prefix are delegated to the matching disk plugin.

## I/O Subsystem

### Buffers and Output

- **`Buffer<T>`** (`io_buffers.h`) -- generic buffer with padding support,
  copy-in/copy-out, and read/write from FDs and chunks.
- **`OutputBuffer`** (`io_buffers.h`) -- specialized for network writes. Three
  sections: Header, CRC, Block. Block data is aligned to 4 KiB
  (`disk::kIoBlockSize`). It assembles per-block header+CRC+payload into the
  aligned block buffer and writes it out block-by-block. Pool-recycled via
  `BuffersPool`.
- **`InputBuffer`** (`io_buffers.h`) -- specialized for inbound `WRITE_DATA`
  traffic. It stores forwarded packet headers in a header buffer, payload data
  in an aligned block buffer, and per-write metadata (`WriteInfo` + CRCs)
  alongside them. `getWriteOperations()` merges contiguous full-block writes
  into fewer disk operations for `job_write()`. When fast replies are used, the
  same buffer also acts as the chunkserver-side write cache: `repliedBlocks`
  tracks which writes were already acknowledged, and `hddspacemgr` may use the
  buffered data to patch later reads until the disk write completes.
  Pool-recycled via `BuffersPool`.
- **`ChunkCopyBuffer`** (`io_buffers.h`) -- specialized for the paths that stage whole
  chunk blocks while copying them: replication and local chunk duplication. Contains only
  a block buffer aligned to 4 KiB (`disk::kIoBlockSize`). Pool-recycled via `BuffersPool`.
- **`BuffersPool<T>`** (`buffers_pool.h`) -- thread-safe pool of
  `OutputBuffer`, `InputBuffer` and `ChunkCopyBuffer` objects, keyed by
  `(headerSize, numBlocks)`. Auto-creates new buffers when the pool is empty, recycles
  after use, and supports TTL-based expiration via `releaseOldBuffers()`.

### Open Chunk Pool

- **`IndexedResourcePool<OpenChunk>`** (`indexed_resource_pool.h`) -- LRU pool
  for caching open file descriptors. Acquire/release by integer ID; stale
  resources are purged after a configurable threshold (default 4 s). Global
  instance: `gOpenChunks`.
- **`OpenChunk`** (`open_chunk.h`) -- RAII wrapper: on destruction, closes
  metaFD/dataFD, releases the chunk via `hddChunkRelease()`, and reports close
  errors as chunk damage.

### Readahead

- **`HDDReadAhead`** (`hdd_readahead.h`) -- configures read-behind and
  read-ahead in block units. Global instance: `gHDDReadAhead`.

### I/O Statistics

- **`IoStat`** (`iostat.h`) -- Linux-only: reads `/proc/diskstats` to
  calculate a load factor for configured disk paths. The chunkserver uses this
  for optional status reporting to the master (`ENABLE_LOAD_FACTOR`), not for
  chunk placement.
- **`HddStats`** (`hdd_stats.h`) -- atomic per-disk I/O counters.
- **`network_stats.*`** -- atomic network I/O counters.

## Trash Manager

When `CHUNK_TRASH_ENABLED` is enabled, deleted chunks are soft-deleted to a
`.trash.bin/` directory. Otherwise, `CmrDisk::unlinkChunk()` unlinks the files
immediately:

- **`ChunkTrashManager`** (`chunk_trash_manager.h`) -- static class with pImpl
  pattern. Moves deleted chunk files to `.trash.bin/`.
- **`ChunkTrashManagerImpl`** (`chunk_trash_manager_impl.h`) -- implements
  timestamped file naming, configurable time-to-live, bulk garbage collection
  (expired file removal), and emergency space recovery when disk space is low.

## Initialization Sequence

Startup is orchestrated by ordered `RunTab` arrays in `init.h`. The sequence
is dependency-ordered:

```
 Normal run:
  1. rnd_init                -- Random number generator
  2. MemoryManager::init     -- Periodic malloc_trim thread
  3. initDiskManager         -- Create DefaultDiskManager (before plugins)
  4. loadPlugins             -- Load .so plugins via PluginManager
  5. hddInit                 -- Parse hdd.cfg, create/reload disks, prepare scan state
  6. mainNetworkThreadInit   -- Bind listen socket (before masterconn)
  7. masterconn_init_threads -- Create master MasterJobPool + replication job pool (MasterJobPool)
  8. masterconn_init         -- Create MasterConn, register event loop hooks
  9. chartsdata_init         -- Charts/monitoring initialization

 Late run (after init):
 10. hddLateInit             -- Spawn disk-management, tester, free-resource, and
                                client-reported-test threads
 11. mainNetworkThreadInitThreads -- Spawn network worker threads
```

Client connections are accepted only after all subsystems are ready.

## Shutdown sequence

When the termination signal is received, the eventloop moves through some
stages before ending the process:

1. **WantExit** -- `mainNetworkThreadWantExit()` closes the listening socket,
   calls `NetworkWorkerThread::askForTermination()` on every worker (stops
   accepting new data, drains in-flight replies), and sets `gDoTerminate`.
2. **CanExit** -- polled each loop tick via `mainNetworkThreadCanExit()`.
   Returns `true` when all workers have emptied their `csservEntries` and
   `bgJobPool_` (or the 30 s forceful-termination timeout fires), **and**
   both master job pools are drained (`masterconn_canexit()`). Workers
   are checked first to avoid a race with in-flight `endChunkLock` replies.
3. **Destruct** -- `eventloop_destruct()` runs registered destructors in
   reverse order: `mainNetworkThreadTerm()` joins all worker threads;
   `hddTerminate()` joins background threads, flushes dirty CRCs, and clears
   `gChunksMap`/`gDisks`; `masterconn_term()` closes the master socket and
   resets the job pools.

## Key Design Patterns

- **Interface-based extensibility** -- all major abstractions are behind pure
  virtual interfaces (`IChunk`, `IDisk`, `IDiskManager`, `IPlugin`), enabling
  alternative implementations to be plugged in.
- **Plugin architecture** -- `PluginManager` + `DiskPlugin` +
  `boost::dll` for runtime `.so` loading. Disks with non-standard storage are
  introduced without modifying core code.
- **Producer-consumer threading** -- `JobPool` uses `PCQueue` with pipe-based
  wakeup for background job processing. Runtime uses client pools in network
  workers plus dedicated master and replication pool instances.
- **State machine** -- `ChunkserverEntry` manages per-connection state
  (`Idle` -> `Read`/`Write*`/forwarding states -> shutdown states); `MasterConn`
  manages connection and registration state machines independently.
- **RAII resource management** -- `OpenChunk` guards FD lifecycle;
  `ChunkFileCreator` ensures incomplete replications are cleaned up;
  `LockFile` prevents concurrent disk access.
- **Object pooling** -- `BuffersPool<T>` recycles memory-efficient
  I/O buffers; `IndexedResourcePool<OpenChunk>` caches open file descriptors
  with LRU eviction.
- **Strategy pattern** -- `IDiskManager` allows pluggable chunk placement
  strategies (currently `DefaultDiskManager` with carry/space-based placement).
- **Write forwarding pipeline** -- write requests form a chain across
  chunkservers (`Client -> CS1 -> CS2 -> ...`) via `fwdSocket`, enabling
  pipelined multi-replica writes.
- **Shared locking** -- `gChunksMapMutex` guards the chunk registry, per-chunk
  `CondVarWithWaitCount` objects coordinate chunk waiters, `gDisksMutex`
  protects disk state, and `gMasterReportsLock` protects the damaged/lost/new
  chunk report queues.
- **Global process state** -- core state is exposed through global variables
  in `global_shared_resources.h`: `gChunksMap`, `gDisks`, `gDiskManager`,
  `gOpenChunks`, etc. Initialized during startup and used process-wide.
