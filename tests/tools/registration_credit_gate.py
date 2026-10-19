#!/usr/bin/env python3
"""Keep pull registration open while forwarding queries and disk-loss reports.

This test-only TCP proxy withholds credits, not inventory packets: each credit
permits one registration bulk. Forward one credit by default, then hold the rest
until the shell test releases them. Other traffic stays live, so successful
reads and loss notifications must work without a completed inventory.

Marker files synchronize the shell test: ready means listening, held means
credits were withheld, and end means registration completion was observed.
The shell creates release to resume registration. Optional query/lost markers
record proxy events; tests also check master-visible results separately.

Optional faults drop query replies to detect false-positive query tests, or
inject EC damage before inventory to verify that later registration preserves it.
"""

import argparse
from pathlib import Path
import select
import socket
import struct

from registration_reordering_proxy import receive_packet, signal


SAU_MATOCS_REGISTER_CHUNKS_START = 1000 + 182
SAU_MATOCS_REGISTER_CHUNKS_CREDIT = 1000 + 183
SAU_CSTOMA_REGISTER_CHUNKS_END = 1000 + 184
SAU_CSTOMA_QUERY_CHUNKS_RESPONSE = 1000 + 181
SAU_CSTOMA_REGISTER_CHUNKS = 1000 + 101
SAU_CSTOMA_CHUNK_DAMAGED = 1000 + 104
SAU_CSTOMA_CHUNK_LOST = 1000 + 105


def credit_packet(count):
    # Network-order packet header followed by protocol version 0 and credit count.
    payload = struct.pack("!II", 0, count)
    return struct.pack("!II", SAU_MATOCS_REGISTER_CHUNKS_CREDIT, len(payload)) + payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--initial-credits", type=int, default=1)
    for marker in ("ready", "held", "release", "end"):
        parser.add_argument(f"--{marker}-marker", required=True)
    parser.add_argument("--query-marker")
    parser.add_argument("--lost-marker")
    parser.add_argument("--drop-query-responses", action="store_true")
    parser.add_argument("--damage-first-bulk", action="store_true")
    args = parser.parse_args()
    if args.initial_credits < 0:
        parser.error("--initial-credits must be nonnegative")

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(1)
        signal(args.ready_marker)
        chunkserver, _ = listener.accept()
        with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
            # Forward only the initial allowance; retain excess credits for release.
            credits_left = args.initial_credits
            held_credits = 0
            released = False
            damage_injected = False
            while True:
                # Return withheld credits once, then stop intercepting new credits.
                if not released and Path(args.release_marker).exists():
                    if held_credits:
                        chunkserver.sendall(credit_packet(held_credits))
                    released = True
                    print("released registration credits", flush=True)

                readable, _, _ = select.select((chunkserver, master), (), (), 0.1)
                for source in readable:
                    packet = receive_packet(source)
                    if packet is None:
                        return
                    packet_type, payload, wire = packet
                    if source is master:
                        if packet_type == SAU_MATOCS_REGISTER_CHUNKS_START:
                            # Credits embedded in START would bypass our allowance.
                            if len(payload) != 12 or struct.unpack("!I", payload[8:])[0] != 0:
                                raise RuntimeError("credit gate requires a paced zero-credit START")
                        if packet_type == SAU_MATOCS_REGISTER_CHUNKS_CREDIT and not released:
                            if len(payload) != 8 or struct.unpack("!I", payload[:4])[0] != 0:
                                raise RuntimeError("invalid registration credit packet")
                            count = struct.unpack("!I", payload[4:])[0]
                            # Split grants that exceed the remaining allowance.
                            allowed = min(count, credits_left)
                            if allowed:
                                chunkserver.sendall(credit_packet(allowed))
                                credits_left -= allowed
                            held_credits += count - allowed
                            if held_credits:
                                signal(args.held_marker)
                            continue
                        chunkserver.sendall(wire)
                        continue

                    if (
                        packet_type == SAU_CSTOMA_REGISTER_CHUNKS
                        and args.damage_first_bulk
                        and not damage_injected
                    ):
                        # Require one version-2 record so injection uses the exact
                        # chunk ID and EC part type, without corrupting disk data.
                        if len(payload) != 22 or struct.unpack("!II", payload[:8]) != (2, 1):
                            raise RuntimeError("damage injection requires a single EC-format record")
                        chunk_id, _, part_type = struct.unpack("!QIH", payload[8:])
                        if part_type == 0:
                            raise RuntimeError("damage injection requires a nonstandard part")
                        # The master must see damage before the inventory record
                        # forwarded below; that record must not restore the part.
                        damage = struct.pack("!IIQH", 1, 1, chunk_id, part_type)
                        master.sendall(
                            struct.pack("!II", SAU_CSTOMA_CHUNK_DAMAGED, len(damage)) + damage
                        )
                        damage_injected = True
                    if packet_type == SAU_CSTOMA_QUERY_CHUNKS_RESPONSE:
                        # Fault injection verifies the query test cannot pass via timeout recovery.
                        if args.drop_query_responses:
                            continue
                        if args.query_marker:
                            signal(args.query_marker)
                    if packet_type == SAU_CSTOMA_REGISTER_CHUNKS_END:
                        if not released:
                            # Fail if the test's incomplete-inventory premise breaks.
                            raise RuntimeError(
                                "inventory completed while registration credits were held"
                            )
                        signal(args.end_marker)
                    master.sendall(wire)
                    if packet_type == SAU_CSTOMA_CHUNK_LOST and args.lost_marker:
                        signal(args.lost_marker)


if __name__ == "__main__":
    main()
