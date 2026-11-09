#!/usr/bin/env python3
"""Hold a zero-space pull registration open to check allocation and repair eligibility."""

import argparse
from pathlib import Path
import select
import socket
import struct
import time

from registration_reordering_proxy import receive_packet, signal


SAU_CSTOMA_REGISTER_SPACE = 1000 + 102
SAU_MATOCS_REGISTER_CHUNKS_START = 1000 + 182
SAU_MATOCS_REGISTER_CHUNKS_CREDIT = 1000 + 183
SAU_CSTOMA_REGISTER_CHUNKS_END = 1000 + 184
CSTOMA_SPACE = 101
SAU_MATOCS_REPLICATE_CHUNK = 1000 + 150
CLTOMA_CHUNKSTEST_INFO = 514
MATOCL_CHUNKSTEST_INFO = 515


def maintenance_loop_end(host, port):
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.sendall(struct.pack("!II", CLTOMA_CHUNKSTEST_INFO, 0))
        packet = receive_packet(connection)
        if packet is None or packet[0] != MATOCL_CHUNKSTEST_INFO or len(packet[1]) != 52:
            raise RuntimeError("invalid chunk-maintenance statistics reply")
        return struct.unpack("!II", packet[1][:8])[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--zero-marker", required=True)
    parser.add_argument("--space-marker", required=True)
    parser.add_argument("--release-marker", required=True)
    parser.add_argument("--end-marker", required=True)
    parser.add_argument("--client-port", required=True, type=int)
    parser.add_argument("--maintenance-marker", required=True)
    parser.add_argument("--repair-marker", required=True)
    args = parser.parse_args()

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(1)
        chunkserver, _ = listener.accept()
        with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
            held_credits = []
            zero_sent = False
            released = False
            previous_loop = None
            completed_loops = 0
            next_probe = 0
            while True:
                if not released and Path(args.release_marker).exists():
                    for credit in held_credits:
                        chunkserver.sendall(credit)
                    held_credits.clear()
                    released = True
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
                    if source is chunkserver:
                        if packet_type == SAU_CSTOMA_REGISTER_SPACE:
                            if zero_sent or len(payload) != 44:
                                raise RuntimeError("unexpected registration-space packet")
                            payload = payload[:4] + bytes(16) + payload[20:]
                            wire = struct.pack("!II", packet_type, len(payload)) + payload
                            zero_sent = True
                            master.sendall(wire)
                            signal(args.zero_marker)
                            continue
                        master.sendall(wire)
                        if packet_type == CSTOMA_SPACE:
                            used, total = struct.unpack("!QQ", payload[:16])
                            if zero_sent and total > used:
                                signal(args.space_marker)
                                if previous_loop is None:
                                    previous_loop = maintenance_loop_end(args.host, args.client_port)
                        if packet_type == SAU_CSTOMA_REGISTER_CHUNKS_END:
                            if not released:
                                raise RuntimeError("registration completed before credit release")
                            signal(args.end_marker)
                    else:
                        if packet_type == SAU_MATOCS_REPLICATE_CHUNK and not released:
                            signal(args.repair_marker)
                        if packet_type == SAU_MATOCS_REGISTER_CHUNKS_START:
                            if len(payload) != 12 or struct.unpack("!I", payload[8:])[0] != 0:
                                raise RuntimeError("test requires a paced zero-credit start")
                        if packet_type == SAU_MATOCS_REGISTER_CHUNKS_CREDIT and not released:
                            held_credits.append(wire)
                        else:
                            chunkserver.sendall(wire)


if __name__ == "__main__":
    main()
