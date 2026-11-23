import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'pack'))
import selector_section


class PackSelectors(unittest.TestCase):
    def test_prepared_program_ids_have_the_cpp_layout(self):
        pipes = [{} for _ in range(10)]
        pipes[5]['selectors'] = {'assume': 32, 'configs': [(1, 100), (2, 101)]}
        pipes[9]['selectors'] = {'assume': 0, 'configs': [(1, 200)]}
        expected = struct.pack('<16I', 2, 0, 5, 24, 9, 48, 2, 32, 1, 100, 2, 101, 1, 0, 1, 200)
        self.assertEqual(selector_section.section_bytes(pipes), expected)
        self.assertEqual(selector_section.section_bytes([{}]), b'')

    def test_programs_join_the_shared_program_heap(self):
        program = {'id': 'folded'}
        record = {'selectors': {'assume': 2, 'configs': [(7, program)]}}
        self.assertEqual(selector_section.programs(record), [program])
        self.assertEqual(selector_section.of_record(record, lambda _: 19), {'assume': 2, 'configs': [(7, 19)]})

    def test_every_observation_has_a_specialization_or_an_identical_generic_proof(self):
        table = 'layout slot=0 length=1 padding=\n0000000000000001 02\n0000000000000001 01\n0000000000000002 01\n'
        records = [
            {'key': 1, 'selectors': {'configs': [(2, 10)]}, 'selector_generic': [1]},
            {'key': 2, 'selector_generic': [1]},
        ]
        report = selector_section.coverage(records, table, lambda r: r['key'])
        self.assertIn('3 observations, 2 normalized IDs, 3 form/config proofs', report[0])
        self.assertTrue(any('byte-identical generic (code, data, bindings and ordering)' in line for line in report))
        with self.assertRaisesRegex(ValueError, 'absent from pack'):
            selector_section.coverage(records[:1], table, lambda r: r['key'])
        records[0]['selector_generic'] = []
        with self.assertRaisesRegex(ValueError, 'expected IDs'):
            selector_section.coverage(records, table, lambda r: r['key'])
        records[0] = {'key': 1, 'outcome': 2, 'why': 'changed fragment bindings or ordering'}
        with self.assertRaisesRegex(ValueError, 'changed fragment bindings or ordering'):
            selector_section.coverage(records, table, lambda r: r['key'])

    def test_an_empty_build_table_has_no_obligations(self):
        report = selector_section.coverage([], 'layout slot=0 length=52 padding=3\n', lambda r: r['key'])
        self.assertEqual(report, ['selector coverage: 0 observations, 0 normalized IDs, 0 form/config proofs'])


if __name__ == '__main__':
    unittest.main()
