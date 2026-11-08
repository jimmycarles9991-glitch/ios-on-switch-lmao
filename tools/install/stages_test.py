"""A run resumes only from a stage made of the same inputs, and a boot refresh only when Data's parts still match."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.install import folder, manifest, sii_fixture, stages

SPEC = {'volumes': {'system': {'uuid': 'S'}, 'data': {'uuid': 'D'}, 'preboot': {'uuid': 'P'}}}
PINS = {'build': '21H461', 'members': {'os': {'content_sha256': 'abc'}}}


def quiet(*parts: object):
    pass


class ScratchTest(unittest.TestCase):
    BUILDS = ('21H461', '24B119', '23F84', '22A3354')

    def test_legacy_build_keeps_the_unsuffixed_directory(self):
        self.assertEqual(stages.default_scratch('21H461').name, 'image')

    def test_each_build_has_its_own_directory(self):
        dirs = [stages.default_scratch(b) for b in self.BUILDS]
        self.assertEqual(len(set(dirs)), len(dirs))


class BazelOutputsTest(unittest.TestCase):
    def test_deltas_are_the_roles_bazel_made(self) -> None:
        args = SimpleNamespace(scratch='/scratch', delta_os='/out/delta_os', delta_app_cryptex=None)
        deltas = stages.Stages(args, SPEC, PINS, quiet).deltas()
        self.assertIn('os', deltas)
        self.assertNotIn('app_cryptex', deltas)
        self.assertEqual(deltas.kept, Path('/scratch') / stages.KEPT)

    def test_an_ipsw_without_translation_has_no_deltas(self) -> None:
        self.assertIsNone(stages.Stages(SimpleNamespace(scratch='/s'), SPEC, PINS, quiet).deltas())

    def test_a_translated_cryptex_comes_with_its_sha256(self) -> None:
        work = Path(tempfile.mkdtemp())
        image = work / 'app.dmg'
        image.write_bytes(b'sealed')
        Path(str(image) + '.sha256').write_text('ab' * 32 + '\n')
        args = SimpleNamespace(scratch=str(work), system_cryptex='/ipsw/sys.dmg', app_cryptex='/ipsw/app.dmg')
        args.translated_app_cryptex = str(image)
        sources, expected = stages.Stages(args, SPEC, PINS, quiet).cryptexes()
        self.assertEqual(sources, {'system_cryptex': '/ipsw/sys.dmg', 'app_cryptex': str(image)})
        self.assertEqual(expected, {'app_cryptex': 'ab' * 32})


class StateTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.work = stages.Stages(SimpleNamespace(scratch=str(self.dir)), SPEC, PINS, quiet)

    def test_a_stage_counts_while_its_image_is_there(self):
        (self.dir / stages.COMPOSED).write_bytes(b'image')
        self.work.save(compose='c1', offset=1 << 20)
        self.assertEqual(self.work.state(), {'offset': 1 << 20, 'compose': 'c1'})
        self.work.save(populate='p1')
        self.assertNotIn('populate', self.work.state())
        (self.dir / stages.POPULATED).write_bytes(b'clone')
        self.assertEqual(self.work.state()['populate'], 'p1')
        (self.dir / stages.COMPOSED).unlink()
        self.assertNotIn('compose', self.work.state())


class CleanTest(unittest.TestCase):
    def test_leftovers_and_partials_go_and_root_owned_ones_are_named(self) -> None:
        scratch = Path(tempfile.mkdtemp())
        (scratch / 'delta-os').mkdir()
        (scratch / 'delta-os/f').write_bytes(b'x')
        (scratch / '141-10410-227.dmg').write_bytes(b'rebuilt')
        (scratch / '.partial-composed.img').write_bytes(b'half')
        (scratch / 'populated.img.partial').write_bytes(b'half')
        (scratch / stages.COMPOSED).write_bytes(b'kept')
        args = SimpleNamespace(scratch=str(scratch), system_cryptex='/ipsw/141-10410-227.dmg', app_cryptex=None)
        lines = []
        work = stages.Stages(args, SPEC, PINS, lambda *p: lines.append(' '.join(map(str, p))))
        real_unlink = Path.unlink

        def unlink(path: Path, missing_ok: bool = False) -> None:
            if path.name.endswith('.dmg'):
                raise PermissionError(path)
            real_unlink(path, missing_ok=missing_ok)

        with (
            mock.patch.object(stages.disks.host, 'plist', return_value={'images': []}),
            mock.patch.object(Path, 'unlink', unlink),
        ):
            work.clean()
        self.assertFalse((scratch / 'delta-os').exists())
        self.assertFalse((scratch / '.partial-composed.img').exists())
        self.assertFalse((scratch / 'populated.img.partial').exists())
        self.assertTrue((scratch / stages.COMPOSED).exists())
        self.assertTrue(any('sudo rm -rf' in line and '141-10410-227.dmg' in line for line in lines), lines)

    def test_state_is_written_whole(self) -> None:
        scratch = Path(tempfile.mkdtemp())
        work = stages.Stages(SimpleNamespace(scratch=str(scratch)), SPEC, PINS, quiet)
        work.save(compose='c', offset=1)
        self.assertEqual([p.name for p in scratch.iterdir()], [stages.STATE])


class MinimumTest(unittest.TestCase):
    def test_room_to_run_and_ios_floor(self):
        layout = {'container_bytes': 1 << 40, 'minimum_bytes': 9 << 30}
        self.assertEqual(stages.minimum_bytes(layout), 16 << 30)
        layout['minimum_bytes'] = (20 << 30) + 5
        self.assertEqual(stages.minimum_bytes(layout), (24 << 30) + 4096)
        self.assertEqual(stages.minimum_bytes({'container_bytes': 8 << 30, 'minimum_bytes': 1 << 30}), 8 << 30)


class HeadTest(unittest.TestCase):
    def test_new_boot_files_change_the_head_only(self):
        out = folder.prepare(Path(tempfile.mkdtemp()) / 'sd')
        sii_fixture.make(out, 'IOS17', bytes([7]) * (90 << 20), {'bl33.bin': b'kernel 1'})
        parts = sorted((out / folder.SII_DIR).glob('IOS17.SII.*'))
        before = [p.read_bytes() for p in parts]
        package = out.parent / 'kernel'
        package.write_bytes(b'kernel 2, longer')
        m = stages.refresh_head(out, 'IOS17', [('bl33.bin', package)], 'iPadOS 17.7.11 (21H461)')
        after = [p.read_bytes() for p in parts]
        self.assertEqual(after[1:], before[1:])
        self.assertEqual(after[0][manifest.BODY_OFFSET :], before[0][manifest.BODY_OFFSET :])
        self.assertEqual(manifest.decode(after[0][: manifest.BOOT_OFFSET]), m)
        start = m.boot[0].offset
        self.assertEqual(after[0][start : start + 16], b'kernel 2, longer')

    def test_boot_files_are_the_packages_boot_entry(self):
        package = Path(tempfile.mkdtemp())
        for relative in ('switchroot/ios/bl33.bin', 'switchroot/ios/bl31.bin', 'bootloader/ini/ios.ini'):
            (package / relative).parent.mkdir(parents=True, exist_ok=True)
            (package / relative).write_bytes(b'x')
        self.assertEqual([n for n, _ in stages.boot_files(package)], ['bl31.bin', 'bl33.bin'])


class IpswVolumesTest(unittest.TestCase):
    def test_the_pins_name_system_and_the_cryptexes(self):
        spec = {
            'volumes': {'system': {'uuid': 'OLD'}, 'data': {'uuid': 'D'}, 'preboot': {'uuid': 'P'}},
            'cryptexes': [
                {'member': 'system_cryptex', 'path': '/private/preboot/Switch-Staging/a.dmg', 'volume_uuid': 'A'},
                {'member': 'app_cryptex', 'path': '/private/preboot/Switch-Staging/b.dmg', 'volume_uuid': 'B'},
            ],
        }
        pins = {
            'volumes': {
                'os_volume_uuid': 'd05a9d99-d227-4757-9f58-c32593d3bfa7',
                'system_cryptex_uuid': 'adb8fd34-da7e-4538-9aaf-4f05ebb09057',
                'app_cryptex_uuid': 'd8b9f68e-f191-4fcd-9f89-d88a322e9b4d',
                'system_cryptex_image': '141-10410-227.dmg',
                'app_cryptex_image': '141-08824-244.dmg',
            }
        }
        out = stages.ipsw_volumes(spec, pins)
        self.assertEqual(out['volumes']['system']['uuid'], 'D05A9D99-D227-4757-9F58-C32593D3BFA7')
        self.assertEqual(out['volumes']['data']['uuid'], 'D')
        self.assertEqual(out['cryptexes'][0]['path'], '/private/preboot/Switch-Staging/141-10410-227.dmg')
        self.assertEqual(out['cryptexes'][1]['volume_uuid'], 'D8B9F68E-F191-4FCD-9F89-D88A322E9B4D')
        self.assertEqual(spec['volumes']['system']['uuid'], 'OLD')
        self.assertIs(stages.ipsw_volumes(spec, {}), spec)


if __name__ == '__main__':
    unittest.main()
