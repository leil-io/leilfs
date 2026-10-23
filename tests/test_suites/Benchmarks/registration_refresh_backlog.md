# Registration refresh backlog benchmark

`Benchmarks.test_registration_refresh_backlog` measures failover responsiveness
while version-changing writes collide with delayed disk-state announcements.
It exercises the refresh scheduler, rather than assuming ordinary failover
traffic necessarily creates pending refreshes.

The benchmark uses five chunkservers (configurable to seven), a real EC(3,2)
hot set, and disjoint **goal-1 standard mock chunks** as background inventory.
The background exercises registry size/lookup pressure, but does **not** model
the memory footprint or copy chains of a full EC production cluster. Use native
Linux and representative real inventories for production acceptance measurements.

## Controlled workload

1. Seed background metadata and real EC files, then synchronize a shadow.
2. Keep in-place writes and read-back checks running across master promotion.
3. Finish CS0 registration, but hold CS1 registration credits. Query traffic
   stays live while the cluster inventory remains incomplete.
4. Mark CS0's real disk to-delete and intercept its genuine old-version reports.
5. Overwrite previously untouched EC targets. Only after successful version
   replies does the proxy forward those old reports, unchanged.
6. Once the refresh window fills, resume CS1's paced bulks while refresh replies
   remain held. Either release those replies (`drain`) or disconnect CS0
   (`disconnect`), then await CS1 registration completion.
7. Verify target chunk IDs, file lengths and contents, and report measurements.

Background repair/evacuation is delayed to isolate the refresh workload. Target
writes are first writes since promotion: repeatedly overwriting an already
unlocked chunk does not necessarily increment its version again. No production
profiling code or extra protocol fields are required.

One peer supplies the controlled conflicting reports and another holds its
inventory open. This does not reproduce simultaneous refresh backlogs on all
seven peers; multi-peer work limits/fairness are covered by the scheduler unit
tests. The benchmark requires the source's 128-key query window to fill (or all
source targets if fewer than 128 exist) before taking either backlog action.
CS1 streams during the `backlog` phase and, with sufficiently large inventories,
through the action phase. Small smoke runs can finish that stream earlier;
check its completion log against phase markers before claiming overlap.

## Parameters

In a prepared test environment, for example:

```sh
CHUNK_COUNT=1000000 TARGET_COUNT=1024 REFRESH_ACTION=disconnect \
    REQUIRE_INVENTORY_OVERLAP=1 \
    build/tests/leil-tests --gtest_filter=Benchmarks.test_registration_refresh_backlog
```

Run `drain` and `disconnect` sequentially so their latency measurements do not
compete for CPU/memory. Start with smaller inventories before increasing scale.

| Environment variable | Default | Meaning |
| --- | ---: | --- |
| `CHUNK_COUNT` | 100000 | Standard mock chunks per server; at least 1000 |
| `CS_COUNT` | 5 | Chunkservers; use 7 for production-like placement |
| `TARGET_COUNT` | 512 | Single-chunk EC target files |
| `WRITERS` | 8 | Concurrent target writers |
| `REGISTRATION_BUDGET` | 50000 | Global inventory chunks/second; must be positive |
| `HOLD_SECONDS` | 3 | Backlog hold and post-action observation intervals |
| `REFRESH_ACTION` | drain | `drain` or `disconnect` |
| `MAX_MASTER_REPLY_MS` | 0 | Optional post-promotion INFO latency limit; 0 reports only |
| `REQUIRE_INVENTORY_OVERLAP` | 0 | Fail if CS1 finishes inventory before the action; use larger backgrounds |

Increase background inventory and target counts independently. At seven servers,
only targets actually placed on CS0 contribute to the controlled refresh load.
The manifest counts these parts, and the test requires matching old reports and
successful version operations before proceeding. Timeout multipliers do not
scale the deliberate hold interval; keep it below the chunkserver timeout.

## Reading the results

- `PROMOTION_SOURCE_REGISTRATION_MS`: promotion to source inventory completion.
- `REFRESH_*_WIRE_MS`: action to fresh replies forwarded, or source stopped.
  This is **not** an internal scheduler completion/destruction timer.
- `PROMOTION_GATE_COMPLETION_MS`: promotion to completion of the held inventory;
  deliberately includes benchmark setup/hold time.
- `INVENTORY_STREAMING_AT_ACTION`: whether CS1 inventory was still unfinished
  at the start of `drain`/`disconnect`; require it for concurrency measurements.
- `WRITES`, `LOAD`: completed write/fsync/read-back operations, errors and maximum
  operation latency, including phases crossed by continuous-load operations.
  Continuous writes use distinct payloads to detect stale read-back data.
  Target writes preserve existing chunk IDs and file sizes. A failover can leave
  an interrupted write's existing 120-second chunk lock to expire; a long client
  operation alone does not establish a master event-loop freeze.
- `PROBE`: INFO round trips/errors by phase and sampled promoted-master peak RSS.
  End-to-end latency includes OS/proxy/virtualization scheduling, not just master
  event-loop work. A 20ms sampling interval can miss shorter stalls.
- `PROXY/STATE`: distinct held/released old reports, held/forwarded reply packets,
  distinct queried IDs held, first/last delivery timestamps (milliseconds since
  proxy start), and fresh to-delete parts observed on the wire. These counters are
  **not** master queue occupancy or exact refresh completion counts.

Correctness assertions always apply. Wall-clock performance is reported unless
an explicit latency limit is supplied. Exact work counts, individual lookup
costs and synchronous destruction require a separate diagnostic build; do not
merge that instrumentation. Neither an emulated run nor extrapolating a small
inventory proves a hard sub-second pause bound at 240M logical chunks.
