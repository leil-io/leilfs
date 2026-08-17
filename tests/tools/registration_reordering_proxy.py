#!/usr/bin/env python3
"""Force a registration packet to arrive after an on-demand query reply.

This is a test-only TCP proxy. It holds the pull-registration packet for one
chunk, forwards the master's query and the chunkserver's priority reply, then
releases that old registration packet immediately after forwarding the
set-version request. The resulting wire order is the one a priority reply
can create when it overtakes queued registration bulks.
"""

import argparse
import select
import socket
import struct
import sys
from typing import List, Optional, Tuple, cast


SAU_CSTOMA_REGISTER_CHUNKS = 1000 + 101
SAU_CSTOMA_QUERY_CHUNKS_RESPONSE = 1000 + 177
SAU_MATOCS_SET_VERSION = 1000 + 140
SAU_MATOCS_SET_VERSION_AND_LOCK = 1000 + 142
SET_VERSION_PACKET_TYPES = (
    SAU_MATOCS_SET_VERSION,
    SAU_MATOCS_SET_VERSION_AND_LOCK,
)


def receive_exact(sock: socket.socket, size: int) -> Optional[bytes]:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def receive_packet(sock: socket.socket) -> Optional[Tuple[int, bytes, bytes]]:
    header = receive_exact(sock, 8)
    if header is None:
        return None
    packet_type, length = struct.unpack("!II", header)
    payload = receive_exact(sock, length)
    if payload is None:
        return None
    return packet_type, payload, header + payload


def registration_contains_chunk(payload: bytes, chunk_id: int) -> bool:
    if len(payload) < 8:
        return False

    version = struct.unpack("!I", payload[:4])[0]
    count = struct.unpack("!I", payload[4:8])[0]
    record_size = {0: 13, 1: 12, 2: 14}.get(version)
    if record_size is None or len(payload) != 8 + count * record_size:
        raise RuntimeError(
            f"invalid register-chunks packet: version={version}, payload={len(payload)}"
        )
    return any(
        struct.unpack("!Q", payload[offset : offset + 8])[0] == chunk_id
        for offset in range(8, len(payload), record_size)
    )


def set_version_targets_chunk(payload: bytes, chunk_id: int) -> bool:
    # The packet starts with its protocol-version word followed by chunk id.
    return len(payload) >= 12 and struct.unpack("!Q", payload[4:12])[0] == chunk_id


def signal(path: str) -> None:
    with open(path, "w", encoding="utf-8") as marker:
        marker.write("ready\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-host", required=True)
    parser.add_argument("--listen-port", required=True, type=int)
    parser.add_argument("--master-host", required=True)
    parser.add_argument("--master-port", required=True, type=int)
    parser.add_argument(
        "--target-chunk", required=True, type=lambda value: int(value, 0)
    )
    parser.add_argument("--held-marker", required=True)
    args = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_host, args.listen_port))
    listener.listen(1)

    chunkserver, _ = listener.accept()
    master = socket.create_connection((args.master_host, args.master_port))

    held_registration: Optional[bytes] = None
    buffered_chunkserver_packets: List[bytes] = []
    query_reply_seen = False
    released = False

    try:
        while True:
            readable, _, _ = select.select((chunkserver, master), (), ())
            for source in readable:
                source_socket = cast(socket.socket, source)
                packet = receive_packet(source_socket)
                if packet is None:
                    return
                packet_type, payload, wire_packet = packet

                if source_socket is chunkserver:
                    if (
                        packet_type == SAU_CSTOMA_REGISTER_CHUNKS
                        and held_registration is None
                        and registration_contains_chunk(payload, args.target_chunk)
                    ):
                        held_registration = wire_packet
                        signal(args.held_marker)
                        print("held target registration packet", flush=True)
                        continue

                    if held_registration is not None and not released:
                        if packet_type == SAU_CSTOMA_QUERY_CHUNKS_RESPONSE:
                            query_reply_seen = True
                            master.sendall(wire_packet)
                            print("forwarded priority query response", flush=True)
                        else:
                            buffered_chunkserver_packets.append(wire_packet)
                        continue

                    master.sendall(wire_packet)
                    continue

                chunkserver.sendall(wire_packet)
                if (
                    query_reply_seen
                    and not released
                    and packet_type in SET_VERSION_PACKET_TYPES
                    and set_version_targets_chunk(payload, args.target_chunk)
                ):
                    assert held_registration is not None
                    master.sendall(held_registration)
                    for buffered_packet in buffered_chunkserver_packets:
                        master.sendall(buffered_packet)
                    released = True
                    print("released stale registration after set-version", flush=True)
    finally:
        master.close()
        chunkserver.close()
        listener.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"registration reordering proxy: {error}", file=sys.stderr, flush=True)
        raise
