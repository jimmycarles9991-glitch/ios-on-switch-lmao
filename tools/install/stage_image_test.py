"""Unit tests for staging a packed image into out/sd and running image.Builder as an ephemeral Bazel action."""

import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import folder, host, pack_action, stage_image


class StageImageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.log = host.Log(self.root / 'stage.log')

    def tearDown(self):
        self.tmp.cleanup()

    def _make_packed(self, name: str, parts: list[bytes], pretranslated: bool = False) -> Path:
        packed = self.root / f'packed-{name}'
        folder.prepare(packed)
        for index, data in enumerate(parts, start=1):
            path = packed / folder.SII_DIR / f'{name}.SII.{index:03d}'
            path.write_bytes(data)
            path.chmod(0o444)
        for relative in folder.SHARED:
            dst = packed / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(f'{name}:{relative}'.encode())
            dst.chmod(0o444)
        if pretranslated:
            pre = packed / 'pretranslated'
            pre.mkdir()
            tc = pre / 'os.trustcache'
            tc.write_bytes(b'trust')
            tc.chmod(0o444)
        folder.record(
            packed,
            name,
            {
                'pack': f'pack-{name}',
                'head': f'head-{name}',
                'label': f'label-{name}',
                'boot': {'overlay': 'a', 'bootstrap': 'b', 'gpu_trustcache': 'c'},
                'minimum_bytes': 20 << 30,
            },
        )
        return packed

    def test_stage_merges_versions_removes_stale_parts_and_makes_writable(self):
        out = self.root / 'out'
        packed17 = self._make_packed('IOS17', [b'ios17-p1', b'ios17-p2'])
        stage_image.stage(packed17, out, self.log)
        stale = out / folder.SII_DIR / 'IOS27.SII.003'
        stale.write_bytes(b'stale')

        packed27 = self._make_packed('IOS27', [b'ios27-p1', b'ios27-p2'], pretranslated=True)
        self.assertEqual(stage_image.stage(packed27, out, self.log), 'IOS27')

        self.assertFalse(stale.exists())
        self.assertEqual((out / folder.SII_DIR / 'IOS17.SII.001').read_bytes(), b'ios17-p1')
        self.assertEqual((out / folder.SII_DIR / 'IOS27.SII.001').read_bytes(), b'ios27-p1')
        self.assertEqual((out / folder.SII_DIR / 'IOS27.SII.002').read_bytes(), b'ios27-p2')
        mode = stat.S_IMODE((out / folder.SII_DIR / 'IOS27.SII.001').stat().st_mode)
        self.assertEqual(mode, 0o644)
        self.assertEqual((out / 'pretranslated/os.trustcache').read_bytes(), b'trust')
        self.assertEqual(stat.S_IMODE((out / 'pretranslated/os.trustcache').stat().st_mode), 0o644)
        self.assertEqual(sorted(folder.records(out)), ['IOS17', 'IOS27'])


class PackActionTest(unittest.TestCase):
    def test_prepare_args_resolves_gpu_base_file_and_cleans_scratch_on_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gpu_dir = root / 'gpu'
            gpu_dir.mkdir()
            trust = gpu_dir / 'trust.json'
            trust.write_text('{}')
            out = root / 'out'
            args = pack_action.parse(['--out', str(out), '--gpu-base', str(trust)])
            captured = {}

            class FakeBuilder:
                def __init__(self, builder_args: object, _log: object) -> None:
                    captured['gpu_base'] = builder_args.gpu_base
                    captured['scratch'] = Path(builder_args.scratch)

                def run(self) -> int:
                    (captured['scratch'] / 'composed.img').write_bytes(b'x')
                    raise host.HostError('boom')

            with (
                mock.patch.object(pack_action.host, 'check_tools'),
                mock.patch.object(pack_action.disks, 'sweep') as sweep,
                mock.patch.object(pack_action.image, 'Builder', FakeBuilder),
            ):
                rc = pack_action.run(args)
            self.assertEqual(rc, 1)
            self.assertEqual(captured['gpu_base'], os.path.abspath(str(gpu_dir)))
            self.assertFalse(captured['scratch'].exists())
            sweep.assert_called_once()


if __name__ == '__main__':
    unittest.main()
