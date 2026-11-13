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
from unittest import mock

from tools.gpu.shaders import merge, precompile, replies as compiler_replies

BUILD = 'b' * 64
CONTENT = merge.CONTENT
IDENTITY = bytes(range(28))


def record(payload: bytes) -> bytes:
    body = merge.RECORD_MAGIC + IDENTITY + struct.pack('<Q', len(payload)) + payload
    return body + hashlib.sha256(body).digest()


def reply(metallib: bytes, warnings: bytes = b'') -> bytes:
    payload = warnings + metallib
    return (
        merge.REPLY_MAGIC
        + struct.pack('<IQ', len(warnings), len(metallib))
        + hashlib.sha256(payload).digest()
        + payload
    )


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


class RefusalsTest(unittest.TestCase):
    def test_a_refusal_reads_the_same_wherever_its_bucket_ran(self):
        library = 'System/Library/PrivateFrameworks/GESS.framework/mlx.metallib'
        sandbox = '/Users/u/Library/Caches/bazel/_bazel_u/0123/sandbox/darwin-sandbox/21961/execroot/_main/_bucket_001'
        lines = [f'C kernel ({where}/root/{library}): compute pipeline' for where in (sandbox + '/store', 'store')]
        grouped = merge.refusals(lines)
        self.assertEqual(list(grouped), ['/' + library])
        self.assertEqual(grouped['/' + library]['first'], [f'C kernel (/{library}): compute pipeline'] * 2)


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

    def test_packed_native_seed_is_installed_beside_the_raw_pack_without_its_host_files(self):
        seed, raw, native = (self.root / name for name in ('seed', 'raw', 'native'))
        for folder in (seed, raw, native / 'raw-0000000000001234' / 'packs', native / 'native-analysis' / 'packs'):
            folder.mkdir(parents=True)
        (seed / 'SEED').write_bytes(b'identity')
        (raw / 'pack.bin').write_bytes(b'raw pack')
        record_index = struct.pack('<8sIIIIII', b'SWSEEDPK', 1, 8, 0, 2, 1, 0) + bytes(32)
        analysis_index = struct.pack('<8sIIIIII', b'SWSEEDPK', 1, 32, 0, 3, 2, 0) + bytes(32)
        packed = {
            'raw-0000000000001234/index': record_index,
            'raw-0000000000001234/packs/000': b'native records',
            'raw-0000000000001234/packs/001': b'derived native records',
            'native-analysis/index': analysis_index,
            'native-analysis/packs/000': b'native compute analyses',
            'native-analysis/packs/001': b'more native compute analyses',
            'native-analysis/packs/002': b'derived native compute analyses',
        }
        for relative, data in packed.items():
            (native / relative).write_bytes(data)
        (native / 'coverage.json').write_bytes(b'{}')
        (native / 'manifest.json').write_bytes(b'{}')
        out = self.root / 'tree'
        size = self.root / 'size.json'
        merge.tree(argparse.Namespace(seed=seed, raw=raw, native=native, pack=[], out=out, size=size))
        records = out / merge.RAW / 'native' / 'raw-0000000000001234'
        self.assertEqual((records / 'packs' / '000').read_bytes(), b'native records')
        self.assertEqual((records / 'index').stat().st_mode & 0o777, 0o644)
        self.assertEqual((out / merge.RAW / 'native').stat().st_mode & 0o777, 0o755)
        self.assertEqual((out / merge.RAW / 'pack.bin').read_bytes(), b'raw pack')
        analyses = out / merge.SEED / 'native-analysis'
        self.assertEqual((analyses / 'packs' / '000').read_bytes(), b'native compute analyses')
        self.assertEqual((analyses / 'packs' / '001').read_bytes(), b'more native compute analyses')
        self.assertEqual((analyses / 'index').read_bytes(), analysis_index)
        self.assertFalse((records / 'packs' / '001').exists())
        self.assertFalse((analyses / 'packs' / '002').exists())
        self.assertFalse((out / merge.RAW / 'native' / 'native-analysis').exists())
        self.assertFalse(list(out.rglob('*.json')))
        self.assertEqual(json.loads(size.read_text())['files'], 7)

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
        replies = self.root / 'msl'
        replies.mkdir()
        good = reply(b'MTLB a kernel', b'a warning')
        (replies / ('a' * 64 + '.reply')).write_bytes(good)
        (replies / ('b' * 64 + '.reply')).write_bytes(good[:-1] + b'?')
        (replies / 'c.reply').write_bytes(good)
        for name, value in (
            ('plan.json', {}),
            (
                'inventory.json',
                {'files': 0, 'embedded_libraries': [], 'unreadable': [], 'libraries': [], 'msl_sources': {}},
            ),
        ):
            (self.root / name).write_text(json.dumps(value))
        out = self.root / 'seed'
        args = argparse.Namespace(
            index=[b[1] for b in buckets],
            report=[b[2] for b in buckets],
            records=self.root / 'records',
            replies=replies,
            ipsw='21H461',
            plan=self.root / 'plan.json',
            inventory=self.root / 'inventory.json',
            out=out,
        )
        merge.join(args)
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
        self.assertEqual(sorted(p.name for p in (out / 'msl').iterdir()), ['a' * 64 + '.reply'])
        self.assertEqual((out / 'msl' / ('a' * 64 + '.reply')).read_bytes(), good)
        manifest = json.loads((out / 'MANIFEST.json').read_text())
        self.assertEqual(manifest['compile_replies'], {'kept': 1, 'refused': 2})
        self.assertEqual(manifest['compiler_reply_inventory']['unattributedReplies'], 1)
        args.strict_replies = True
        with self.assertRaisesRegex(ValueError, 'invalid reply'):
            merge.join(args)

    def test_installer_checks_copied_reply_bytes(self):
        seed = self.root / 'seed'
        (seed / 'msl').mkdir(parents=True)
        path = seed / 'msl' / ('a' * 64 + '.reply')
        path.write_bytes(reply(b'MTLB fixture'))
        inventory = compiler_replies.inventory(seed / 'msl', strict=True)
        (seed / 'MANIFEST.json').write_text(json.dumps({'compiler_reply_inventory': inventory}))
        args = argparse.Namespace(
            seed=seed, pack=[], raw=None, native=None, out=self.root / 'tree', size=self.root / 'size'
        )
        merge.tree(args)
        self.assertEqual((args.out / merge.SEED / 'msl' / path.name).read_bytes(), path.read_bytes())
        clone = merge.clone

        def damaged(source: Path, target: Path) -> None:
            clone(source, target)
            if target.suffix == '.reply':
                target.write_bytes(reply(b'other valid output'))

        with mock.patch.object(merge, 'clone', side_effect=damaged), self.assertRaisesRegex(ValueError, 'differ'):
            merge.tree(args)


if __name__ == '__main__':
    unittest.main()
