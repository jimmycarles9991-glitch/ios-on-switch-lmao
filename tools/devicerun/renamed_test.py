#!/usr/bin/env python3
"""Pulls and HW Test runs from before the driver's rename read in today's names: one example of each family."""

import unittest

from tools.devicerun import renamed


class Current(unittest.TestCase):
    def test_each_family_reads_in_its_new_name(self) -> None:
        for before, after in (
            (
                'HWTEST gpu.raw.plain_rows PASS dev=raw raw_cbs=12 raw_paths=raw/12',
                'HWTEST gpu.gm20b.plain_rows PASS dev=gm20b gm20b_cbs=12 gm20b_paths=gm20b/12',
            ),
            ('raw: harvest a 1/2 00', 'SwitchMetal: harvest a 1/2 00'),
            ('refused raw:tex.shape 4x4', 'refused gm20b:tex.shape 4x4'),
            ('backboardd raw 5s c=300(60.0/s)', 'backboardd gm20b 5s c=300(60.0/s)'),
            (
                '-[RMTexture foo]: unrecognized selector sent to',
                '-[SwitchMetalTexture foo]: unrecognized selector sent to',
            ),
            ('stutter.raw-ring-waits', 'stutter.gm20b-ring-waits'),
            ('crash.raw-selector', 'crash.gm20b-selector'),
        ):
            with self.subTest(before=before):
                self.assertEqual(renamed.current(before), after)

    def test_a_line_already_in_new_names_is_kept(self) -> None:
        line = 'SwitchMetal: gm20b:tex.shape on gpu.gm20b.plain_rows'
        self.assertEqual(renamed.current(line), line)

    def test_a_report_reads_in_new_names(self) -> None:
        old = {'results': [{'id': 'gpu.raw.plain_rows', 'metrics': {'dev': 'raw', 'raw_cbs': 3}}]}
        new = {'results': [{'id': 'gpu.gm20b.plain_rows', 'metrics': {'dev': 'gm20b', 'gm20b_cbs': 3}}]}
        self.assertEqual(renamed.current_report(old), new)

    def test_a_predicate_literal_also_finds_its_old_spelling(self) -> None:
        self.assertEqual(
            renamed.spellings('SwitchMetal: a state the pack lacks'),
            ['SwitchMetal: a state the pack lacks', 'raw: a state the pack lacks'],
        )
        self.assertEqual(renamed.spellings('unchanged words'), ['unchanged words'])


if __name__ == '__main__':
    unittest.main()
