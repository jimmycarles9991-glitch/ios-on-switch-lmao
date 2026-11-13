import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu import stage_manifest


def index(packs: int, observed: int, magic: bytes = b'SWSEEDPK') -> bytes:
    return struct.pack('<8sIIIIII', magic, 1, 8, 0, packs, observed, 0) + bytes(32)


class StageManifestTest(unittest.TestCase):
    def test_packed_native_records_and_analysis_reach_the_runtime_lookup_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = 'raw-0123456789abcdef/'
            analysis = 'native-analysis/'
            inputs = {
                'base/ngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal': b'plugin',
                'seed/SEED': b'compatible seed',
                'seed/MANIFEST.json': b'{}',
                'pack': b'compatibility records',
                'raw/pack.bin': b'raw pipeline pack',
                'native/' + records + 'index': index(2, 1),
                'native/' + records + 'packs/000': b'native records',
                'native/' + analysis + 'index': index(3, 2),
                'native/' + analysis + 'packs/000': b'native analyses',
                'native/' + analysis + 'packs/001': b'more native analyses',
            }
            host = {
                'native/manifest.json': b'native manifest',
                'native/coverage.json': b'{"complete":false}',
                'native/' + records + 'packs/001': b'derived native records',
                'native/' + analysis + 'packs/002': b'derived native analyses',
            }
            for name, data in (inputs | host).items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            output = root / 'bundle.stage'
            args = ['stage_manifest.py', '--self', 'bundle.stage', '--out', str(output)]
            for name in ('base', 'seed', 'raw', 'native'):
                args += ['--' + name, str(root / name), '--' + name + '-rlocation', name]
            args += ['--pack', str(root / 'pack'), '--pack-rlocation', 'pack']
            with mock.patch.object(sys, 'argv', args):
                stage_manifest.main()
            lines = output.read_text().splitlines()
            self.assertEqual(lines[0], stage_manifest.HEADER + 'bundle.stage')
            files = {parts[0]: parts[1:] for parts in (line.split('\t') for line in lines[1:])}
            self.assertEqual(len(files), len(inputs))
            expected = {
                stage_manifest.RAW + '/native/' + records + 'index': 'native/' + records + 'index',
                stage_manifest.RAW + '/native/' + records + 'packs/000': 'native/' + records + 'packs/000',
                stage_manifest.SEED + '/' + analysis + 'index': 'native/' + analysis + 'index',
                stage_manifest.SEED + '/' + analysis + 'packs/000': 'native/' + analysis + 'packs/000',
                stage_manifest.SEED + '/' + analysis + 'packs/001': 'native/' + analysis + 'packs/001',
                stage_manifest.RAW + '/pack.bin': 'raw/pack.bin',
                stage_manifest.SEED + '/packs/000': 'pack',
            }
            for target, source in expected.items():
                data = inputs[source]
                self.assertEqual(files[target], [hashlib.sha256(data).hexdigest(), str(len(data)), '644', source])
            self.assertFalse([target for target in files if target.endswith(('manifest.json', 'coverage.json'))])
            derived = [
                stage_manifest.RAW + '/native/' + records + 'packs/001',
                stage_manifest.SEED + '/' + analysis + 'packs/002',
            ]
            self.assertFalse([target for target in derived if target in files])
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
                'seed/SEED': b'seed',
                'pack': b'records',
                'raw/pack.bin': b'raw pipeline pack',
                'native/native-analysis/index': index(1, 1),
                'native/native-analysis/packs/000': b'analyses',
            }.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(data)
            seed = ['--pack', str(root / 'pack'), '--pack-rlocation', 'pack']
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
            self.assertEqual(len((root / 'once.stage').read_text().splitlines()), 1 + 6)

    def test_a_packed_directory_without_a_seed_pack_index_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            native = Path(directory)
            for name, data in (
                ('raw-0123456789abcdef/index', index(2, 1, b'NOTASEED')),
                ('native-analysis/index', b''),
            ):
                (native / name).parent.mkdir(parents=True)
                (native / name).write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'seed pack'):
                    stage_manifest.native_target(native, name.replace('index', 'packs/000'))


if __name__ == '__main__':
    unittest.main()
