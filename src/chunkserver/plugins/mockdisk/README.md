# Mock disk plugin

Test-only disk plugin which fakes a configurable number of chunks **without
creating any chunk file**. Built to exercise chunk registration, shadow
promotion and read paths at scales (hundreds of millions of chunks) where
creating real files is not feasible. Never use it in production.

## hdd.cfg line

```
mock:<chunkCount>:<firstChunkId>:<path>
```

- `chunkCount` — number of fake chunks the disk hosts.
- `firstChunkId` — id of the first chunk; ids are sequential. Use `1` to match
  metadata seeded with `generate_mock_chunks_changelog` (tests/tools/metadata.sh).
- `path` — a real directory (created if missing); only lock files and trash
  bookkeeping live there.

## Behavior

- The disk scan synthesizes `chunkCount` chunks (version 1, standard type)
  directly into the in-memory registry via `IDisk::scanSyntheticChunks`
  (see `hddScanSyntheticChunks` in `src/chunkserver/hddspacemgr.cc`).
- Every chunk is full (1024 blocks) and every block returns the same static
  pattern: `byte[i] = (i & 0xFF) ^ 0x5A`, with a valid CRC. Reads through the
  whole stack (chunkserver → mount) succeed.
- Chunk file descriptors point to `/dev/null`; writes are accepted and
  discarded. The disk is never selectable for new chunks and reports zero
  available space.

## Memory budget

~150–180 bytes of chunkserver RSS per fake chunk (registry map node + chunk
object + disk bookkeeping):

| chunks | chunkserver RAM |
|--------|-----------------|
| 1M     | ~0.2 GB         |
| 5M     | ~0.9 GB         |
| 200M   | ~32–36 GB       |

The master additionally holds its own chunk entries for the seeded metadata,
in the same order of magnitude. Plan manual 100–200M benchmark runs on
machines sized accordingly (see
`tests/test_suites/Benchmarks/test_chunk_registration_benchmark.sh`).
