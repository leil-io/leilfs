#!/usr/bin/env python3
"""Delay a real disk-state announcement across a chunk version operation.

Initial registration passes unchanged. After a disk reload, hold the target's
CHUNK_NEW(v, to-delete) and observe its next SET_VERSION(v -> v+1). In busy mode,
send the held report before forwarding that command, so its reply cannot race
ahead. In settled mode, wait for both the successful status and the shell's
release marker, created after the overwrite finishes.

Only packet ordering changes: the announced version and disk flag are real.
Queries stay live so the master can retrieve fresh state from the chunkserver.
"""

import argparse
from pathlib import Path
import select
import socket
import struct

from registration_reordering_proxy import receive_packet, signal


# Inspect only the packets needed to order the race; forward all other traffic.
CHUNK_NEW = 1000 + 107
REGISTER_CHUNKS_END = 1000 + 184
SET_VERSION = 1000 + 140
SET_VERSION_AND_LOCK = 1000 + 142
SET_VERSION_STATUS = 1000 + 141
TO_DELETE = 0x80000000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--listen-port', type=int, required=True)
    parser.add_argument('--master-port', type=int, required=True)
    parser.add_argument('--target-chunk', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--order', choices=('busy', 'settled'), required=True)

    # Marker files synchronize the shell's reload/write/assertion steps with packet forwarding.
    for marker in ('ready', 'registered', 'held', 'status', 'release', 'delivered'):
        parser.add_argument(f'--{marker}-marker', required=True)
    args = parser.parse_args()

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.listen_port))
        listener.listen(1)
        signal(args.ready_marker)
        chunkserver, _ = listener.accept()
        with chunkserver, socket.create_connection((args.host, args.master_port)) as master:
            # Capture only a post-registration reload report for the target copy.
            registered = False
            held_report = None
            old_version = None

            operation_seen = False
            status_seen = False
            delivered = False

            def deliver_report():
                nonlocal delivered
                # Replay the captured bytes unchanged; no version or disk flag is fabricated.
                master.sendall(held_report)
                delivered = True
                print(f'delivered stale disk state while {args.order}: version={old_version}',
                      flush=True)
                signal(args.delivered_marker)

            while True:
                # Settled mode waits for the forwarded success status AND the shell's
                # confirmation that the overwrite/fsync has returned.
                if args.order == 'settled' and not delivered and Path(args.release_marker).exists():
                    if held_report is None or not status_seen:
                        raise RuntimeError('release requires a held report and successful operation')
                    deliver_report()

                # Poll marker changes even when neither socket has traffic.
                readable, _, _ = select.select((chunkserver, master), (), (), 0.1)
                for source in readable:
                    packet = receive_packet(source)
                    if packet is None:
                        # Reconnection could hide the bug by supplying a fresh inventory.
                        raise RuntimeError('connection closed; reconnect must not repair this race')
                    packet_type, payload, wire = packet

                    if source is master:
                        # Observe the command before the worker can execute or acknowledge it.
                        if packet_type in (SET_VERSION, SET_VERSION_AND_LOCK):
                            if len(payload) != 22:
                                raise RuntimeError('expected EC-format version command')
                            protocol, chunk_id, part_type, old, new = struct.unpack('!IQHII', payload)
                            expected_protocol = 1 if packet_type == SET_VERSION else 0
                            if protocol != expected_protocol:
                                raise RuntimeError('unexpected version-command protocol')
                            if chunk_id == args.target_chunk and held_report is not None:
                                if (
                                    operation_seen or part_type != 0
                                    or old != old_version or new != old + 1
                                ):
                                    raise RuntimeError('expected one version increment of the held copy')
                                operation_seen = True
                                print(f'observed version operation: {old} -> {new}', flush=True)
                                if args.order == 'busy':
                                    # The master already owns BUSY v+1, but the worker
                                    # cannot reply until we forward its command below.
                                    deliver_report()

                        # Queries and unrelated master commands must remain live during the race.
                        chunkserver.sendall(wire)
                        continue

                    # The remaining branches handle chunkserver-to-master traffic.
                    if packet_type == CHUNK_NEW:
                        if len(payload) != 22 or struct.unpack('!II', payload[:8]) != (1, 1):
                            raise RuntimeError('expected a single EC-format announcement')
                        chunk_id, version, part_type = struct.unpack('!QIH', payload[8:])
                        if chunk_id == args.target_chunk:
                            if held_report is None:
                                # Keep the disk reload announcement out of the master's input
                                # until the selected BUSY/settled release point.
                                if not registered or part_type != 0 or not version & TO_DELETE:
                                    raise RuntimeError('expected a post-registration disk reload report')
                                old_version = version & ~TO_DELETE
                                held_report = wire
                                print(f'held disk reload report: version={old_version}, to-delete=true',
                                      flush=True)
                                signal(args.held_marker)
                                continue
                            # Extra reports must not conceal the initial race, but
                            # fresh reconciliation after delivery is legitimate.
                            if not delivered:
                                raise RuntimeError('extra target report before stale delivery')

                    if packet_type == SET_VERSION_STATUS and operation_seen:
                        if len(payload) != 15:
                            raise RuntimeError('expected EC-format version status')
                        protocol, chunk_id, part_type, status = struct.unpack('!IQHB', payload)
                        if chunk_id == args.target_chunk:
                            if protocol != 1 or part_type != 0 or status != 0 or status_seen:
                                raise RuntimeError('expected one successful version status')
                            # Forward status before signaling the shell. Settled mode also
                            # waits for release after the overwrite/fsync completes.
                            master.sendall(wire)
                            status_seen = True
                            print('forwarded successful version status', flush=True)
                            signal(args.status_marker)
                            continue

                    master.sendall(wire)
                    if packet_type == REGISTER_CHUNKS_END:
                        if registered:
                            raise RuntimeError('unexpected second registration')
                        registered = True
                        # Signal only after forwarding the inventory-end packet to the master.
                        signal(args.registered_marker)


if __name__ == '__main__':
    main()
