"""Turn the render server's split captures into pack experiments.

The device logs each captured split draw as `raw: splitcap` lines (raw/runtime/raw_split_capture.h, debug switch
split-capture). Each record becomes one experiment per form, the draw replayed as one quad with its own vertices, root
push words and uniform blocks at their residual offsets.

Usage: capture.py <log> <batch.json> [--key <16 hex digits>]
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

from tools.benchloop import protocol

MAGIC, VERSION = 0x50414353, 1
HEAD = re.compile(r'splitcap (\d+) key=([0-9a-f]{16}) .* bytes=(\d+) parts=(\d+)')
PART = re.compile(r'splitcap (\d+)\.(\d+) ([0-9a-f]+)')
NAMES = {raw: name for name, (raw, _) in protocol.FORMATS.items()}


def blobs(text: str) -> dict[int, bytes]:
    """Give each complete record's blob by its serial; a record missing a part is left out."""
    sizes, parts = {}, {}
    for line in text.splitlines():
        if m := HEAD.search(line):
            sizes[int(m[1])] = (int(m[3]), int(m[4]))
        elif m := PART.search(line):
            parts.setdefault(int(m[1]), {})[int(m[2])] = bytes.fromhex(m[3])
    out = {}
    for serial, (size, count) in sizes.items():
        got = parts.get(serial, {})
        if sorted(got) == list(range(count)):
            blob = b''.join(got[i] for i in range(count))
            if len(blob) == size:
                out[serial] = blob
    return out


def record(blob: bytes) -> dict:
    """Read one blob as raw_split_capture.h splitCaptureBlob writes it."""
    words = struct.unpack_from('<16I', blob)
    if words[0] != MAGIC or words[1] != VERSION:
        raise ValueError('not a split capture')
    at = 64
    push = list(struct.unpack_from('<32I', blob, at))
    at += 128

    def chunk() -> bytes:
        nonlocal at
        (n,) = struct.unpack_from('<I', blob, at)
        data = blob[at + 4 : at + 4 + n]
        at += 4 + n
        return data

    vertices = chunk()
    (count,) = struct.unpack_from('<I', blob, at)
    at += 4
    blocks = []
    for _ in range(count):
        stage, slot, offset = struct.unpack_from('<3I', blob, at)
        at += 12
        blocks.append({'stage': stage, 'slot': slot, 'offset': offset, 'data': chunk()})
    return {
        'key': words[3] << 32 | words[2],
        'ca': bool(words[4]),
        'format': words[5],
        'width': words[6],
        'height': words[7],
        'box': list(words[8:12]),
        'primitive': words[12],
        'first': words[13],
        'count': words[14],
        'push': push,
        'vertices': vertices,
        'blocks': blocks,
    }


def experiments(serial: int, r: dict) -> list[dict]:
    """Give the record's draw as one experiment per form; none for a target format the bench cannot read."""
    if r['format'] not in NAMES:
        return []
    x0, y0, x1, y1 = r['box']
    rect = [x0, y0, x1, y1] if x0 < x1 and y0 < y1 else [0, 0, r['width'], r['height']]
    quad = {
        'rect': rect,
        'ubos': [{**b, 'data': protocol.b64(b['data'])} for b in r['blocks']],
        'vertices': protocol.b64(r['vertices']),
        'primitive': r['primitive'],
        'first': r['first'],
        'count': r['count'],
    }
    if not r['vertices']:
        del quad['vertices'], quad['primitive'], quad['first'], quad['count']
    return [
        {
            'id': f'cap{serial}-{form}',
            'kind': 'pack',
            'target': {'format': NAMES[r['format']], 'width': r['width'], 'height': r['height']},
            'template': f'{r["key"]:016x}',
            'readback': 'full',
            'pack': {'form': form, 'ca': r['ca'], 'quads': [quad]},
            'meta': {'push': r['push']},
        }
        for form in protocol.PACK_FORMS
    ]


def batch(text: str, name: str, key: int | None = None) -> dict:
    """Give every captured record of `text` (of state `key` alone, when given) as a batch."""
    out = []
    for serial, blob in sorted(blobs(text).items()):
        r = record(blob)
        if key is None or r['key'] == key:
            out += experiments(serial, r)
    return protocol.batch(name, out[: protocol.MAX_EXPERIMENTS])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--key', type=lambda text: int(text, 16))
    args = parser.parse_args()
    b = batch(args.log.read_text(errors='replace'), args.out.stem.replace('_', '-'), args.key)
    args.out.write_text(json.dumps(b, indent=1, sort_keys=True) + '\n')
    print(f'{len(b["experiments"])} experiments')
    return 0


if __name__ == '__main__':
    sys.exit(main())
