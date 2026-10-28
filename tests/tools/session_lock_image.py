#!/usr/bin/env python3
"""Normalize persisted locks, including ranges and pending order, without a Shadow RPC."""

import argparse
from pathlib import Path
import struct


def image_locks(data, width):
    # Header fields are inode, metadata version and next session id after the signature.
    offset = 8 + width + 8 + 4
    while offset + 16 <= len(data):
        header = data[offset:offset + 16]
        if header in (b'[SFS EOF MARKER]', b'[MFS EOF MARKER]'):
            return None
        name, length = struct.unpack('>8sQ', header)
        offset += 16
        assert offset + length <= len(data), 'section exceeds the metadata image'
        if name == b'FLCK 1.0':
            return decode_locks(data[offset:offset + length], width)
        offset += length
    raise AssertionError('metadata has no complete lock section')


def decode_locks(data, width):
    # Four counted groups: flock active/pending, then POSIX active/pending.
    entry = struct.Struct('>I' + ('I' if width == 4 else 'Q') + 'QIHQQ')
    offset, rows = 0, []
    for kind in ('flock', 'posix'):
        for state in ('active', 'pending'):
            count, = struct.unpack_from('>Q', data, offset)
            offset += 8
            assert count <= (len(data) - offset) // entry.size, 'truncated lock queue'
            positions = {}
            for _ in range(count):
                fields = entry.unpack_from(data, offset)
                version, inode, owner, session, lock_type, start, end = fields
                offset += entry.size
                assert version == 0 and lock_type in (2, 4), 'unknown lock encoding'
                position = positions.get(inode, 0) if state == 'pending' else 0
                positions[inode] = position + 1
                rows.append(f'{kind} {state} {inode} {position} {owner} {session} '
                            f'{lock_type} {start} {end}')
    assert offset == len(data), 'trailing bytes in lock section'
    return sorted(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('metadata', type=Path)
    data = parser.parse_args().metadata.read_bytes()
    assert data[:8] in (b'SFSM 2.9', b'SAUM 2.9', b'LIZM 2.9'), 'unknown metadata signature'

    # Only a width with valid section boundaries and a complete lock section is accepted.
    candidates = []
    for width in (4, 8):
        try:
            rows = image_locks(data, width)
            if rows is not None:
                candidates.append(rows)
        except (AssertionError, struct.error):
            pass
    assert len(candidates) == 1, 'metadata has no unique valid lock section'
    for row in candidates[0]:
        print(row)


if __name__ == '__main__':
    main()
