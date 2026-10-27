#!/usr/bin/env python3
"""Create a measured refresh backlog using real, delayed disk-state reports.

After registration completes, a disk reload announces (old version, to-delete).
Hold those reports until each corresponding SET_VERSION succeeds, then forward
them unchanged. The master must query fresh state instead of trusting the old
report. Hold the query replies until released, or close the connection to test
backlog destruction. No versions, flags or successful statuses are fabricated.

The manifest limits interception to the benchmark's untouched EC hot set.
Counters describe wire traffic, not internal master queue sizes/completions.
"""

import argparse
import json
from pathlib import Path
import select
import socket
import struct
import time

from registration_reordering_proxy import receive_packet, signal


# Only these packet types need inspection; unrelated traffic remains live.
CHUNK_NEW = 1107
REGISTER_END = 1184
SET_VERSION = 1140
SET_VERSION_AND_LOCK = 1142
SET_VERSION_STATUS = 1141
QUERY_RESPONSE = 1181
TO_DELETE = 0x80000000


def packet(kind, payload):
    # Reframe filtered announcement batches without changing their record values.
    return struct.pack("!II", kind, len(payload)) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--master-port", type=int, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()

    directory = args.directory
    targets = {entry["id"] for entry in json.loads(args.manifest.read_text())["files"]}

    # Per (chunk ID, part type): captured report, commanded version, acknowledged version.
    reports, operations, released = {}, {}, {}

    # Held reply packets preserve arrival order; sets count distinct target IDs/parts.
    replies = []
    fresh = set()
    held_ids = set()

    started = time.monotonic()
    metrics = dict(
        held_reports=0,
        released_reports=0,
        held_replies=0,
        held_query_ids=0,
        forwarded_replies=0,
        fresh_parts=0,
        disconnected=False,
    )
    last_save = 0

    def save(force=False):
        # Publish by rename so the shell never reads a partially written JSON file.
        nonlocal last_save
        now = time.monotonic()
        if force or now - last_save >= 0.1:
            temporary = directory / "state.new"
            temporary.write_text(json.dumps(metrics) + "\n")
            temporary.replace(directory / "state.json")
            last_save = now

    def forward_reply(master, wire, found):
        master.sendall(wire)
        metrics["forwarded_replies"] += 1

        # Count a target part only when its to-delete flag and version reflect
        # the operation acknowledged before the old announcement was released.
        for chunk_id, version, part_type in found:
            expected = released.get((chunk_id, part_type))
            if (
                expected is not None
                and version & TO_DELETE
                and version & ~TO_DELETE >= expected
            ):
                if (chunk_id, part_type) not in fresh:
                    elapsed = (time.monotonic() - started) * 1000
                    metrics.setdefault("first_fresh_ms", elapsed)
                    metrics["last_fresh_ms"] = elapsed
                fresh.add((chunk_id, part_type))

        metrics["fresh_parts"] = len(fresh)

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(1)
        save(True)
        signal(str(directory / "ready"))

        chunkserver, _ = listener.accept()
        with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
            while True:
                # The disconnect action closes both sockets and leaves the master
                # to discard this connection's pending refresh state.
                if (directory / "disconnect").exists():
                    metrics["disconnected"] = True
                    save(True)
                    return

                # Release saved replies in order; subsequent target replies pass through.
                release = (directory / "release").exists()
                if release and replies:
                    for wire, found in replies:
                        forward_reply(master, wire, found)
                    replies.clear()

                save()

                # Poll marker files too, including when both sockets are idle.
                readable, _, _ = select.select((chunkserver, master), (), (), 0.02)
                for source in readable:
                    received = receive_packet(source)
                    if received is None:
                        raise RuntimeError("unexpected disconnect before benchmark action")
                    kind, payload, wire = received

                    if source is master:
                        # Observe real version commands but never hold master-to-CS traffic.
                        if kind in (SET_VERSION, SET_VERSION_AND_LOCK):
                            protocol, chunk_id, part_type, old, new = struct.unpack(
                                "!IQHII", payload
                            )
                            key = (chunk_id, part_type)

                            if key in reports:
                                expected_protocol = 1 if kind == SET_VERSION else 0
                                if protocol != expected_protocol or new != old + 1:
                                    raise RuntimeError("expected a real version increment")

                                if reports[key][1] & ~TO_DELETE != old or key in operations:
                                    raise RuntimeError("held report does not match version operation")

                                operations[key] = new

                        chunkserver.sendall(wire)
                        continue

                    # The shell arms interception only after CS0 finishes registration.
                    # Hold manifest targets; do not stall unrelated disk announcements.
                    if kind == CHUNK_NEW and (directory / "arm").exists():
                        protocol, count = struct.unpack("!II", payload[:8])
                        if protocol != 1 or len(payload) != 8 + count * 14:
                            raise RuntimeError("expected EC-format disk-state reports")

                        passed = []
                        for record in struct.iter_unpack("!QIH", payload[8:]):
                            chunk_id, version, part_type = record
                            key = (chunk_id, part_type)
                            if chunk_id in targets and key not in released:
                                if not version & TO_DELETE or key in reports:
                                    raise RuntimeError("expected one real to-delete report per part")
                                reports[key] = record
                            else:
                                passed.append(record)

                        metrics["held_reports"] = len(reports)

                        # A mixed batch needs new framing, but its forwarded records are unchanged.
                        if passed:
                            body = struct.pack("!II", 1, len(passed))
                            body += b"".join(struct.pack("!QIH", *record) for record in passed)
                            master.sendall(packet(kind, body))

                        continue

                    if kind == SET_VERSION_STATUS:
                        protocol, chunk_id, part_type, status = struct.unpack("!IQHB", payload)
                        key = (chunk_id, part_type)

                        if key in operations and key not in released:
                            if protocol != 1 or status != 0:
                                raise RuntimeError("target version operation failed")

                            # Preserve TCP ordering: successful status precedes the old report.
                            master.sendall(wire)
                            report = struct.pack("!IIQIH", 1, 1, *reports[key])
                            master.sendall(packet(CHUNK_NEW, report))
                            released[key] = operations[key]
                            metrics["released_reports"] = len(released)

                            elapsed = (time.monotonic() - started) * 1000
                            metrics.setdefault("first_release_ms", elapsed)
                            metrics["last_release_ms"] = elapsed
                            continue

                    # Replies list queried IDs first, followed by the found part records.
                    if kind == QUERY_RESPONSE:
                        protocol, _, count = struct.unpack("!IQI", payload[:16])
                        offset = 16 + count * 8
                        found_count = struct.unpack("!I", payload[offset:offset + 4])[0]
                        found = list(struct.iter_unpack("!QIH", payload[offset + 4:]))
                        if protocol != 0 or len(found) != found_count:
                            raise RuntimeError("invalid query reply")

                        queried = struct.unpack(f"!{count}Q", payload[16:offset])

                        # Hold the whole reply when it mentions a target; other replies stay live.
                        if any(chunk_id in targets for chunk_id in queried):
                            if release:
                                forward_reply(master, wire, found)
                            else:
                                replies.append((wire, found))
                                metrics["held_replies"] += 1

                                held_ids.update(
                                    chunk_id for chunk_id in queried if chunk_id in targets
                                )
                                metrics["held_query_ids"] = len(held_ids)

                            continue

                    # Ordinary inventory and other chunkserver traffic are forwarded unchanged.
                    master.sendall(wire)
                    if kind == REGISTER_END:
                        signal(str(directory / "registered"))


if __name__ == "__main__":
    main()
