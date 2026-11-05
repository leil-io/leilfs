#!/usr/bin/env python3
"""Read one block directly from the specified chunkserver, without mount replica selection.

The test's preload holds the real pread inside the chunkserver. This connection
therefore keeps its part Locked while the event loop answers discovery queries.
After release, verify the read payload and status against the original file.
"""

import argparse
from pathlib import Path
import socket
import struct
import zlib

from registration_reordering_proxy import receive_packet


READ = 1200
READ_STATUS = 1201
READ_DATA = 1202
BLOCK_SIZE = 65536


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--chunk-id", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--version", type=int, required=True)
    parser.add_argument("--expected", type=Path, required=True)
    args = parser.parse_args()

    # Version 1 uses a 16-bit part type. Type zero is a standard replicated chunk.
    request = struct.pack("!IQIHII", 1, args.chunk_id, args.version, 0, 0, BLOCK_SIZE)
    data = bytearray()
    with socket.create_connection((args.host, args.port), timeout=10) as connection:
        connection.settimeout(90)
        connection.sendall(struct.pack("!II", READ, len(request)) + request)
        while True:
            packet = receive_packet(connection)
            if packet is None:
                raise RuntimeError("replica closed before completing the read")
            kind, payload, _ = packet
            if kind == 0:
                continue
            if kind == READ_STATUS:
                protocol, chunk_id, status = struct.unpack("!IQB", payload)
                if (protocol, chunk_id, status) != (0, args.chunk_id, 0):
                    raise RuntimeError(f"read failed: {protocol}, {chunk_id}, {status}")
                break
            if kind != READ_DATA:
                raise RuntimeError(f"unexpected read packet {kind}")
            protocol, chunk_id, offset, size, crc = struct.unpack("!IQIII", payload[:24])
            block = payload[24:]
            if (protocol, chunk_id, offset, size) != (0, args.chunk_id, len(data), len(block)):
                raise RuntimeError("invalid read data header")
            if zlib.crc32(block) != crc:
                raise RuntimeError("read returned invalid CRC")
            data.extend(block)

    if len(data) != BLOCK_SIZE or data != args.expected.read_bytes()[:BLOCK_SIZE]:
        raise RuntimeError("direct read did not return the original block")
    print("direct replica read completed with original data and valid CRC", flush=True)


if __name__ == "__main__":
    main()
