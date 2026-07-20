# Mount Client (`leil-mount`) -- Architectural Reference

The `src/mount` module is the client-side runtime for LeilFS. It is
responsible for:

- translating frontend requests (FUSE and C/C++ API) into metadata RPCs to the
  master,
- coordinating chunkserver reads/writes,
- maintaining client-side caches and I/O limiting,
- exposing operational special files (for example `.stats`,
  `.saunafs_tweaks`, `.masterinfo`).

This directory produces the shared `mount` library and multiple frontends:

- `leil-mount` (`src/mount/fuse`) -- FUSE daemon.
- `saunafs-client` / `saunafs-client-cpp` (`src/mount/client`) -- embeddable
  client libraries (when `ENABLE_CLIENT_LIB` is enabled).

## Code Organization

`src/mount` is mostly flat, with frontend subdirectories:

```
src/mount/
|-- *.{h,cc}         # Core mount runtime and data path
|-- fuse/            # FUSE frontend (leil-mount)
|-- client/          # C/C++ API libraries
`-- windows -> ...   # Symlink to the Windows-client repo (populated when building for Windows)
```

Key groups in the top-level directory:

| File / group | Subsystem |
|---|---|
| `sauna_client.*` | High-level client API used by frontends |
| `mastercomm.*` | Master RPC transport, reconnect loop, packet dispatch |
| `masterproxy.*` | Local loopback proxy for tools/meta-facing master access |
| `readdata.*`, `readdata_cache.*`, `readahead_adviser.h`, `chunk_reader.*` | Read path and readahead |
| `writedata.*`, `chunk_writer.*`, `write_cache_block.*` | Write path and chunk write orchestration |
| `chunk_locator.*` | Chunk lock/location helpers shared by both paths (`ReadChunkLocator`, `WriteChunkLocator`, `TruncateWriteChunkLocator`) |
| `direntry_cache.h`, `negative_cache.h`, `symlinkcache.*`, `acl_cache.h`, `group_cache.h` | Metadata and access caches |
| `g_io_limiters.*`, `global_io_limiter.*`, `io_limit_group.*` | Local/global I/O limiting |
| `special_*.cc`, `special_inode.*`, `path_by_inode.h`, `oplog.*`, `tweaks.*` | Special inode virtual files and runtime controls |
| `mount_info.*`, `notification_area_logging.h`, `stats.*` | Mount identity, notifications, stats |

## Runtime Frontends

### FUSE (`sfsmount`)

`src/mount/fuse/main.cc` sets up low-level FUSE ops and starts one of two
modes:

- **normal mode** (`meta = false`): calls `SaunaClient::fs_init(...)`, then
  serves regular filesystem operations (`sfs_fuse.cc`).
- **meta mode** (`meta = true`): initializes only master connection threads and
  metadata-special handlers (`sfs_meta_fuse.cc`) via direct
  `masterproxy_init`/`symlink_cache_init`/`fs_init_master_connection`/`fs_init_threads`
  path, without `SaunaClient::fs_init` and without read/write data-pipeline
  initialization.

`sfs_fuse.cc` adapts `fuse_req_t` to `SaunaClient::Context`, updates
supplementary groups, and translates `RequestException` to `errno`.

### Embedded Client Libraries

`src/mount/client` provides:

- `saunafs-client` (C API wrapper in `saunafs_c_api.*`),
- `saunafs-client-cpp` (`Client` class in `client.*`),
- `saunafsmount_shared` exposing the `SaunaClient`-based ABI.

In this repository, the C API (`saunafs-client`) is used by the
`src/nfs-ganesha` FSAL integration.

`Client` uses `dlopen` and symbol binding (`sauna_client_c_linkage.cc`) to call
the underlying singleton-style mount runtime.

When `Client` instances are created in the same process, instance 1 opens the
installed `libsaunafsmount_shared.so`, while later instances load a temporary
copy from `/tmp` to isolate singleton global state per instance.

## Build-Time Gates and External Consumers

| Component | Build gate | Notes |
|---|---|---|
| `mount` library | always from `src/mount` | Core client runtime shared by all frontends. |
| `leil-mount` | top-level `NOT MINGW` + FUSE3 detected (fatal error if not found) | Added via `add_subdirectory(src/mount/fuse)`. |
| `saunafs-client`, `saunafs-client-cpp`, `saunafsmount_shared` | `ENABLE_CLIENT_LIB=ON` | Unit/integration test mode (`ENABLE_TESTS`) forces this option on. |
| `fsalsaunafs` (NFS-Ganesha FSAL) | `ENABLE_NFS_GANESHA=ON` | Links against `saunafs-client_pic`; `ENABLE_CLIENT_LIB=ON` is required for in-tree builds since `saunafs-client_pic` is only produced when that option is on. |

## Initialization Sequence (Normal Data Mount)

Regular mount startup goes through `SaunaClient::fs_init()` in
`sauna_client.cc`:

1. `socketinit()` + `mycrc32_init()`.
2. `fs_init_master_connection(params)` -- copies init params into internal
   globals, initializes connection/session state; if not delayed init,
   connects/registers to master.
3. `symlink_cache_init(...)`.
4. I/O limits setup (part 1): stores config path tweak, instantiates global
   limiter (`gGlobalIoLimiter()`).
5. Starts master communication threads (`fs_init_threads(...)`) and local
   master proxy (`masterproxy_init()`).
6. I/O limits setup (part 2): instantiates local limiter (`gLocalIoLimiter()`),
   loads limits config (`fsLoadMountIoLimits()`).
7. `read_data_init(...)` (read workers, delayed ops worker, read cache pool).
8. `WriteAlgorithm::write_data_init(...)` (write workers, write cache).
9. `notifications_area_logging_init(...)`.
10. `mount_info_init(...)`.
11. Local client init (`init(...)`):
   - cache/timing knobs,
   - direntry/negative/ACL cache setup,
   - stats pointers and tweak variables,
   - optional Linux malloc-trim background thread.

If `delayed_init` is enabled, step 2 sets `sessionlost = true` and
`sessionid = 0` and returns immediately without connecting. Once
`fs_init_threads` starts, `fs_receive_thread` sees `sessionlost` and takes the
lost-session branch, calling `fs_connect(false)` to establish a new session
(not `fs_reconnect`).

For FUSE **meta mode**, startup is intentionally smaller and bypasses
`SaunaClient::fs_init`: `masterproxy_init()`, `symlink_cache_init()`,
`fs_init_master_connection()`, `fs_init_threads()`.

## Local Master Proxy (`masterproxy.*`)

`masterproxy` opens a loopback listener on `127.0.0.1` and forwards packets to
the active master through `fs_custom(...)`.

One path is handled locally instead of being forwarded: tools registration
(`CLTOMA_FUSE_REGISTER` with `REGISTER_TOOLS`) gets an immediate synthetic
`MATOCL_FUSE_REGISTER` success reply.

`masterproxy_getlocation(...)` rewrites `.masterinfo` endpoint fields to proxy
host/port only when the proxy is active and the master protocol version is new
enough.

## Master Communication (`mastercomm.*`)

`mastercomm` is the control plane between mount and master.

### Request/Reply Correlation

Each calling thread gets a `threc` record (linked list in `threchead`) with:

- unique `packetId`,
- `outputBuffer` / `inputBuffer`,
- condition variable and state flags (`sent`, `received`, `waiting`).

Outgoing packets encode `packetId` as message ID; replies are routed back by
message ID in `fs_receive_thread`.

### Core Threads

- `fs_receive_thread`:
  - maintains connection/reconnect loop,
  - reads packet headers and payloads,
  - first dispatches async packet types via registered `PacketHandler`s,
  - otherwise routes replies to waiting `threc`s.
- `fs_nop_thread`:
  - sends periodic `ANTOAN_NOP`,
  - periodically reports reserved inodes (`CLTOMA_FUSE_RESERVED_INODES`),
  - sends mount-info updates (`cltoma::updateMountInfo`) on tweak/global-change
    events only when connected and supported by current master version,
  - reacts to tweak changes (for example I/O limits reload, TLS config
    reconnect trigger).

### Reconnect and Session Model

On disconnect, `fs_receive_thread` closes socket/TLS and retries:

- reconnection with existing session (`fs_reconnect`) when possible,
- full re-register (`fs_connect`) when session is lost.

Before reconnecting, it marks outstanding sent requests as failed and wakes
waiting callers via their `threc` condition variables.

After successful new-session registration it sends config (`fs_register_config`).

### TLS

If TLS config is set (`tlsConfigFile`), registration path attempts STARTTLS and
TLS handshake (`TlsSession`, `SSL_*`). TLS setup/teardown is integrated with
socket lifecycle (`fs_starttls_connection`, `fs_endtls_connection`, `fs_close`).

### Async Packet Handlers

`mastercomm` supports one handler per packet type
(`fs_register_packet_type_handler`), used by:

- write subsystem (`SAU_MATOCL_UNLOCK_CHUNK_NOTICE`),
- global I/O limiter (`SAU_MATOCL_IOLIMITS_CONFIG`).

## Read Path Architecture

### Core Components

- `ReadRecord` (per open read stream): cache, readahead adviser, pending request
  tracking, refresh state.
- `ReadCache`:
  - intrusive ordered set + LRU list + reserved list,
  - entry refcounting for in-flight readers,
  - global memory accounting (`gReadCacheMaxSize`, `gUsedReadCacheMemory`),
  - reusable entry pool (`ReadCacheEntriesPool`),
  - periodic garbage collection and adaptive expiration-time tuning under memory
    pressure.
- `ReadaheadOperationsManager`:
  - maintains priority queue of scheduled readahead requests,
  - merges cache hits + pending requests + newly scheduled requests.
- `ChunkReader`:
  - locates chunk via `ReadChunkLocator`,
  - chooses best server per chunk part using `globalChunkserverStats`,
  - builds read plan (`ChunkReadPlanner`) and executes it
    (`ReadPlanExecutor`),
  - tracks CRC-failing replicas and retries planning.

### Worker Model

`read_data_init` starts:

- one delayed-ops thread (`read_data_delayed_ops`) for periodic cleanup and
  cache GC,
- N read worker threads (`read_worker`) consuming readahead requests.

`read_worker` executes `read_to_buffer`, updates entry state, and notifies
waiting readers through per-request condition variables.

The explicit prefetch path (`read_prefetch`) exists but is dormant:
`gUsingReadPrefetch` is initialized to `false` and no in-tree path sets it to
`true`, so `read_prefetch` is never called in practice.

### Read Flow (`SaunaClient::read`)

1. Enforce local/global I/O limiter waits.
2. If file handle is in write-only mode (`IO_WRITEONLY`), reject with `EACCES`.
3. If file handle is in read-write mode (`IO_WRITE`), flush write data first
   and transition to read mode.
4. Ensure file handle has active `ReadRecord`.
5. Flush pending writes for same inode from other handles
   (`write_data_flush_inode`).
6. Align to block boundaries and call `read_data(...)`.
7. `read_data`:
   - direct read path (no readahead) when cache disabled or adviser says random,
   - otherwise schedules/awaits readahead requests via
     `ReadaheadOperationsManager`.

## Write Path Architecture

The write path uses the following shared building blocks:

- `WriteCacheBlock` -- cached block fragment metadata + payload.
- `WriteChunkLocator` -- `WRITE_CHUNK` lock acquisition and `WRITE_END` unlock.
- `ChunkWriter` -- chunkserver-chain execution, parity handling, operation
  pipelining.

### `ChunkWriter` Responsibilities

`ChunkWriter`:

- creates per-chunk-part `WriteExecutor`s and sends `WRITE_INIT`,
- groups cached blocks into operations by stripe; each operation tracks a
  covered block interval (`minimumModifiedOffset`/`maximumModifiedOffset`) so
  blocks from the same stripe with differing modified ranges can be consolidated
  into one operation, reducing partial-stripe reads,
- prevents overlapping pending operations,
- for partial-stripe writes:
  - reads missing blocks (`readBlocks`) using the standard read planner/executor
    (same connector used by the read path),
  - computes parity (`computeParityBlock`) for XOR/EC goals,
- polls chunkserver sockets + local pipe wakeup,
- processes write statuses, updates file length, and retires journal entries.

### Write Algorithm

`namespace WriteAlgorithm` in `writedata.cc` implements the chunk-level write
scheduling:

- inode holds multiple `ChunkData` nodes (`chunkDataList`), one per chunk index,
- each chunk has its own data chain and queue state,
- job queue item is chunk-level, allowing independent scheduling/retries per
  chunk of the same inode,
- a `pendingChunkData` deque enforces the
  `sfsmaxchunkswritteninparallelperinode` limit (0 = unlimited); chunks beyond
  the limit are queued and promoted as active chunks complete.

### Flush and Truncate Semantics

- `write_data_flush` waits until pending queued work for the file/inode is
  drained.
- `write_data_truncate`:
  1. flushes outstanding writes,
  2. calls `fs_truncate(...)`,
  3. when master asks client-side zero-fill for EC/XOR consistency, writes zero
     range using `TruncateWriteChunkLocator`,
  4. finalizes with `fs_truncateend(...)`.

`fs_truncate(...)` is retried for lock/contention/transient statuses
(`LOCKED`, `CHUNKLOST`, `NOTDONE`) with backoff before deciding failure.

### Unlock Notices and Retry Acceleration

The write subsystem registers a handler for `SAU_MATOCL_UNLOCK_CHUNK_NOTICE`.
The delayed-queue worker consumes these notifications and, for each
`(inode, chunkIndex)` pair, either:

- immediately re-enqueues the matching delayed `ChunkData` job, or
- sets its `recentlyUnlockNoticeReceived` flag so the currently running worker
  skips its normal retry delay on the next failure path.

## Local Caching and State

### Directory/Lookup Caches

- `DirEntryCache`:
  - lookup by `(parent, uid, gid, name)`,
  - readdir-index cache by `(parent, uid, gid, index)`,
  - inode-to-attrs shortcut cache.
- `NegativeCache`:
  - caches ENOENT lookups with timeout and size cap,
  - lets lookup return fast negative entries (`ino=0`, entry timeout).

### Other Caches

- `symlinkcache` -- fixed-size hash-based symlink target cache.
- `AclCache` -- LRU cache for `fs_getacl(...)` results.
- `GroupCache` -- maps supplementary group sets to compact IDs registered in
  master.
- `statfs` cache in `SaunaClient::statfs` (optional timeout-based).

### Disconnect Effects

`masterDisconnectedCallback()` resets:

- group cache,
- direntry cache,
- readdir sessions (marks them for restart logic).

## Special Inodes and Operational Files

Special inode IDs are defined in `common/special_inode_defs.h`. Key files:

- `.masterinfo` -- master/proxy endpoint data.
- `.stats` -- client stats snapshot.
- `.oplog`, `.ophistory` -- operation logs.
- `.saunafs_tweaks` -- runtime tweak inspection/update surface.
- `.saunafs_file_by_inode`, `.saunafs_path_by_inode` -- inode helper views.
- `.saunafs_mount_info` -- mount metadata dump (user/pid/version/options).

Dispatch logic is implemented in `special_lookup.cc`, `special_read.cc`,
`special_write.cc`, and related helpers.

## I/O Limiting and Notifications

- Local limiter: `MountLimiter` (config file driven, in-process).
- Global limiter: `MasterLimiter` (requests limits to master via
  `SAU_CLTOMA_IOLIMIT` and receives config updates asynchronously).
- `SaunaClient::read` / `write` always pass through local and global limiter
  proxies before data I/O.

Notification subsystem (`notification_area_logging.h`) can emit user-facing
messages with suppression windows and inode-to-fullpath expansion.

## Shutdown Sequence

Regular shutdown (`SaunaClient::fs_term()`):

1. `write_data_term()`
2. `read_data_term()`
3. `masterproxy_term()`
4. `::fs_term()` (global-namespace function in `mastercomm.cc`; joins
   receive/nop threads, clears request records, closes socket/TLS)
5. `symlink_cache_term()`
6. `socketrelease()`
7. `notifications_area_logging_term()`

In FUSE meta mode, shutdown is handled in `fuse/main.cc` by terminating
`masterproxy`, `mastercomm`, and symlink cache directly (without full
`SaunaClient::fs_term` path).

## Key Design Patterns

- **Dual transport dispatch** -- message-ID based RPC correlation (`threc`) plus
  packet-type async handlers.
- **Chunk-level write scheduling** -- one `ChunkData` job per chunk index per
  inode, with a configurable parallel-write cap per inode
  (`sfsmaxchunkswritteninparallelperinode`).
- **Producer-consumer workers** -- read/write delayed queues and worker pools for
  I/O overlap.
- **Layered cache stack** -- inode read cache, direntry cache, negative cache,
  symlink cache, ACL cache.
- **Special inodes as control plane** -- operational introspection/control
  exposed as virtual files.
