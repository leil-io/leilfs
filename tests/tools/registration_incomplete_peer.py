#!/usr/bin/env python3
"""Keep an empty inventory incomplete without taking any real EC parts offline.

The shell test stores data on real chunkservers. This additional protocol peer
owns no chunks and advertises no capacity. It either withholds query replies
while pull registration is open, or reports 5.12.0 and withholds the final push
space report. Both modes isolate discovery admission from data availability.

Creating the release marker finishes registration and answers held queries
with genuine misses. NOPs keep the connection alive throughout the experiment.
This models legacy protocol eligibility; it does not run legacy binaries.
"""

import argparse
from pathlib import Path
import select
import socket
import struct
import time

from registration_reordering_proxy import receive_packet, signal


REGISTER_HOST = 1100
REGISTER_SPACE = 1102
REGISTER_HOST_REPLY = 1175
QUERY_CHUNKS = 1180
QUERY_REPLY = 1181
REGISTER_CREDIT = 1183
REGISTER_END = 1184


def send_packet(connection, packet_type, payload=b""):
    connection.sendall(struct.pack("!II", packet_type, len(payload)) + payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument("--service-port", required=True, type=int)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--legacy", action="store_true")
    parser.add_argument(
        "--query-release-marker",
        help="reply to held queries when this marker appears, without ending registration",
    )
    args = parser.parse_args()

    prefix = args.prefix
    version = (5 << 16) | ((12 if args.legacy else 13) << 8)
    address = struct.unpack("!I", socket.inet_aton(args.host))[0]
    # Both releases use host packet version 1 with a NUL-terminated cluster ID.
    host = struct.pack("!IIHIII", 1, address, args.service_port, 60000, version, 8)
    host += b"default\0"
    space = struct.pack("!IQQIQQI", 0, 0, 0, 0, 0, 0, 0)

    with socket.create_connection((args.host, args.master_port)) as connection:
        send_packet(connection, REGISTER_HOST, host)
        pending_queries = []
        query_count = 0
        queries_released = False
        released = False
        last_keepalive = time.monotonic()

        while True:
            if not released and Path(prefix + "-release").exists():
                if args.legacy:
                    send_packet(connection, REGISTER_SPACE, space)
                else:
                    send_packet(connection, REGISTER_END, struct.pack("!IQ", 0, 0))
                if not queries_released:
                    for payload in pending_queries:
                        # Echo version, generation and IDs; found and busy vectors are empty.
                        send_packet(connection, QUERY_REPLY, payload + struct.pack("!II", 0, 0))
                    pending_queries.clear()
                    queries_released = True
                released = True
                signal(prefix + "-completed")
                print("released empty inventory and held queries", flush=True)

            query_release_marker = args.query_release_marker
            if (
                query_release_marker
                and not queries_released
                and Path(query_release_marker).exists()
            ):
                for payload in pending_queries:
                    # Report this query while keeping the inventory incomplete.
                    send_packet(connection, QUERY_REPLY, payload + struct.pack("!II", 0, 0))
                pending_queries.clear()
                queries_released = True
                signal(prefix + "-queries-released")
                print("released held queries; inventory remains incomplete", flush=True)

            if time.monotonic() - last_keepalive >= 1:
                send_packet(connection, 0)
                last_keepalive = time.monotonic()

            readable, _, _ = select.select((connection,), (), (), 0.1)
            if not readable:
                continue
            packet = receive_packet(connection)
            if packet is None:
                raise RuntimeError("master disconnected the incomplete peer")
            packet_type, payload, _ = packet

            if packet_type == REGISTER_HOST_REPLY:
                if len(payload) < 5 or payload[4] != 0:
                    raise RuntimeError("master refused the test peer")
                if args.legacy:
                    signal(prefix + "-ready")
                else:
                    send_packet(connection, REGISTER_SPACE, space)
            elif packet_type == REGISTER_CREDIT:
                # Receiving credit proves the master processed the initial space report.
                signal(prefix + "-ready")
            elif packet_type == QUERY_CHUNKS:
                if args.legacy:
                    raise RuntimeError("master queried a peer without query support")
                query_count += 1
                signal(prefix + "-query")
                if query_count == 2:
                    signal(prefix + "-query-2")
                print("query received; held=" + str(not queries_released), flush=True)
                if queries_released:
                    send_packet(connection, QUERY_REPLY, payload + struct.pack("!II", 0, 0))
                else:
                    pending_queries.append(payload)


if __name__ == "__main__":
    main()
