#!/usr/bin/env python3
"""Exercise START-timeout fallback on the wire.

Forward early REGISTER_SPACE, drop START/credits, and hold the target report
and fallback tail until the shell test finishes checking credits and repair.
"""

import argparse
from pathlib import Path
import select
import socket
import time

from registration_reordering_proxy import receive_packet, registration_contains_chunk, signal
from registration_space_proxy import (
    SAU_CSTOMA_REGISTER_CHUNKS_END,
    SAU_CSTOMA_REGISTER_SPACE,
    SAU_MATOCS_REGISTER_CHUNKS_CREDIT,
    SAU_MATOCS_REGISTER_CHUNKS_START,
    SAU_MATOCS_REPLICATE_CHUNK,
    maintenance_loop_end,
)


SAU_CSTOMA_REGISTER_CHUNKS = 1000 + 101


def proxy_connection(chunkserver, master, args):
    # Markers survive reconnects; held packets and accounting are per socket.
    held = []
    saw_chunks = False
    forwarded_bulk = False
    holding_tail = False
    released = False
    previous_loop = None
    completed_loops = 0
    next_probe = 0
    while True:
        if not released and Path(args.release_marker).exists():
            for packet_type, wire in held:
                master.sendall(wire)
                if packet_type == SAU_CSTOMA_REGISTER_CHUNKS_END:
                    signal(args.end_marker)
            held.clear()
            released = True
        # Require real maintenance progress before asserting no repair was attempted.
        if previous_loop is not None and not released and time.monotonic() >= next_probe:
            current_loop = maintenance_loop_end(args.host, args.client_port)
            if current_loop != previous_loop:
                completed_loops += 1
                previous_loop = current_loop
            if completed_loops >= 2:
                signal(args.maintenance_marker)
            next_probe = time.monotonic() + 0.2
        readable, _, _ = select.select((chunkserver, master), (), (), 0.1)
        for source in readable:
            packet = receive_packet(source)
            if packet is None:
                return
            packet_type, payload, wire = packet
            if source is master:
                if packet_type == SAU_MATOCS_REGISTER_CHUNKS_START:
                    # Force timeout; a successful push retry must not need START either.
                    continue
                if packet_type == SAU_MATOCS_REGISTER_CHUNKS_CREDIT:
                    # Opening credits are legitimate; regrants after a push bulk are not.
                    # Drop both so no credit can accidentally release a pull sweep.
                    if forwarded_bulk:
                        signal(args.credit_marker)
                    else:
                        signal(args.opening_marker)
                    continue
                if packet_type == SAU_MATOCS_REPLICATE_CHUNK and not released:
                    signal(args.repair_marker)
                chunkserver.sendall(wire)
                continue

            if not released:
                if packet_type == SAU_CSTOMA_REGISTER_CHUNKS:
                    saw_chunks = True
                    if registration_contains_chunk(payload, args.target_chunk):
                        # Keep an existing replica unknown while other bulks are forwarded.
                        held.append((packet_type, wire))
                        continue
                if packet_type == SAU_CSTOMA_REGISTER_SPACE and saw_chunks:
                    holding_tail = True
                # Do not let ordinary space updates overtake held completion packets.
                if holding_tail:
                    held.append((packet_type, wire))
                    continue
            master.sendall(wire)
            if packet_type == SAU_CSTOMA_REGISTER_SPACE and not saw_chunks:
                signal(args.space_marker)
            if packet_type == SAU_CSTOMA_REGISTER_CHUNKS and not forwarded_bulk:
                forwarded_bulk = True
                previous_loop = maintenance_loop_end(args.host, args.client_port)
                signal(args.bulk_marker)
            if packet_type == SAU_CSTOMA_REGISTER_CHUNKS_END:
                signal(args.end_marker)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--client-port", required=True, type=int)
    parser.add_argument("--target-chunk", required=True, type=lambda value: int(value, 0))
    for name in (
        "ready", "space", "opening", "bulk", "maintenance", "credit", "repair", "release", "end"
    ):
        parser.add_argument(f"--{name}-marker", required=True)
    args = parser.parse_args()

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(1)
        signal(args.ready_marker)
        # The fix reconnects for push fallback, so keep accepting replacement sockets.
        while True:
            chunkserver, _ = listener.accept()
            with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
                proxy_connection(chunkserver, master, args)


if __name__ == "__main__":
    main()
