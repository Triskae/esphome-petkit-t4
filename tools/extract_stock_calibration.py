#!/usr/bin/env python3
"""Extract known PETKIT T4 calibration from full, unencrypted ESP32 flash dumps.

No fixed flash size, partition address, entry index or source hash. Supports
legacy and indexed NVS blobs. Unknown or inconsistent formats are rejected.
References: ESP-IDF components/nvs_flash/src/nvs_types.hpp and nvs_page.hpp.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

PAGE_SIZE = 4096
ENTRY_COUNT = 126
VARIABLE_TYPES = {0x21, 0x41, 0x42}


def crc32(data: bytes) -> int:
    return zlib.crc32(data, 0xFFFFFFFF)


@dataclass(frozen=True)
class Partition:
    table_offset: int
    offset: int
    size: int
    flags: int


@dataclass(frozen=True)
class Entry:
    order: tuple[int, int]
    offset: int
    namespace: int
    kind: int
    chunk: int
    key: bytes
    value: bytes
    payload: bytes


def find_partition(flash: bytes, label: str, table_offset: int | None) -> Partition:
    wanted = label.encode('ascii')
    offsets = [table_offset] if table_offset is not None else range(0, len(flash), PAGE_SIZE)
    matches = []
    for base in offsets:
        if base < 0 or base + 32 > len(flash) or flash[base:base + 2] != b'\xaa\x50':
            continue
        candidate = None
        terminated = False
        for pos in range(base, min(base + 0xC00, len(flash)), 32):
            raw = flash[pos:pos + 32]
            if len(raw) != 32:
                break
            if raw == b'\xff' * 32:
                terminated = True
                break
            if raw[:2] == b'\xeb\xeb':
                if hashlib.md5(flash[base:pos]).digest() != raw[16:32]:
                    raise ValueError(f'Partition table MD5 mismatch at {base:#x}')
                terminated = True
                break
            magic, kind, subtype, start, size, name, flags = struct.unpack('<HBBII16sI', raw)
            if magic != 0x50AA:
                break
            if name.rstrip(b'\0') == wanted:
                if candidate is not None:
                    raise ValueError(f'Duplicate partition label: {label}')
                if (kind, subtype) != (1, 2):
                    raise ValueError(f'{label} is not an NVS data partition')
                candidate = Partition(base, start, size, flags)
        if terminated and candidate:
            matches.append(candidate)
    if len(matches) != 1:
        raise ValueError(f'Expected one partition table containing {label}; found {len(matches)}. '
                         'Supply a full dump or use --table-offset for multiple tables.')
    part = matches[0]
    if part.flags & 1:
        raise ValueError('Encrypted partition is unsupported; supply a decrypted dump')
    if part.offset % PAGE_SIZE or part.size <= 0 or part.size % PAGE_SIZE:
        raise ValueError('Invalid NVS partition alignment or size')
    if part.offset + part.size > len(flash):
        raise ValueError('Truncated dump: calibration partition is not fully present')
    return part


def read_entries(flash: bytes, part: Partition) -> list[Entry]:
    pages = []
    for address in range(part.offset, part.offset + part.size, PAGE_SIZE):
        page = flash[address:address + PAGE_SIZE]
        if page == b'\xff' * PAGE_SIZE:
            continue
        state, sequence = struct.unpack_from('<II', page)
        if state not in {0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8}:
            raise ValueError(f'Unsupported/corrupt NVS page state at {address:#x}')
        if page[8] not in {0xFF, 0xFE}:
            raise ValueError(f'Unsupported NVS page version at {address:#x}')
        if crc32(page[4:28]) != struct.unpack_from('<I', page, 28)[0]:
            raise ValueError(f'NVS page CRC mismatch at {address:#x}')
        pages.append((sequence, address, page))
    if not pages:
        raise ValueError('No initialized NVS pages; calibration is absent or encrypted')
    sequences = [p[0] for p in pages]
    newest = [s for s in sequences if all((s - other) % (1 << 32) < (1 << 31)
                                        for other in sequences)]
    if len(newest) != 1 or len(set(sequences)) != len(sequences):
        raise ValueError('Ambiguous NVS page sequence numbers')
    pages.sort(key=lambda p: -((newest[0] - p[0]) % (1 << 32)))
    entries = []
    for rank, (_, address, page) in enumerate(pages):
        bitmap = int.from_bytes(page[32:64], 'little')
        index = 0
        while index < ENTRY_COUNT:
            if ((bitmap >> (index * 2)) & 3) != 2:
                index += 1
                continue
            pos = 64 + index * 32
            raw = page[pos:pos + 32]
            ns, kind, span, chunk, stored_crc = struct.unpack_from('<BBBBI', raw)
            if crc32(raw[:4] + raw[8:]) != stored_crc:
                raise ValueError(f'NVS entry CRC mismatch at {address + pos:#x}')
            if span < 1 or index + span > ENTRY_COUNT:
                raise ValueError(f'Invalid NVS entry span at {address + pos:#x}')
            key = raw[8:24].split(b'\0', 1)[0]
            if not key or b'\0' not in raw[8:24]:
                raise ValueError(f'Invalid NVS key at {address + pos:#x}')
            payload = b''
            if kind in VARIABLE_TYPES:
                size = struct.unpack_from('<H', raw, 24)[0]
                if span != 1 + (size + 31) // 32:
                    raise ValueError('NVS variable-data size/span mismatch')
                if any(((bitmap >> (j * 2)) & 3) != 2
                       for j in range(index + 1, index + span)):
                    raise ValueError('Incomplete NVS variable-data entry')
                payload = page[pos + 32:pos + 32 + size]
                if crc32(payload) != struct.unpack_from('<I', raw, 28)[0]:
                    raise ValueError(f'NVS blob CRC mismatch at {address + pos:#x}')
            elif span != 1:
                raise ValueError('Unsupported multi-entry NVS type')
            entries.append(Entry((rank, index), address + pos, ns, kind, chunk,
                                 key, raw[24:32], payload))
            index += span
    return entries


def read_blob(entries: list[Entry], namespace: str, key: str) -> tuple[bytes, list[Entry]]:
    namespaces = {e.value[0] for e in entries
                  if e.namespace == 0 and e.kind == 1 and e.key == namespace.encode('ascii')}
    if len(namespaces) != 1:
        raise ValueError(f'Missing or ambiguous NVS namespace: {namespace}')
    ns = namespaces.pop()
    if not 0 < ns < 255:
        raise ValueError('Invalid NVS namespace index')
    records = [e for e in entries if e.namespace == ns and e.key == key.encode('ascii')]
    # Chunk data is not committed until its matching blob index exists.
    committed = [e for e in records if e.kind != 0x42]
    if not committed:
        raise ValueError(f'No committed calibration blob: {namespace}/{key}')
    head = max(committed, key=lambda e: e.order)
    if head.kind not in {0x41, 0x48}:
        raise ValueError('Calibration key has an unsupported NVS data type')
    if head.kind == 0x41:
        return head.payload, [head]
    size, count, start = struct.unpack_from('<IBB', head.value)
    if start not in {0, 0x80} or count < 1 or count > 127 or start + count > 0xFF:
        raise ValueError('Unsupported NVS blob index layout')
    selected = []
    for chunk in range(start, start + count):
        candidates = [e for e in records if e.kind == 0x42 and e.chunk == chunk]
        if not candidates:
            raise ValueError(f'Missing committed NVS blob chunk: {chunk}')
        if len({e.payload for e in candidates}) > 1:
            raise ValueError('Conflicting live NVS blob chunks; refusing ambiguous recovery')
        selected.append(max(candidates, key=lambda e: e.order))
    payload = b''.join(e.payload for e in selected)
    if len(payload) != size:
        raise ValueError('NVS blob index size does not match its chunks')
    return payload, [head, *selected]


def extract(path: Path, *, partition: str = 'user_calibrt', namespace: str = 'T4',
            key: str = 'data', table_offset: int | None = None) -> dict:
    flash = path.read_bytes()
    part = find_partition(flash, partition, table_offset)
    payload, records = read_blob(read_entries(flash, part), namespace, key)
    if len(payload) != 20:
        raise ValueError(f'Unknown PETKIT calibration schema: expected 20 bytes, got {len(payload)}')
    zero, low, high, slope_low, slope_high = struct.unpack('<5i', payload)
    if not (0 < high < low < zero <= 0xFFFFFF and slope_low > 0 and slope_high > 0):
        raise ValueError('Calibration values do not match the known PETKIT T4 schema')
    return {
        'source_sha256': hashlib.sha256(flash).hexdigest(),
        'source_size': len(flash), 'partition_table_offset': hex(part.table_offset),
        'partition': partition, 'partition_offset': hex(part.offset),
        'namespace': namespace, 'key': key, 'blob_size': len(payload),
        'record_offsets': [hex(e.offset) for e in records], 'crc_verified': True,
        'calibration': {
            'zero_raw': zero, 'low_reference_raw': low, 'high_reference_raw': high,
            'slope_low_raw_per_gram': slope_low, 'slope_high_raw_per_gram': slope_high,
        },
        'warning': 'Known T4 schema only; extraction does not prove compatibility with other '
                   'models. Do not replace the current assembled-device tare blindly.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--partition', default='user_calibrt')
    parser.add_argument('--namespace', default='T4')
    parser.add_argument('--key', default='data')
    parser.add_argument('--table-offset', type=lambda value: int(value, 0),
                        help='Explicit partition-table offset, e.g. 0x8000')
    args = parser.parse_args()
    try:
        result = extract(args.dump, partition=args.partition, namespace=args.namespace,
                         key=args.key, table_offset=args.table_offset)
    except (OSError, ValueError, struct.error) as exc:
        print(f'Extraction failed: {exc}', file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
