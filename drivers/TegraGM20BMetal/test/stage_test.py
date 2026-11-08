"""The GPU stager on a plain directory standing in for the Data volume.

It copies only what changed, reads nothing when the card's list matches the build, and a cut-short stage is finished by
the next one.
"""

import gzip
import importlib.util
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    'stage', Path(__file__).resolve().parents[1] / 'stage-tegra-gm20b-metal.py'
)
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)

SELF = '_main/gpu/bundle.stage'
SEED = 'ngpu/addons/TegraGM20BMetal.m2v-seed/shared'


class StageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.runfiles = self.root / 'runfiles'
        self.data = self.root / 'data'
        (self.data / 'mobile').mkdir(parents=True)
        self.files = {
            stage.BUNDLE + '/TegraGM20BMetal': b'plugin',
            stage.BUNDLE + '/Info.plist': b'plist',
            SEED + '/index': b'index',
            SEED + '/packs/000': b'pack zero',
            SEED + '/packs/001': b'pack one',
            stage.RAW + '/pack.bin': b'raw',
        }

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self) -> Path:
        lines = []
        for path, data in sorted(self.files.items()):
            source = self.runfiles / '_main/out' / path
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(data)
            mode = '755' if path.endswith('/TegraGM20BMetal') else '644'
            digest = stage.hashlib.sha256(data).hexdigest()
            lines.append(f'{path}\t{digest}\t{len(data)}\t{mode}\t_main/out/{path}')
        manifest = self.runfiles / SELF
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(stage.MANIFEST_HEADER + SELF + '\n' + ''.join(line + '\n' for line in lines))
        return manifest

    def sync(self) -> str:
        return stage.sync(self.data, stage.from_manifest(self.manifest()), lambda _: None)

    def card(self) -> dict[str, bytes]:
        top = self.data / 'ngpu/addons'
        return {p.relative_to(self.data).as_posix(): p.read_bytes() for p in top.rglob('*') if p.is_file()}

    def test_first_stage_replaces_the_directories_and_copies_everything(self):
        (self.data / SEED / 'v1/aa').mkdir(parents=True)
        (self.data / SEED / 'v1/aa/old').write_bytes(b'one file per entry')
        self.assertTrue(self.sync().startswith('6 copied'))
        self.assertEqual(self.card(), self.files)
        self.assertEqual((self.data / stage.BUNDLE / 'TegraGM20BMetal').stat().st_mode & 0o777, 0o755)
        self.assertFalse((self.data / stage.PENDING).exists())

    def test_an_unchanged_build_reads_nothing_but_the_lists_first_line(self):
        self.sync()
        manifest = self.manifest()
        for source in (self.runfiles / '_main/out').rglob('*'):
            if source.is_file():
                source.unlink()
        self.assertTrue(stage.sync(self.data, stage.from_manifest(manifest), lambda _: None).startswith('unchanged'))

    def test_a_change_copies_that_file_and_a_dropped_file_goes(self):
        self.sync()
        self.files[SEED + '/packs/001'] = b'pack one, recompiled'
        del self.files[stage.RAW + '/pack.bin']
        self.assertTrue(self.sync().startswith('1 copied (0 MiB), 1 removed, 4 unchanged'))
        self.assertEqual(self.card(), self.files)
        self.assertFalse((self.data / stage.RAW).exists(), 'an emptied directory goes too')

    def test_a_cut_short_stage_is_finished_by_the_next(self):
        self.sync()
        (self.data / SEED / 'packs/000').write_bytes(b'half wri')
        (self.data / stage.PENDING).write_text(SEED + '/packs/000\n')
        self.assertTrue(self.sync().startswith('1 copied'))
        self.assertEqual(self.card(), self.files)

    def test_the_list_names_what_is_staged(self):
        self.sync()
        with gzip.open(self.data / stage.STATE, 'rt') as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[0], '# ' + stage.digest(stage.from_manifest(self.manifest())))
        self.assertEqual(sorted(line.split('\t')[0] for line in lines[1:]), sorted(self.files))

    def test_a_directory_build_stages_as_its_manifest_does(self):
        build = self.root / 'base'
        for path, data in self.files.items():
            (build / path).parent.mkdir(parents=True, exist_ok=True)
            (build / path).write_bytes(data)
        (build / 'TegraGM20BMetal.trustcache').write_bytes(b'trust')
        self.assertEqual(stage.digest(stage.from_directory(build)), stage.digest(stage.from_manifest(self.manifest())))


if __name__ == '__main__':
    unittest.main()
