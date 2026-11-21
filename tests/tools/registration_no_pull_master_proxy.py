#!/usr/bin/env python3
"""Emulate a master that reports 5.13.0 without pull-registration support.

Stand in for a master that reports 5.13.0 but predates paced registration:
never deliver START or CREDIT to the chunkserver, and close the connection
when the chunkserver sends REGISTER_CHUNKS_END, as such a master does with any
unknown packet. Accepts reconnects and appends one line per event to the log.
"""

import argparse
import select
import socket

from registration_reordering_proxy import receive_packet, signal

REGISTER_CHUNKS_START = 1000 + 182
REGISTER_CHUNKS_CREDIT = 1000 + 183
REGISTER_CHUNKS_END = 1000 + 184


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--ready-marker", required=True)
    args = parser.parse_args()

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
                open_connection = True
                while open_connection:
                    readable, _, _ = select.select((chunkserver, master), (), (), 0.1)
                    for source in readable:
                        packet = receive_packet(source)
                        if packet is None:
                            side = "master" if source is master else "chunkserver"
                            print(f"connection {connection_count} closed by {side}", flush=True)
                            open_connection = False
                            break
                        packet_type, _, wire = packet
                        if source is master:
                            if packet_type in (REGISTER_CHUNKS_START, REGISTER_CHUNKS_CREDIT):
                                print(f"connection {connection_count} dropped master packet "
                                      f"{packet_type}", flush=True)
                                continue
                            chunkserver.sendall(wire)
                            continue
                        if packet_type == REGISTER_CHUNKS_END:
                            print(f"connection {connection_count} END received, killed",
                                  flush=True)
                            open_connection = False
                            break
                        master.sendall(wire)


if __name__ == "__main__":
    main()
