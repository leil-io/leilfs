#!/usr/bin/env python3
"""External workloads/measurements for the registration refresh benchmark.

Use fixed-size, in-place writes: truncating/replacing files would create new
chunk IDs instead of exercising version changes on existing EC parts. The
manifest and read-back checks detect that shortcut. Master INFO round trips
include scheduling/network delays; they are not handler-only pause timings.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

from registration_reordering_proxy import receive_packet


# Keep each file within one chunk; later writes replace only its first block.
SIZE = 128 * 1024
BLOCK = 4096


def payload(index, changed, iteration=0):
    return f"{'new' if changed else 'old'}-{index:08d}-{iteration:012d}\n".encode().ljust(BLOCK, b".")


def filename(directory, index):
    return directory / f"file_{index:06d}"


def chunk_ids(paths, require_full_ec=False):
    ids = []

    # Batch CLI calls: per-file shell assertions dominate setup at large counts.
    for offset in range(0, len(paths), 128):
        batch = paths[offset:offset + 128]
        output = subprocess.check_output(["saunafs", "fileinfo", *map(str, batch)], text=True)
        chunks = re.findall(r"chunk \d+:\s+([0-9A-Fa-f]{16})_[0-9A-Fa-f]{8}", output)
        if len(chunks) != len(batch):
            raise RuntimeError(f"expected one chunk per file: {output}")

        # Require all five parts before the race. Final verification also runs
        # after a source disconnect, when fewer parts can legitimately be visible.
        parts = re.findall(r"(?m)^\s+copy \d+:.*part \d/5 of ec\(3,2\)", output)
        if require_full_ec and len(parts) != 5 * len(batch):
            raise RuntimeError("target inventory must contain all five EC parts per file")

        ids.extend(int(chunk, 16) for chunk in chunks)

    return ids


def write_file(path, index, create=False, iteration=0):
    # Only seeding creates/resizes files; overwrites must retain the existing chunk ID.
    flags = os.O_RDWR | (os.O_CREAT | os.O_EXCL if create else 0)
    with os.fdopen(os.open(path, flags, 0o644), "r+b", buffering=0) as stream:
        if create:
            stream.truncate(SIZE)

        expected = payload(index, not create, iteration)
        if os.pwrite(stream.fileno(), expected, 0) != BLOCK:
            raise RuntimeError("short write")

        os.fsync(stream.fileno())

        if (os.pread(stream.fileno(), BLOCK, 0) != expected
                or os.fstat(stream.fileno()).st_size != SIZE):
            raise RuntimeError(f"read-back/length mismatch: {path}")


def load(args):
    def worker(worker_id):
        metrics = dict(
            operations=0,
            errors=0,
            max_latency_ms=0,
            phases={},
        )

        while True:
            # Workers own disjoint file indices, avoiding same-file writer contention.
            for index in range(worker_id, args.count, args.workers):
                if args.stop and args.stop.exists():
                    return metrics

                before = args.phase.read_text().strip() if args.phase else "all"
                start = time.monotonic()
                failed = False

                try:
                    # Vary every continuous overwrite: reading an earlier
                    # successful iteration must not satisfy the read-back check.
                    iteration = metrics["operations"] if args.stop else 0
                    write_file(
                        filename(args.directory, index), index, args.action == "seed", iteration
                    )
                except (OSError, RuntimeError) as error:
                    metrics["errors"] += 1
                    failed = True
                    print(str(error), flush=True)

                metrics["operations"] += 1
                elapsed = (time.monotonic() - start) * 1000
                metrics["max_latency_ms"] = max(metrics["max_latency_ms"], elapsed)

                # Preserve phase crossings so a long failover wait is not attributed
                # entirely to the phase in which the operation eventually finishes.
                after = args.phase.read_text().strip() if args.phase else before
                phase = before if before == after else f"{before}->{after}"
                bucket = metrics["phases"].setdefault(
                    phase, dict(operations=0, errors=0, max_latency_ms=0)
                )
                bucket["operations"] += 1
                bucket["errors"] += int(failed)
                bucket["max_latency_ms"] = max(bucket["max_latency_ms"], elapsed)

            # Without --stop, seed/write runs once; otherwise repeat until the marker appears.
            if args.stop is None:
                return metrics

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(worker, range(args.workers)))

    # Each worker owns its metrics. Merge after joining, using maxima rather than
    # averages so one stalled client operation remains visible in the report.
    result = dict(
        operations=sum(item["operations"] for item in results),
        errors=sum(item["errors"] for item in results),
        max_latency_ms=max(item["max_latency_ms"] for item in results),
        elapsed_ms=(time.monotonic() - start) * 1000,
        phases={},
    )

    for worker_result in results:
        for phase, values in worker_result["phases"].items():
            bucket = result["phases"].setdefault(
                phase, dict(operations=0, errors=0, max_latency_ms=0)
            )
            bucket["operations"] += values["operations"]
            bucket["errors"] += values["errors"]
            bucket["max_latency_ms"] = max(bucket["max_latency_ms"], values["max_latency_ms"])

    args.output.write_text(json.dumps(result) + "\n")
    print(json.dumps(result), flush=True)

    if result["errors"]:
        raise SystemExit(1)


def probe(args):
    phases = {}
    peak_rss = 0

    while not args.stop.exists():
        phase = args.phase.read_text().strip()
        metrics = phases.setdefault(phase, dict(operations=0, errors=0, max_latency_ms=0))

        # Measure connection setup plus the INFO round trip, not an internal callback.
        start = time.monotonic()
        try:
            with socket.create_connection((args.host, args.port), timeout=2) as connection:
                connection.sendall(struct.pack("!II", 510, 0))
                reply = receive_packet(connection)
                if reply is None or reply[0] != 511:
                    raise RuntimeError("invalid master INFO reply")
        except (OSError, RuntimeError):
            metrics["errors"] += 1

        metrics["operations"] += 1
        metrics["max_latency_ms"] = max(
            metrics["max_latency_ms"], (time.monotonic() - start) * 1000
        )

        # Linux /proc reports total master RSS, not just refresh-backlog allocations.
        try:
            status = Path(f"/proc/{args.pid}/status").read_text()
            peak_rss = max(peak_rss, int(re.search(r"VmRSS:\s+(\d+)", status)[1]))
        except FileNotFoundError:
            pass

        # Leave 20 ms between samples; short pauses can fall between probes.
        time.sleep(0.02)

    args.output.write_text(json.dumps(dict(phases=phases, peak_master_rss_kb=peak_rss)) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)

    for name in ("seed", "write"):
        command = commands.add_parser(name)
        command.add_argument("directory", type=Path)
        command.add_argument("--count", type=int, required=True)
        command.add_argument("--workers", type=int, default=8)
        command.add_argument("--stop", type=Path)
        command.add_argument("--phase", type=Path)
        command.add_argument("--output", type=Path, required=True)

    command = commands.add_parser("manifest")
    command.add_argument("directory", type=Path)
    command.add_argument("--source-disk", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)

    command = commands.add_parser("verify")
    command.add_argument("manifest", type=Path)

    command = commands.add_parser("probe")
    command.add_argument("--host", required=True)
    command.add_argument("--port", type=int, required=True)
    command.add_argument("--pid", type=int, required=True)
    for option in ("phase", "stop", "output"):
        command.add_argument(f"--{option}", type=Path, required=True)

    command = commands.add_parser("report")
    command.add_argument("--directory", type=Path, required=True)
    command.add_argument("--max-master-reply-ms", type=float, default=0)

    args = parser.parse_args()

    if args.action in ("seed", "write"):
        if args.count < 1 or not 1 <= args.workers <= args.count:
            parser.error("require count >= workers >= 1")
        load(args)

    elif args.action == "manifest":
        # Record identities before overwriting, with the full EC inventory present.
        paths = sorted(args.directory.iterdir())
        files = [
            dict(path=str(path), id=chunk)
            for path, chunk in zip(paths, chunk_ids(paths, True))
        ]

        # Only targets actually present on the proxied source disk contribute to the race.
        source_ids = set()
        for path in args.source_disk.rglob("*.dat"):
            match = re.search(r"_([0-9A-Fa-f]{16})_[0-9A-Fa-f]{8}\.dat$", path.name)
            if match:
                source_ids.add(int(match[1], 16))

        source_parts = sum(entry["id"] in source_ids for entry in files)
        if not source_parts:
            raise RuntimeError("source disk has no target parts")

        args.output.write_text(json.dumps(dict(files=files, source_parts=source_parts)) + "\n")

    elif args.action == "verify":
        # Targets receive one overwrite; the continuous hot set uses a separate directory.
        files = json.loads(args.manifest.read_text())["files"]
        ids = chunk_ids([Path(entry["path"]) for entry in files])

        for index, entry in enumerate(files):
            path = Path(entry["path"])
            if ids[index] != entry["id"] or path.stat().st_size != SIZE:
                raise RuntimeError(f"chunk identity/length changed: {path}")

            with path.open("rb") as stream:
                if stream.read(BLOCK) != payload(index, True):
                    raise RuntimeError(f"incorrect target data: {path}")

    elif args.action == "probe":
        probe(args)

    else:
        failed = False

        for name in ("writes", "load", "probe", "proxy/state"):
            result = json.loads((args.directory / f"{name}.json").read_text())
            print(f"{name.upper()}: {json.dumps(result, sort_keys=True)}", flush=True)

            if name in ("writes", "load"):
                failed |= result["errors"] != 0 or result["operations"] == 0

            if name == "probe":
                # Promotion may briefly refuse connections. Once all required
                # peers are back, failures are errors, not acceptable stalls.
                for phase, metrics in result["phases"].items():
                    if phase == "promotion":
                        continue

                    failed |= metrics["errors"] != 0

                    # A zero limit leaves timings informational; correctness checks still apply.
                    if args.max_master_reply_ms:
                        failed |= metrics["max_latency_ms"] > args.max_master_reply_ms

                failed |= "conflicts" not in result["phases"]

        if failed:
            raise SystemExit("benchmark correctness/optional latency bound failed")


if __name__ == "__main__":
    main()
