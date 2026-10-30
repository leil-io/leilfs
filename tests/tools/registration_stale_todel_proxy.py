#!/usr/bin/env python3
"""Hold a disk-removal reannouncement until a concurrent version change completes.

Forward chunkserver <-> master traffic and hold the first SAU_CSTOMA_CHUNK_NEW
that announces the target chunk with the to-delete flag, until the release
marker exists. Records a marker when the master closes a connection, then keeps
accepting reconnects as a plain forwarder.
"""

import argparse
from pathlib import Path
import select
import socket
import struct

from registration_reordering_proxy import receive_packet, signal

CHUNK_NEW = 1000 + 107
TO_DELETE = 0x80000000


def announced_versions(payload, chunk_id):
    # version 0 entries: id:64 version:32 type:8; version 1: id:64 version:32 type:16
    if len(payload) < 8:
        return []
    packet_version, count = struct.unpack("!II", payload[:8])
    entry_size = 14 if packet_version == 1 else 13
    versions = []
    for index in range(count):
        offset = 8 + index * entry_size
        entry = payload[offset:offset + entry_size]
        if len(entry) != entry_size:
            break
        entry_id, version = struct.unpack("!QI", entry[:12])
        if entry_id == chunk_id:
            versions.append(version)
    return versions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--target-chunk", required=True, type=lambda value: int(value, 0))
    for marker in ("ready", "held", "release", "delivered", "master-closed"):
        parser.add_argument(f"--{marker}-marker", required=True)
    args = parser.parse_args()

    held_report = None
    delivered = False
    connection_count = 0
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(4)
        signal(args.ready_marker)
        while True:
            chunkserver, _ = listener.accept()
            connection_count += 1
            print(f"connection {connection_count} accepted", flush=True)
            with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
                while True:
                    if held_report is not None and not delivered and Path(args.release_marker).exists():
                        master.sendall(held_report)
                        delivered = True
                        print("delivered held report", flush=True)
                        signal(args.delivered_marker)

                    readable, _, _ = select.select((chunkserver, master), (), (), 0.1)
                    closed = False
                    for source in readable:
                        packet = receive_packet(source)
                        if packet is None:
                            side = "master" if source is master else "chunkserver"
                            print(f"connection {connection_count} closed by {side}", flush=True)
                            if source is master:
                                signal(args.master_closed_marker)
                            closed = True
                            break
                        packet_type, payload, wire = packet
                        if source is master:
                            chunkserver.sendall(wire)
                            continue
                        if packet_type == CHUNK_NEW and held_report is None:
                            versions = announced_versions(payload, args.target_chunk)
                            print(f"CHUNK_NEW target versions: {[hex(v) for v in versions]}",
                                  flush=True)
                            if any(version & TO_DELETE for version in versions):
                                held_report = wire
                                print("held to-delete announcement", flush=True)
                                signal(args.held_marker)
                                continue
                        master.sendall(wire)
                    if closed:
                        break


if __name__ == "__main__":
    main()
