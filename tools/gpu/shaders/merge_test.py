"""The replies the seed keeps, and the tree the image builder lays out of them, the native records and the pack."""

import argparse
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu.shaders import merge, replies as compiler_replies


def reply(metallib: bytes, warnings: bytes = b'') -> bytes:
    payload = warnings + metallib
    return (
        compiler_replies.MAGIC
        + struct.pack('<IQ', len(warnings), len(metallib))
        + hashlib.sha256(payload).digest()
        + payload
    )


class SeedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_packed_native_seed_is_installed_beside_the_raw_pack_without_its_host_files(self):
        seed, raw, native = (self.root / name for name in ('seed', 'raw', 'native'))
        for folder in (seed, raw, native / 'rec-0000000000001234' / 'packs'):
            folder.mkdir(parents=True)
        (seed / 'MANIFEST.json').write_text('{}')
        (raw / 'pack.bin').write_bytes(b'raw pack')
        record_index = struct.pack('<8sIIIIII', b'SWSEEDPK', 1, 8, 0, 2, 1, 0) + bytes(32)
        packed = {
            'rec-0000000000001234/index': record_index,
            'rec-0000000000001234/packs/000': b'native records',
            'rec-0000000000001234/packs/001': b'derived native records',
        }
        for relative, data in packed.items():
            (native / relative).write_bytes(data)
        (native / 'coverage.json').write_bytes(b'{}')
        (native / 'manifest.json').write_bytes(b'{}')
        out = self.root / 'tree'
        size = self.root / 'size.json'
        merge.tree(argparse.Namespace(seed=seed, raw=raw, native=native, out=out, size=size))
        records = out / merge.RAW / 'native' / 'rec-0000000000001234'
        self.assertEqual((records / 'packs' / '000').read_bytes(), b'native records')
        self.assertEqual((records / 'index').stat().st_mode & 0o777, 0o644)
        self.assertEqual((out / merge.RAW / 'native').stat().st_mode & 0o777, 0o755)
        self.assertEqual((out / merge.RAW / 'pack.bin').read_bytes(), b'raw pack')
        self.assertFalse((records / 'packs' / '001').exists())
        self.assertFalse((out / merge.RAW / 'native' / 'coverage.json').exists())
        self.assertFalse((out / merge.RAW / 'native' / 'manifest.json').exists())
        self.assertEqual(sorted(p.name for p in (out / merge.SEED).iterdir()), ['MANIFEST.json'])
        self.assertEqual(json.loads(size.read_text())['files'], 4)

    def test_the_replies_keep_those_whose_envelope_holds(self):
        replies = self.root / 'msl'
        replies.mkdir()
        good = reply(b'MTLB a kernel', b'a warning')
        (replies / ('a' * 64 + '.reply')).write_bytes(good)
        (replies / ('b' * 64 + '.reply')).write_bytes(good[:-1] + b'?')
        (replies / 'c.reply').write_bytes(good)
        out = self.root / 'seed'
        args = argparse.Namespace(replies=replies, reply_input=None, strict_replies=False, ipsw='21H461', out=out)
        merge.replies(args)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['MANIFEST.json', 'msl'])
        self.assertEqual(sorted(p.name for p in (out / 'msl').iterdir()), ['a' * 64 + '.reply'])
        self.assertEqual((out / 'msl' / ('a' * 64 + '.reply')).read_bytes(), good)
        manifest = json.loads((out / 'MANIFEST.json').read_text())
        self.assertEqual(manifest['ipsw'], '21H461')
        self.assertEqual(manifest['compile_replies'], {'kept': 1, 'refused': 2})
        self.assertEqual(manifest['compiler_reply_inventory']['unattributedReplies'], 1)
        args.strict_replies = True
        with self.assertRaisesRegex(ValueError, 'invalid reply'):
            merge.replies(args)

    def test_a_build_without_the_devices_replies_keeps_only_the_manifest(self):
        out = self.root / 'seed'
        merge.replies(argparse.Namespace(replies=None, reply_input=None, strict_replies=False, ipsw='24B119', out=out))
        self.assertEqual([p.name for p in out.iterdir()], ['MANIFEST.json'])
        self.assertEqual(json.loads((out / 'MANIFEST.json').read_text())['compile_replies'], {'kept': 0, 'refused': 0})

    def test_installer_checks_copied_reply_bytes(self):
        seed = self.root / 'seed'
        (seed / 'msl').mkdir(parents=True)
        path = seed / 'msl' / ('a' * 64 + '.reply')
        path.write_bytes(reply(b'MTLB fixture'))
        inventory = compiler_replies.inventory(seed / 'msl', strict=True)
        (seed / 'MANIFEST.json').write_text(json.dumps({'compiler_reply_inventory': inventory}))
        args = argparse.Namespace(seed=seed, raw=None, native=None, out=self.root / 'tree', size=self.root / 'size')
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
