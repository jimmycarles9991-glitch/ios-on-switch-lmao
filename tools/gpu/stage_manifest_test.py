import gzip
import hashlib
import importlib.util
import plistlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu import stage_format, stage_manifest

STAGER = Path(__file__).resolve().parents[2] / 'drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py'


def index(packs: int, observed: int, magic: bytes = b'SWSEEDPK') -> bytes:
    return struct.pack('<8sIIIIII', magic, 1, 8, 0, packs, observed, 0) + bytes(32)


def stager():
    spec = importlib.util.spec_from_file_location('stager', STAGER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(root: Path, kext: str = '') -> Path:
    """Write a small build's inputs below `root` and //gpu:bundle's manifest over them; return the manifest."""
    for name, data in {
        'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal': b'plugin',
        'base/ngpu/addons/TegraGM20BMetal.bundle/Info.plist': b'info',
        'seed/MANIFEST.json': b'{}',
        'raw/pack.bin': b'raw pipeline pack',
        'native/rec-0123456789abcdef/index': index(1, 1),
        'native/rec-0123456789abcdef/packs/000': b'records',
    }.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)
    manifest = root / 'bundle.stage'
    args = ['stage_manifest.py', '--self', 'bundle.stage', '--out', str(manifest)]
    for name in ('base', 'seed', 'raw', 'native'):
        args += ['--' + name, str(root / name), '--' + name + '-rlocation', name]
    if kext:
        (root / 'kext.txt').write_text(kext + '\n')
        args += ['--kext-digest', str(root / 'kext.txt')]
    with mock.patch.object(sys, 'argv', args):
        stage_manifest.main()
    return manifest


class StageManifestTest(unittest.TestCase):
    def test_packed_native_records_reach_the_runtime_lookup_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = 'rec-0123456789abcdef/'
            reply = 'msl/' + 'a' * 64 + '.reply'
            inputs = {
                'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal': b'plugin',
                'seed/' + reply: b'compiler reply',
                'seed/MANIFEST.json': b'{}',
                'raw/pack.bin': b'raw pipeline pack',
                'native/' + records + 'index': index(2, 1),
                'native/' + records + 'packs/000': b'native records',
            }
            host = {
                'native/manifest.json': b'native manifest',
                'native/coverage.json': b'{"complete":false}',
                'native/' + records + 'packs/001': b'derived native records',
            }
            for name, data in (inputs | host).items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            output = root / 'bundle.stage'
            args = ['stage_manifest.py', '--self', 'bundle.stage', '--out', str(output)]
            for name in ('base', 'seed', 'raw', 'native'):
                args += ['--' + name, str(root / name), '--' + name + '-rlocation', name]
            with mock.patch.object(sys, 'argv', args):
                stage_manifest.main()
            lines = output.read_text().splitlines()
            self.assertEqual(lines[0], '# gpu stage 2 runfiles bundle.stage')
            entries = [line.split('\t') for line in lines[1:]]
            files = {parts[1]: parts[2:] for parts in entries if parts[0] == 'file'}
            self.assertEqual(len(files), len(inputs))
            expected = {
                stage_manifest.RAW + '/native/' + records + 'index': 'native/' + records + 'index',
                stage_manifest.RAW + '/native/' + records + 'packs/000': 'native/' + records + 'packs/000',
                stage_manifest.RAW + '/pack.bin': 'raw/pack.bin',
                stage_manifest.SEED + '/' + reply: 'seed/' + reply,
            }
            for target, source in expected.items():
                data = inputs[source]
                self.assertEqual(files[target], [hashlib.sha256(data).hexdigest(), str(len(data)), '644', source])
            self.assertFalse([target for target in files if target.endswith(('manifest.json', 'coverage.json'))])
            self.assertNotIn(stage_manifest.RAW + '/native/' + records + 'packs/001', files)
            plugin = 'ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal'
            self.assertEqual(files[plugin][2], '755')
            for digest, length, _, source in files.values():
                data = (root / source).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
                self.assertEqual(len(data), int(length))

    def test_the_manifest_made_from_the_seeds_part_is_the_one_made_at_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in {
                'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal': b'plugin',
                'seed/MANIFEST.json': b'{}',
                'raw/pack.bin': b'raw pipeline pack',
                'native/rec-0123456789abcdef/index': index(1, 1),
                'native/rec-0123456789abcdef/packs/000': b'records',
            }.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(data)
            seed = []
            for name in ('seed', 'raw', 'native'):
                seed += ['--' + name, str(root / name), '--' + name + '-rlocation', name]
            base = ['--self', 'bundle.stage', '--base', str(root / 'base'), '--base-rlocation', 'base']
            runs = (
                [*base, *seed, '--out', str(root / 'once.stage')],
                ['--part', *seed, '--out', str(root / 'seed.part')],
                [*base, '--from-part', str(root / 'seed.part'), '--out', str(root / 'joined.stage')],
            )
            for args in runs:
                with mock.patch.object(sys, 'argv', ['stage_manifest.py', *args]):
                    stage_manifest.main()
            self.assertEqual((root / 'joined.stage').read_text(), (root / 'once.stage').read_text())
            self.assertEqual(len((root / 'once.stage').read_text().splitlines()), 1 + 5 + 18)

    def test_the_kext_identity_follows_the_header(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'base/ngpu/addons/TegraGM20BMetal.bundle').mkdir(parents=True)
            (root / 'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal').write_bytes(b'plugin')
            (root / 'seed.part').write_text('')
            (root / 'kext.txt').write_text('f00d\n')
            args = ['--self', 'bundle.stage', '--base', str(root / 'base'), '--base-rlocation', 'base']
            args += ['--from-part', str(root / 'seed.part'), '--kext-digest', str(root / 'kext.txt')]
            with mock.patch.object(sys, 'argv', ['stage_manifest.py', *args, '--out', str(root / 'out.stage')]):
                stage_manifest.main()
            lines = (root / 'out.stage').read_text().splitlines()
            self.assertEqual(lines[0], '# gpu stage 2 runfiles bundle.stage')
            self.assertEqual(lines[1], '# kext f00d')
            self.assertEqual(len(lines), 2 + 1 + 18)

    def test_a_packed_directory_without_a_seed_pack_index_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            native = Path(directory)
            name = 'rec-0123456789abcdef/index'
            (native / name).parent.mkdir(parents=True)
            (native / name).write_bytes(index(2, 1, b'NOTASEED'))
            with self.assertRaisesRegex(ValueError, 'seed pack'):
                stage_manifest.native_target(native, 'rec-0123456789abcdef/packs/000')


