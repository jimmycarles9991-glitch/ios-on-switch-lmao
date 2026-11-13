# SPDX-License-Identifier: MIT
import argparse
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest


def checksum(data: bytes | bytearray) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


class NativeSeedTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix='switch-native-seed-')
        cls.root = pathlib.Path(cls.temporary.name)
        cls.bucket = cls.root / 'bucket'
        store = cls.root / 'libraries'
        data = store / 'data/example.metallib'
        framework = store / 'root/System/Library/Frameworks/Example.framework/default.metallib'
        for file in (data, framework):
            file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ARGS.metallib, file)
        shutil.copyfile(ARGS.compute_metallib, store / 'data/compute.metallib')
        head = 'R ok=1 label=test samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0'
        v, f = 'fetch_vertex', 'fetch_nodiscard_fragment'
        data_library = 'lib=data:example'
        framework_library = 'lib=/System/Library/Frameworks/Example.framework/default.metallib'
        stages = [
            (v, f),
            (f'{v} {data_library}', f'{f} {data_library}'),
            (v, f'{f} {data_library}'),
            (f'{v} {framework_library}', f'{f} {framework_library}'),
            (f'{v} {data_library}', f'{f} {framework_library}'),
            (v, f'{f} lib=data:absent'),
            (v, 'missing_function'),
        ]
        lines = [f'{head} | v {vs} | f {fs} | ' for vs, fs in stages]
        lines += [lines[0] + f'fl {f} {data_library} as=linked_alias | ']
        dispatch = (
            'D ok=1 tg=32,1,1 edge=0,0,0 threads=0 tgmem=0:128 tex=0:125:2:1:1:02030405 smp= | '
            'form=d opt=0 max=0 tgm=0 | k artifact_probe lib=data:compute | '
        )
        lines += [
            dispatch,
            dispatch.replace('data:compute', 'data:absent'),
            dispatch.replace('edge=0,0,0', 'edge=1,0,0'),
        ]
        analysis = 'C ok=1 form=d opt=0 max=0 tgm=0 depth=1 | k artifact_probe lib=data:compute | '
        lines += [analysis, analysis.replace('max=0', 'max=64'), analysis.replace('data:compute', 'data:absent')]
        corpus = cls.root / 'observed.log'
        corpus.write_text('\n'.join(lines) + '\n')
        derived = cls.root / 'derived.log'
        derived.write_text(lines[0] + '\n')
        subprocess.run(
            [
                ARGS.tool,
                '--libs',
                str(store),
                ARGS.nvk,
                str(cls.root / 'cache'),
                ARGS.metallib,
                '0',
                str(cls.bucket),
                str(corpus),
                '--derived',
                str(derived),
            ],
            check=True,
        )
        cls.manifest = json.loads((cls.bucket / 'manifest.json').read_text())

    @classmethod
    def tearDownClass(cls) -> None:
        if ARGS.evidence:
            for name in ('bucket', 'restamped'):
                shutil.copytree(cls.root / name, pathlib.Path(ARGS.evidence) / name, dirs_exist_ok=True)
        cls.temporary.cleanup()

    def run_restamp(
        self, output: pathlib.Path, *inputs: pathlib.Path, success: bool = True
    ) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [ARGS.tool, '--restamp', '0123456789abcdef', str(output), *map(str, inputs)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_generic_inputs_and_full_identity(self):
        rows = self.manifest['records']
        self.assertEqual(len(rows), 15)
        self.assertEqual([row['status'] for row in rows[:7]], ['made'] * 5 + ['missing-library', 'missing-function'])
        for row in rows[:5]:
            self.assertEqual(len(row['vertexAir']), 64)
            self.assertEqual(len(row['fragmentAir']), 64)
        self.assertNotIn(' | air ', rows[0]['key'])
        self.assertIn(' | air ', rows[1]['key'])
        self.assertIn(' | air ', rows[2]['key'])
        self.assertNotIn(' | air ', rows[3]['key'])
        self.assertIn('Example', rows[3]['key'])
        self.assertIn(' | air ', rows[4]['key'])
        self.assertIn(' | linked f', rows[7]['key'])
        self.assertIn('as=linked_alias', rows[7]['key'])
        self.assertFalse(rows[0]['derived'])
        self.assertTrue(rows[-1]['derived'])
        self.assertEqual(rows[0]['artifact'], rows[-1]['artifact'])

    def test_compute_dispatch_uses_runtime_identity_and_persists_kernel(self):
        made, missing, edge = self.manifest['records'][8:11]
        self.assertEqual([made['status'], missing['status'], edge['status']], ['made', 'missing-library', 'refused'])
        self.assertEqual(made['kind'], 'compute')
        self.assertTrue(made['key'].startswith('kernel | '))
        self.assertIn('tg=32,1,1 | tgmem=0:128', made['key'])
        self.assertIn('tex=0:125:2:1:1:02030405', made['key'])
        self.assertEqual(made['vertexAir'], '00' * 32)
        record = (self.bucket / made['artifact']).read_bytes()
        self.assertEqual(struct.unpack_from('<I', record, 12)[0], 3)

    def test_compute_kind_mismatch_is_rejected(self):
        bucket, manifest = self.changed_bucket('bad-compute-kind')
        manifest['records'][8]['kind'] = 'render'
        (bucket / 'manifest.json').write_text(json.dumps(manifest))
        self.run_restamp(self.root / 'reject-compute-kind', bucket, success=False)

    def test_compute_analysis_preserves_descriptor_and_reflection_identity(self):
        first, changed, missing = self.manifest['records'][11:14]
        self.assertEqual([first['status'], changed['status'], missing['status']], ['made', 'made', 'missing-library'])
        self.assertEqual(first['kind'], 'analysis')
        self.assertEqual([entry['reflection'] for entry in first['analysis']], [False, True])
        identities = [entry['name'] for row in (first, changed) for entry in row['analysis']]
        self.assertEqual(len(set(identities)), 4)
        for row in (first, changed):
            for entry in row['analysis']:
                data = (self.bucket / entry['path']).read_bytes()
                self.assertEqual(data[:8], b'SWNCART\0')
                self.assertEqual(data[24:56].hex(), entry['name'][:64])

    def test_restamp_preserves_programs_and_all_rows(self):
        output = self.root / 'restamped'
        self.run_restamp(output, self.bucket, self.bucket)
        manifest = json.loads((output / 'manifest.json').read_text())
        self.assertEqual(manifest['stack'], '0123456789abcdef')
        self.assertEqual(len(manifest['records']), len(self.manifest['records']) * 2)
        for index, new in enumerate(manifest['records']):
            old = self.manifest['records'][index % len(self.manifest['records'])]
            for field in ('status', 'source', 'line', 'derived', 'input', 'key', 'vertexAir', 'fragmentAir'):
                self.assertEqual(old.get(field), new.get(field), field)
            if old['status'] == 'made':
                if old['kind'] == 'analysis':
                    self.assertEqual(old['analysis'], new['analysis'])
                    for entry in old['analysis']:
                        self.assertEqual(
                            (self.bucket / entry['path']).read_bytes(), (output / entry['path']).read_bytes()
                        )
                        self.assertEqual((output / entry['path']).stat().st_mode & 0o777, 0o644)
                    continue
                before = (self.bucket / old['artifact']).read_bytes()
                after_file = output / new['artifact']
                after = after_file.read_bytes()
                self.assertEqual(before[:16], after[:16])
                self.assertEqual(before[24:-8], after[24:-8])
                self.assertEqual(struct.unpack_from('<Q', after, 16)[0], 0x0123456789ABCDEF)
                self.assertEqual(struct.unpack_from('<Q', after, len(after) - 8)[0], checksum(after[:-8]))
                self.assertEqual(after_file.stat().st_mode & 0o777, 0o644)
                self.assertEqual(after_file.parent.stat().st_mode & 0o777, 0o755)

    def changed_bucket(self, name: str) -> tuple[pathlib.Path, dict]:
        destination = self.root / name
        shutil.copytree(self.bucket, destination)
        return destination, json.loads((destination / 'manifest.json').read_text())

    def test_rejects_checksum_damage(self):
        bucket, manifest = self.changed_bucket('bad-checksum')
        record = bucket / manifest['records'][0]['artifact']
        data = bytearray(record.read_bytes())
        data[-1] ^= 1
        record.write_bytes(data)
        self.run_restamp(self.root / 'reject-checksum', bucket, success=False)

    def test_rejects_manifest_identity_or_path_mismatch(self):
        for field, replacement in (('vertexAir', '00' * 32), ('artifact', '../outside.rec')):
            bucket, manifest = self.changed_bucket(f'bad-{field}')
            manifest['records'][0][field] = replacement
            (bucket / 'manifest.json').write_text(json.dumps(manifest))
            self.run_restamp(self.root / f'reject-{field}', bucket, success=False)

    def test_rejects_unknown_versions(self):
        for field in ('schema', 'packVersion', 'cacheVersion'):
            bucket, manifest = self.changed_bucket(f'bad-{field}')
            manifest[field] += 1
            (bucket / 'manifest.json').write_text(json.dumps(manifest))
            self.run_restamp(self.root / f'reject-{field}', bucket, success=False)

    def test_rejects_damaged_analysis_and_duplicate_reflection_contract(self):
        for change in ('checksum', 'contract', 'path'):
            bucket, manifest = self.changed_bucket('analysis-' + change)
            row = manifest['records'][11]
            if change == 'checksum':
                path = bucket / row['analysis'][0]['path']
                data = bytearray(path.read_bytes())
                data[-1] ^= 1
                path.write_bytes(data)
            elif change == 'contract':
                row['analysis'][1]['reflection'] = False
            else:
                row['analysis'][0]['path'] = '../outside.nca'
            (bucket / 'manifest.json').write_text(json.dumps(manifest))
            self.run_restamp(self.root / ('reject-analysis-' + change), bucket, success=False)

    def test_rejects_different_valid_payload_under_same_name(self):
        bucket, manifest = self.changed_bucket('conflict')
        row = manifest['records'][0]
        path = bucket / row['artifact']
        data = bytearray(path.read_bytes())
        key = row['key'].encode()
        descriptor = key.split(b' | ')[0].rstrip()
        second_key = data.index(descriptor, 92 + len(key))
        program_id = second_key + len(descriptor)
        program_id_bytes = struct.unpack_from('<I', data, program_id)[0]
        image_length = program_id + 4 + program_id_bytes
        self.assertGreater(struct.unpack_from('<I', data, image_length)[0], 0)
        data[image_length + 4] ^= 1
        data[-8:] = struct.pack('<Q', checksum(data[:-8]))
        path.write_bytes(data)
        self.run_restamp(self.root / 'reject-conflict', self.bucket, bucket, success=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tool', required=True)
    parser.add_argument('--nvk', required=True)
    parser.add_argument('--metallib', required=True)
    parser.add_argument('--compute-metallib', required=True)
    parser.add_argument('--evidence')
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
