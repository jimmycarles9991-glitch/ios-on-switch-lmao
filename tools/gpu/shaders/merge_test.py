"""The packed seed the join writes.

Every bucket's entries are found where its pack holds them, the first bucket's where two name one, and the device's
records of the seed's identity in the last pack.
"""

import argparse
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.gpu.shaders import merge, precompile

BUILD = 'b' * 64
CONTENT = merge.CONTENT
IDENTITY = bytes(range(28))


def record(payload: bytes) -> bytes:
    body = merge.RECORD_MAGIC + IDENTITY + struct.pack('<Q', len(payload)) + payload
    return body + hashlib.sha256(body).digest()


def read_index(path: Path) -> tuple[str, dict[tuple[bytes, int], tuple[int, int, int]]]:
    """Read the index back, and look every entry up through its run, as seed.rs does."""
    data = path.read_bytes()
    magic, version, _, count = struct.unpack_from('<8sIIQ', data)
    assert (magic, version) == (merge.INDEX_MAGIC, merge.INDEX_VERSION), (magic, version)
    (bits,) = struct.unpack_from('<I', data, 56)
    starts = struct.unpack_from(f'<{(1 << bits) + 1}I', data, 64)
    records = 64 + 4 * len(starts)
    assert len(data) == records + 48 * count
    entries = {}
    for i in range(count):
        name, space, _, pack, size, offset = struct.unpack_from('<32sBBHIQ', data, records + 48 * i)
        run = int.from_bytes(name[:4], 'big') >> (32 - bits) if bits else 0
        assert starts[run] <= i < starts[run + 1], 'every entry in its run'
        entries[name, space] = (pack, offset, size)
    return data[24:56].hex(), entries


def entry(n: int) -> str:
    return f'v1/{BUILD}/{n:02x}/{n:02x}' + 'e' * 62


class PrefixTest(unittest.TestCase):
    def test_runs_hold_a_few_entries(self):
        self.assertEqual(merge.prefix_bits(0), 0)
        self.assertEqual(merge.prefix_bits(112637), 15)


class JoinTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def bucket(self, number: int, files: dict[str, bytes]) -> tuple[Path, Path, Path]:
        cache = self.root / f'cache{number}'
        for relative, data in files.items():
            (cache / relative).parent.mkdir(parents=True, exist_ok=True)
            (cache / relative).write_bytes(data)
        pack = self.root / f'{number:03d}.pack'
        lines, identity = precompile.pack(cache, pack)
        index, report = self.root / f'{number:03d}.index', self.root / f'{number:03d}.json'
        index.write_text(''.join(lines))
        report.write_text(json.dumps({'identity': identity}))
        return pack, index, report

    def test_join_packs_the_buckets(self):
        shared = record(b'one pipeline, made by two buckets')
        named = f'v1/{CONTENT}/cc/cc' + 'c' * 62
        buckets = [
            self.bucket(0, {entry(1): b'first', named: shared}),
            self.bucket(1, {entry(2): b'second', named: shared}),
        ]
        records = self.root / 'records/dd'
        records.mkdir(parents=True)
        (records / ('dd' + 'd' * 62)).write_bytes(record(b'the device made this one'))
        for name, value in (
            ('plan.json', {}),
            (
                'inventory.json',
                {'files': 0, 'embedded_libraries': [], 'unreadable': [], 'libraries': [], 'msl_sources': {}},
            ),
        ):
            (self.root / name).write_text(json.dumps(value))
        out = self.root / 'seed'
        merge.join(
            argparse.Namespace(
                index=[b[1] for b in buckets],
                report=[b[2] for b in buckets],
                records=self.root / 'records',
                ipsw='21H461',
                plan=self.root / 'plan.json',
                inventory=self.root / 'inventory.json',
                out=out,
            )
        )
        build, entries = read_index(out / 'index')
        self.assertEqual(build, BUILD)
        packs = [b[0] for b in buckets] + [out / 'packs/002']

        def read(relative: str) -> bytes:
            _, directory, _, name = relative.split('/')
            name, space = bytes.fromhex(name), int(directory == CONTENT)
            pack, offset, size = entries[name, space]
            return packs[pack].read_bytes()[offset : offset + size]

        self.assertEqual(read(entry(1)), b'first')
        self.assertEqual(read(entry(2)), b'second')
        self.assertEqual(read(named), shared)
        self.assertEqual(entries[bytes.fromhex(named.rsplit('/', 1)[-1]), 1][0], 0, 'the first bucket to name it')
        self.assertEqual(read(f'v1/{CONTENT}/dd/dd' + 'd' * 62), record(b'the device made this one'))
        self.assertIn('generated m2v-build bbbbbbbb', (out / 'SEED').read_text())


if __name__ == '__main__':
    unittest.main()