class StageFormat2Test(unittest.TestCase):
    def test_every_manifest_carries_backboardds_preferences(self):
        with tempfile.TemporaryDirectory() as directory:
            lines = build(Path(directory)).read_text().splitlines()
            prefs = [line.split('\t') for line in lines if line.startswith('pref\t')]
            self.assertEqual(
                {(plist, uid, key, value) for _, plist, uid, key, value in prefs},
                {
                    (f'{home}/Library/Preferences/{name}', str(uid), key, 'false')
                    for home, uid in (('mobile', 501), ('root', 0))
                    for name in ('com.apple.backboardd.plist', 'backboardd.plist', '.GlobalPreferences.plist')
                    for key in (
                        'CAEnableDeepFramebuffer',
                        'CAEnableFramebufferCompression',
                        'CAEnableIOSurfaceCompression',
                    )
                },
            )

    def test_the_tree_holds_each_file_at_its_path_with_the_same_records(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build(root, kext='f00d' * 16)
            tree = root / 'tree'
            roots = [f'{name}={root / name}' for name in ('base', 'seed', 'raw', 'native')]
            with mock.patch.object(
                sys,
                'argv',
                ['stage_manifest.py', '--tree', str(manifest), '--out', str(tree)] + [f'--root={r}' for r in roots],
            ):
                stage_manifest.main()
            runfiles = manifest.read_text().splitlines()
            ours = (tree / stage_format.TREE_MANIFEST).read_text().splitlines()
            self.assertEqual(ours[0], '# gpu stage 2 tree')
            self.assertEqual(ours[1], '# kext ' + 'f00d' * 16)
            self.assertEqual(len(ours), len(runfiles))
            for theirs, mine in zip(runfiles[1:], ours[1:], strict=True):
                if theirs.startswith(stage_format.KEXT):
                    self.assertEqual(theirs, mine)
                    continue
                kind, *fields = theirs.split('\t')
                if kind == 'file':
                    fields[-1] = fields[0]
                self.assertEqual('\t'.join([kind, *fields]), mine)
            for path, mode, source in stage_format.files(runfiles):
                self.assertEqual((tree / path).read_bytes(), (root / source).read_bytes())
                self.assertEqual((tree / path).stat().st_mode & 0o777, mode)
            listed = {path for path, _, _ in stage_format.files(runfiles)} | {stage_format.TREE_MANIFEST}
            self.assertEqual({p.relative_to(tree).as_posix() for p in tree.rglob('*') if p.is_file()}, listed)

    def test_the_stager_stages_the_tree_and_the_runfiles_manifest_alike(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = build(root, kext='beef' * 16)
            tree = root / 'tree'
            roots = [f'--root={name}={root / name}' for name in ('base', 'seed', 'raw', 'native')]
            with mock.patch.object(
                sys, 'argv', ['stage_manifest.py', '--tree', str(manifest), '--out', str(tree), *roots]
            ):
                stage_manifest.main()
            module = stager()
            staged, states = [], []
            for name, source, read in (
                ('runfiles', manifest, lambda: module.from_manifest(manifest)),
                ('tree', tree, lambda: module.from_directory(tree)),
            ):
                data = root / f'data-{name}'
                (data / 'mobile').mkdir(parents=True)
                wanted, prefs = read()
                module.sync(data, wanted, lambda _: None, module.manifest_kext(source))
                module._merge_preferences(data, prefs)
                staged.append(
                    {
                        p.relative_to(data).as_posix(): (p.read_bytes(), p.stat().st_mode & 0o777)
                        for p in data.rglob('*')
                        if p.is_file() and p.name != 'stage.tsv.gz'
                    }
                )
                with gzip.open(data / module.STATE, 'rt') as handle:
                    states.append(handle.read())
                plist = plistlib.loads((data / 'mobile/Library/Preferences/com.apple.backboardd.plist').read_bytes())
                self.assertEqual(plist, stage_format.PREFS)
            self.assertEqual(staged[0], staged[1])
            self.assertEqual(states[0], states[1])

    def test_a_directory_without_a_stage_manifest_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'ngpu').mkdir()
            with self.assertRaises(SystemExit) as refused:
                stager().from_directory(Path(directory))
            self.assertIn('gpu.stage', str(refused.exception))

    def test_a_format_1_manifest_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / 'bundle.stage'
            manifest.write_text('# gpu stage 1 bundle.stage\n')
            with self.assertRaises(SystemExit):
                stager().from_manifest(manifest)
            with self.assertRaises(ValueError):
                stage_format.as_tree(manifest.read_text().splitlines())
            with self.assertRaises(ValueError):
                stage_format.files(manifest.read_text().splitlines())


if __name__ == '__main__':
    unittest.main()
