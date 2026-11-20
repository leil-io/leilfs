#!/usr/bin/env python3
"""Observe real mount truncate phases and the first concurrent write reply.

Forward packets unchanged. Optionally hold the client-assisted truncate reply
until a marker exists: the shell can then connect an incomplete peer after the
master granted the truncate lock but before the mount starts zeroing data.
The proxy never fabricates statuses or performs the filesystem operation.
"""

import argparse
from pathlib import Path
import select
import socket
import struct
import threading

from registration_reordering_proxy import receive_packet, signal


TRUNCATE_REPLY = 1465
TRUNCATE_END = 1497
TRUNCATE_END_REPLY = 1498
WRITE_REQUEST = 1434
WRITE_REPLY = 1435

counter_lock = threading.Lock()


def record_first_status(marker, status):
    if not marker.exists():
        temporary = marker.with_name(marker.name + ".tmp")
        temporary.write_text(str(status) + "\n")
        temporary.replace(marker)


def record_write_request(prefix):
    marker = Path(prefix + "-write-request-count")
    with counter_lock:
        count = int(marker.read_text(encoding="utf-8")) if marker.exists() else 0
        temporary = marker.with_name(marker.name + ".tmp")
        temporary.write_text(str(count + 1) + "\n", encoding="utf-8")
        temporary.replace(marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--prepared-release-marker")
    args = parser.parse_args()

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(8)
        signal(args.prefix + "-ready")
        while True:
            # fileinfo and other tools open independent connections through .masterinfo.
            client, _ = listener.accept()
            threading.Thread(target=forward_client, args=(client, args), daemon=True).start()


def forward_client(client, args):
    with client, socket.create_connection((args.host, args.master_port)) as master:
        prepared_reply = None
        while True:
            if prepared_reply is not None and Path(args.prepared_release_marker).exists():
                client.sendall(prepared_reply)
                prepared_reply = None

            readable, _, _ = select.select((client, master), (), (), 0.05)
            for source in readable:
                packet = receive_packet(source)
                if packet is None:
                    return
                packet_type, payload, wire = packet
                destination = client if source is master else master

                if source is master and packet_type == TRUNCATE_REPLY:
                    version = struct.unpack("!I", payload[:4])[0]
                    if version == 2:
                        signal(args.prefix + "-prepared")
                        print("client-assisted truncate prepared", flush=True)
                        if args.prepared_release_marker:
                            prepared_reply = wire
                            continue
                    elif version == 0:
                        marker = Path(args.prefix + "-truncate-status")
                        record_first_status(marker, payload[8])
                        print("initial truncate status=" + str(payload[8]), flush=True)

                if source is client and packet_type == TRUNCATE_END:
                    signal(args.prefix + "-end-sent")
                    print("TRUNCATE_END forwarded", flush=True)
                elif source is master and packet_type == TRUNCATE_END_REPLY:
                    signal(args.prefix + "-end-reply")
                    print("TRUNCATE_END reply=" + payload.hex(), flush=True)
                elif source is client and packet_type == WRITE_REQUEST:
                    record_write_request(args.prefix)
                    signal(args.prefix + "-write-request")
                elif source is master and packet_type == WRITE_REPLY:
                    version = struct.unpack("!I", payload[:4])[0]
                    status = payload[8] if version == 0 else 0
                    marker = Path(args.prefix + "-write-status")
                    record_first_status(marker, status)
                    print("write-start status=" + str(status), flush=True)

                destination.sendall(wire)


if __name__ == "__main__":
    main()
