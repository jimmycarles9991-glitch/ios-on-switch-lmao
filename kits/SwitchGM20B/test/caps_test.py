"""kits/SwitchGM20B/caps on the host.

The generator's output does not change from run to run or from the build's, every row carries a grade and a citation,
and a row that breaks a rule fails the generator.
"""

import argparse
import os
import shutil
import tempfile
import unittest

import caps
import gen_caps


def generate(args: argparse.Namespace) -> tuple[str, str]:
    table, rows = gen_caps.build(args)
    return gen_caps.header_text(table, rows), gen_caps.json_text(table, rows)


class CapsTest(unittest.TestCase):
    def setUp(self):
        self.args = gen_caps.runfiles_args()

    def test_output_is_stable(self):
        first = generate(self.args)
        self.assertEqual(first, generate(self.args))
        with open('kits/SwitchGM20B/SwitchGM20BCaps.h', encoding='utf-8') as f:
            self.assertEqual(first[0], f.read())
        with open('kits/SwitchGM20B/gm20b_caps.json', encoding='utf-8') as f:
            self.assertEqual(first[1], f.read())

    def test_every_row_has_a_grade_and_a_citation(self):
        table, _ = gen_caps.build(self.args)
        for row in table.formats + table.limits + table.features:
            self.assertIn(row.cols['grade'], caps.GRADES, row.where())
            self.assertTrue(row.cols['citation'].strip(), row.where())
            self.assertTrue(row.cols['hwtest'].strip(), row.where())

    def broken(self, table: str, old: str, new: str):
        scratch = tempfile.mkdtemp(dir=os.environ.get('TEST_TMPDIR'))
        for name in ('formats', 'limits', 'features', 'methods'):
            shutil.copy(os.path.join(self.args.tables, name + '.tsv'), scratch)
        path = os.path.join(scratch, table + '.tsv')
        with open(path, encoding='utf-8') as f:
            text = f.read()
        self.assertIn(old, text)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text.replace(old, new, 1))
        args = gen_caps.runfiles_args()
        args.tables = scratch
        with self.assertRaises(caps.CapsError):
            gen_caps.build(args)

    def test_a_row_without_a_grade_fails(self):
        self.broken('limits', '\tchip\t', '\t\t')

    def test_a_row_without_a_citation_fails(self):
        self.broken('limits', '\tthird_party/mesa/src/nouveau/headers/nvidia/classes/cl906f.h:174\t', '\t\t')

    def test_a_method_the_headers_lack_fails(self):
        self.broken('features', 'b197 SET_PIXEL_SHADER_INTERLOCK_CONTROL,', 'b197 SET_PIXEL_SHADER_INTERLOCKS,')

    def test_an_unknown_hw_test_fails(self):
        self.broken('limits', '\tmetal.render.color\t', '\tmetal.render.colour\t')

    def test_a_format_nil_lacks_fails(self):
        self.broken('formats', '\tB8G8R8A8_UNORM\t', '\tB8G8R8A8_UNORMAL\t')


if __name__ == '__main__':
    unittest.main()
