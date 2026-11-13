import argparse
import copy
import hashlib
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles

from tools.gpu.shaders import native_seed


class NativeSeedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stack = '0000000000001234'
        self.name = '0000000000000010'
        artifact = f'raw-{self.stack}/{self.name}.rec'
        (self.root / artifact).parent.mkdir()
        (self.root / artifact).write_bytes(b'validated native record')
        self.tools = {'stack': self.stack, 'nvk_uuid': 'uuid', 'nvk_build_id': 'compiler'}
        self.manifest = {
            'schema': 1,
            'stack': self.stack,
            'packVersion': 14,
            'cacheVersion': 7,
            'records': [{'name': self.name, 'artifact': artifact, 'derived': False, 'status': 'made'}],
        }

    def report(self, expected: int = 1):
        return native_seed.coverage(self.manifest, expected, self.root, '24B119', self.tools)

    def test_derived_guesses_do_not_hide_observed_refusals(self):
        self.manifest['records'].append({'derived': True, 'status': 'refused'})
        report = self.report()
        self.assertTrue(report['complete'])
        self.assertEqual(report['ipsw'], '24B119')
        self.assertEqual(report['counts']['derived'], {'refused': 1})
        self.assertEqual(len(report['artifacts']), 1)
        self.manifest['records'][0] = {'derived': False, 'status': 'missing-library'}
        self.assertFalse(self.report()['complete'])

    def test_missing_rows_and_wrong_stack_fail(self):
        with self.assertRaises(ValueError):
            self.report(2)
        self.manifest['stack'] = '0000000000004321'
        with self.assertRaises(ValueError):
            self.report()

    def test_zero_observed_states_is_not_a_complete_system_seed(self):
        self.manifest['records'] = []
        self.assertFalse(self.report(0)['complete'])

    def test_partial_checkpoint_preserves_incomplete_coverage(self):
        self.manifest['records'].append({'derived': False, 'status': 'refused'})
        report = self.report(2)
        (self.root / 'coverage.json').write_text(json.dumps(report))
        output = self.root / 'checked.json'
        args = argparse.Namespace(seed=self.root, out=output, strict=True)
        with self.assertRaisesRegex(ValueError, 'seed incomplete'):
            native_seed.check(args)
        self.assertFalse(output.exists())
        args.strict = False
        native_seed.check(args)
        checked = json.loads(output.read_text())
        self.assertFalse(checked['complete'])
        self.assertEqual(checked, {key: value for key, value in report.items() if key != 'artifacts'})

    def test_only_matching_real_records_are_certified(self):
        original = copy.deepcopy(self.manifest['records'][0])
        for artifact in ('../outside.rec', 'raw-other/key.rec', f'raw-{self.stack}/missing.rec'):
            self.manifest['records'][0]['artifact'] = artifact
            with self.assertRaises(ValueError):
                self.report()
        self.manifest['records'][0] = original
        (self.root / original['artifact']).unlink()
        with self.assertRaises(ValueError):
            self.report()

    def test_observed_and_derived_partition_is_preserved(self):
        source = 'R ok=1 original\nD ok=1 shape\n# derived\nR ok=1 guessed\nC ok=1 kernel\n'
        self.assertEqual(
            native_seed.split_lines(source), (['R ok=1 original', 'D ok=1 shape'], ['R ok=1 guessed', 'C ok=1 kernel'])
        )
        self.assertEqual(native_seed.split_lines('R ok=1 observed\n'), (['R ok=1 observed'], []))

    def test_unseeded_dispatch_cannot_hide_behind_render_coverage(self):
        self.manifest['records'].append({'kind': 'compute', 'derived': False, 'status': 'refused'})
        report = self.report(2)
        self.assertFalse(report['complete'])
        self.assertEqual(report['kinds'], {'render': {'made': 1}, 'compute': {'refused': 1}, 'analysis': {}})

    def analysis_row(self):
        artifacts = []
        for reflected in (False, True):
            key = hashlib.sha256(str(reflected).encode()).digest()
            payload = b'validated native analysis payload'
            name = key.hex() + '.nca'
            relative = 'native-analysis/' + name
            path = self.root / relative
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(
                b'SWNCART\0'
                + struct.pack('<IIQ', 1, 88, len(payload))
                + key
                + hashlib.sha256(payload).digest()
                + payload
            )
            artifacts.append({'name': name, 'path': relative, 'reflection': reflected})
        return {'kind': 'analysis', 'derived': False, 'status': 'made', 'analysis': artifacts}

    def test_analysis_contracts_and_files_are_required_for_coverage(self):
        row = self.analysis_row()
        self.manifest['records'].append(row)
        self.assertTrue(self.report(2)['complete'])
        self.assertEqual(len(self.report(2)['artifacts']), 3)
        for change in ('missing-contract', 'duplicate-contract', 'wrong-path', 'corrupt-file'):
            with self.subTest(change=change):
                changed = copy.deepcopy(row)
                self.manifest['records'][-1] = changed
                if change == 'missing-contract':
                    changed['analysis'].pop()
                elif change == 'duplicate-contract':
                    changed['analysis'][1]['reflection'] = False
                elif change == 'wrong-path':
                    changed['analysis'][0]['path'] = '../outside.nca'
                else:
                    path = self.root / changed['analysis'][0]['path']
                    data = bytearray(path.read_bytes())
                    data[-1] ^= 1
                    path.write_bytes(data)
                with self.assertRaises(ValueError):
                    self.report(2)

    def test_unseeded_analysis_cannot_hide_behind_dispatch_coverage(self):
        self.manifest['records'].append({'kind': 'analysis', 'derived': False, 'status': 'missing-library'})
        report = self.report(2)
        self.assertFalse(report['complete'])
        self.assertEqual(report['kinds']['analysis'], {'missing-library': 1})

    def test_restamp_materializes_sandbox_analysis_links_and_preserves_validation(self):
        row = self.analysis_row()
        self.manifest['records'].append(row)
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))
        sandbox = self.root / 'sandbox'
        sandbox.mkdir()
        for name in ('manifest.json', f'raw-{self.stack}'):
            (sandbox / name).symlink_to(self.root / name)
        analysis = sandbox / 'native-analysis'
        analysis.mkdir()
        for entry in row['analysis']:
            (analysis / entry['name']).symlink_to(self.root / entry['path'])
        with self.assertRaises(ValueError):
            native_seed.analysis_artifacts(row, sandbox)
        view = native_seed.restamp_view(sandbox, self.root / 'view')
        self.assertEqual(native_seed.analysis_artifacts(row, view), native_seed.analysis_artifacts(row, self.root))
        for entry in row['analysis']:
            path = view / entry['path']
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            os.close(fd)
            self.assertFalse(path.is_symlink())
            data = bytearray(path.read_bytes())
            data[-1] ^= 1
            path.unlink()
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                native_seed.analysis_artifacts(row, view)
            path.write_bytes((self.root / entry['path']).read_bytes())

    def region_rows(self):
        line = 'D ok=1 tg=8,5,3 edge=3,1,1 threads=1 | form=f | k test lib=data:hash |'
        prototype = self.manifest['records'][0]
        return [
            dict(
                prototype,
                kind='compute',
                source='capture',
                line=4,
                input=line,
                region=mask,
                local=shape,
                regionMode=mode,
                key=f'kernel | hash | tg={shape[0]},{shape[1]},{shape[2]} | tgmem= | region=1',
            )
            for mask, (shape, mode) in native_seed.region_signatures(line).items()
        ]

    def test_all_regions_count_as_one_original_operation(self):
        self.manifest['records'] = self.region_rows()
        report = self.report()
        self.assertTrue(report['complete'])
        self.assertEqual(report['counts']['observed'], {'made': 1})
        self.assertEqual(report['variants']['observed'], {'made': 8})
        self.manifest['records'][4]['status'] = 'refused'
        self.manifest['records'][4].pop('artifact')
        report = self.report()
        self.assertFalse(report['complete'])
        self.assertEqual(report['kinds']['compute'], {'refused': 1})
        self.assertEqual(report['variants']['observed'], {'made': 7, 'refused': 1})

    def test_missing_duplicate_and_mislabeled_regions_fail_coverage(self):
        original = self.region_rows()
        for change in ('missing', 'duplicate', 'shape', 'mode', 'mask', 'key', 'input'):
            with self.subTest(change=change):
                rows = copy.deepcopy(original)
                if change == 'missing':
                    rows.pop()
                elif change == 'duplicate':
                    rows.append(copy.deepcopy(rows[0]))
                elif change == 'shape':
                    rows[1]['local'][0] = 4
                elif change == 'mode':
                    rows[1]['regionMode'] = False
                elif change == 'mask':
                    rows[1]['region'] = 15
                elif change == 'key':
                    rows[1]['key'] = rows[0]['key']
                else:
                    rows[1]['input'] += ' different'
                self.manifest['records'] = rows
                with self.assertRaises(ValueError):
                    self.report()

    def test_whole_dispatch_refusal_is_accounted_but_never_complete(self):
        row = self.region_rows()[0]
        for key in ('region', 'regionMode', 'local', 'artifact', 'key'):
            row.pop(key)
        row['status'] = 'refused'
        self.manifest['records'] = [row]
        self.assertFalse(self.report()['complete'])

    def test_repeated_original_inputs_keep_separate_provenance(self):
        rows = self.region_rows()
        repeat = copy.deepcopy(rows)
        for row in repeat:
            row['line'] = 5
        self.manifest['records'] = rows + repeat
        self.assertTrue(self.report(2)['complete'])
        self.assertEqual(self.report(2)['variants']['observed'], {'made': 16})

    def test_equal_count_cannot_substitute_another_original_input(self):
        rows = self.region_rows()
        for row in rows:
            row['line'] = 1
        expected = [('capture', [rows[0]['input']], [])]
        native_seed.validate_inputs(rows, expected)
        for row in rows:
            row['source'] = 'other'
        with self.assertRaises(ValueError):
            native_seed.validate_inputs(rows, expected)

    def test_pack_lists_keep_each_file_once_in_its_most_observed_tier(self):
        record = self.manifest['records'][0]
        guessed = dict(record, derived=True)
        derived_only = dict(
            record, name='0000000000000020', artifact=f'raw-{self.stack}/0000000000000020.rec', derived=True
        )
        analysis = dict(self.analysis_row(), derived=True)
        self.manifest['records'] = [guessed, record, derived_only, {'derived': False, 'status': 'refused'}, analysis]
        records, analyses = native_seed.pack_lists(self.manifest)
        self.assertEqual(
            records, {self.name: (0, record['artifact']), '0000000000000020': (1, derived_only['artifact'])}
        )
        self.assertEqual(analyses, {entry['name'][:64]: (1, entry['path']) for entry in analysis['analysis']})
        self.manifest['records'].append(dict(record, artifact=f'raw-{self.stack}/elsewhere.rec'))
        with self.assertRaises(ValueError):
            native_seed.pack_lists(self.manifest)

    def test_a_records_program_slots_are_packed_and_certified_with_it(self):
        record = self.manifest['records'][0]
        slot = {'name': '00000000000000a0', 'path': f'raw-{self.stack}/00000000000000a0.slot'}
        (self.root / slot['path']).write_bytes(bytes(native_seed.SLOT_PAGE_BYTES * 2))
        record['slots'] = [slot]
        self.assertIn(slot['path'], self.report()['artifacts'])
        records, _ = native_seed.pack_lists(self.manifest)
        self.assertEqual(records[slot['name']], (0, slot['path']))
        (self.root / slot['path']).write_bytes(bytes(100))
        with self.assertRaises(ValueError):
            self.report()
        record['slots'] = [dict(slot, path=f'raw-{self.stack}/00000000000000a1.slot')]
        with self.assertRaises(ValueError):
            self.report()

    def test_pack_writes_the_device_layout_the_same_every_time(self):
        self.manifest['records'].append(self.analysis_row())
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))
        build = bytes(range(32))
        m2v_index = self.root / 'm2v-index'
        m2v_index.write_bytes(b'M2VSEED2' + bytes(16) + build + bytes(8))
        tool = Path(runfiles.Create().Rlocation('_main/kits/SwitchSeedPack/seedpack'))
        outputs = []
        for run in ('first', 'second'):
            work = self.root / ('work-' + run)
            for folder in ('home', 'tmp'):
                (work / folder).mkdir(parents=True)
            out = self.root / ('packed-' + run)
            native_seed.pack(argparse.Namespace(seed=self.root, m2v_index=m2v_index, tool=tool, out=out), work)
            outputs.append(
                {p.relative_to(out).as_posix(): p.read_bytes() for p in sorted(out.rglob('*')) if p.is_file()}
            )
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(
            sorted(outputs[0]),
            [f'native-analysis/{name}' for name in ('index', 'packs/000')]
            + [f'raw-{self.stack}/{name}' for name in ('index', 'packs/000')],
        )
        records = outputs[0][f'raw-{self.stack}/index']
        self.assertEqual(records[:8], b'SWSEEDPK')
        self.assertEqual(struct.unpack('<6I', records[8:32]), (1, 8, 1, 1, 1, 0))
        self.assertEqual(records[32:64], int(self.stack, 16).to_bytes(32, 'little'))
        self.assertEqual(outputs[0][f'raw-{self.stack}/packs/000'][:23], b'validated native record')
        self.assertEqual(outputs[0]['native-analysis/index'][32:64], build)


if __name__ == '__main__':
    unittest.main()
