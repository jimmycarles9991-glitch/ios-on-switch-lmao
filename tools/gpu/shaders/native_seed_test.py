import argparse
import copy
import json
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
        artifact = f'rec-{self.stack}/{self.name}.rec'
        (self.root / artifact).parent.mkdir()
        (self.root / artifact).write_bytes(b'validated native record')
        self.tools = {'stack': self.stack}
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
        for artifact in ('../outside.rec', 'raw-other/key.rec', f'rec-{self.stack}/missing.rec'):
            self.manifest['records'][0]['artifact'] = artifact
            with self.assertRaises(ValueError):
                self.report()
        self.manifest['records'][0] = original
        (self.root / original['artifact']).unlink()
        with self.assertRaises(ValueError):
            self.report()

    def test_observed_and_derived_partition_is_preserved(self):
        source = 'R ok=1 original\nD ok=1 shape\n# derived\nR ok=1 guessed\nD ok=1 guessed shape\n'
        self.assertEqual(
            native_seed.split_lines(source),
            (['R ok=1 original', 'D ok=1 shape'], ['R ok=1 guessed', 'D ok=1 guessed shape']),
        )
        self.assertEqual(native_seed.split_lines('R ok=1 observed\n'), (['R ok=1 observed'], []))

    def test_a_compute_state_line_makes_no_native_record(self):
        source = 'C ok=1 form=d opt=0 | k kernel lib=data:compute |\n# derived\nC ok=1 form=f | k kernel |\n'
        self.assertEqual(native_seed.split_lines(source), ([], []))

    def test_unseeded_dispatch_cannot_hide_behind_render_coverage(self):
        self.manifest['records'].append({'kind': 'compute', 'derived': False, 'status': 'refused'})
        report = self.report(2)
        self.assertFalse(report['complete'])
        self.assertEqual(report['kinds'], {'render': {'made': 1}, 'compute': {'refused': 1}})

    def test_an_analysis_row_is_no_native_record(self):
        self.manifest['records'].append({'kind': 'analysis', 'derived': False, 'status': 'made', 'analysis': []})
        with self.assertRaisesRegex(ValueError, 'unknown native record kind: analysis'):
            self.report(2)

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
            record, name='0000000000000020', artifact=f'rec-{self.stack}/0000000000000020.rec', derived=True
        )
        self.manifest['records'] = [guessed, record, derived_only, {'derived': False, 'status': 'refused'}]
        records = native_seed.pack_lists(self.manifest)
        self.assertEqual(
            records, {self.name: (0, record['artifact']), '0000000000000020': (1, derived_only['artifact'])}
        )
        self.manifest['records'].append(dict(record, artifact=f'rec-{self.stack}/elsewhere.rec'))
        with self.assertRaises(ValueError):
            native_seed.pack_lists(self.manifest)

    def test_a_records_program_slots_are_packed_and_certified_with_it(self):
        record = self.manifest['records'][0]
        slot = {'name': '00000000000000a0', 'path': f'rec-{self.stack}/00000000000000a0.slot'}
        (self.root / slot['path']).write_bytes(bytes(native_seed.SLOT_PAGE_BYTES * 2))
        record['slots'] = [slot]
        self.assertIn(slot['path'], self.report()['artifacts'])
        records = native_seed.pack_lists(self.manifest)
        self.assertEqual(records[slot['name']], (0, slot['path']))
        (self.root / slot['path']).write_bytes(bytes(100))
        with self.assertRaises(ValueError):
            self.report()
        record['slots'] = [dict(slot, path=f'rec-{self.stack}/00000000000000a1.slot')]
        with self.assertRaises(ValueError):
            self.report()

    def test_pack_writes_the_device_layout_the_same_every_time(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))
        tool = Path(runfiles.Create().Rlocation('_main/kits/SwitchSeedPack/seedpack'))
        outputs = []
        for run in ('first', 'second'):
            work = self.root / ('work-' + run)
            for folder in ('home', 'tmp'):
                (work / folder).mkdir(parents=True)
            out = self.root / ('packed-' + run)
            native_seed.pack(argparse.Namespace(seed=self.root, tool=tool, out=out), work)
            outputs.append(
                {p.relative_to(out).as_posix(): p.read_bytes() for p in sorted(out.rglob('*')) if p.is_file()}
            )
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(sorted(outputs[0]), [f'rec-{self.stack}/{name}' for name in ('index', 'packs/000')])
        records = outputs[0][f'rec-{self.stack}/index']
        self.assertEqual(records[:8], b'SWSEEDPK')
        self.assertEqual(struct.unpack('<6I', records[8:32]), (1, 8, 1, 1, 1, 0))
        self.assertEqual(records[32:64], int(self.stack, 16).to_bytes(32, 'little'))
        self.assertEqual(outputs[0][f'rec-{self.stack}/packs/000'][:23], b'validated native record')


if __name__ == '__main__':
    unittest.main()
