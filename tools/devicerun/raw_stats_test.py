"""raw_stats.py's per-boot sum of the raw path's rates lines."""

import unittest

from tools.devicerun import devicerun, raw_stats
from tools.devicerun.rules import INFO, NO_DATA

SUBSYSTEM = 'com.brunostjohn.switch.TegraGM20BMetal'


def record(message: str, boot: str = 'BOOTA000', second: int = 0) -> dict:
    return {
        'timestamp': f'2026-11-07 12:00:{second:02d}.000000+0000',
        'bootUUID': boot,
        'processImagePath': '/usr/libexec/backboardd',
        'subsystem': SUBSYSTEM,
        'category': 'default',
        'messageType': 'Default',
        'eventMessage': message,
    }


def rows(*records: dict) -> list:
    results, _ = devicerun.run_checks(list(records), [raw_stats.CHECK])
    return raw_stats.rows(results[0])


class RawStats(unittest.TestCase):
    def test_reasons_and_predicates_add_up_per_boot(self):
        tail = ' sw=0 cpu=0 acq=3 enc=40us/c d=12.0 p=1.0 b=0.00 defer=0 abort=0 rw=0 pool=8MB/2 grow=0 imp=0/0MB'
        results = rows(
            record(
                'TegraGM20BMetal: backboardd raw 5s c=300(60.0/s) m2v=4(state:3,texture:1)'
                + tail
                + ' why=xr_target:5/3,fetch_ipc:2/1',
            ),
            record(
                'TegraGM20BMetal: SpringBoard raw 5s c=100(20.0/s) m2v=2(state:2)' + tail + ' why=xr_target:4/2',
                second=5,
            ),
            record('TegraGM20BMetal: backboardd raw 5s c=290(58.0/s) m2v=0' + tail + ' why=-', boot='BOOTB000'),
        )
        self.assertEqual([r.verdict for r in results], [INFO, INFO])
        first = results[0].observed
        self.assertIn('2 lines: raw 400, m2v 6 (1.5%)', first)
        self.assertIn('reasons state 5, texture 1', first)
        self.assertIn('predicates hit/alone xr_target 9/5, fetch_ipc 2/1', first)
        self.assertIn('m2v by process backboardd 4, SpringBoard 2', first)
        self.assertEqual(results[0].check.name, 'raw.falls-off.BOOTA000')
        self.assertIn('raw 290, m2v 0', results[1].observed)
        self.assertIn('reasons -', results[1].observed)

    def test_other_raw_lines_are_not_rates(self):
        results = rows(record('raw: texture on m2v: 2D RGBA16F'), record('raw: first xr_target: a=1 (0x1) b=2 (0x2)'))
        self.assertEqual(results[0].verdict, NO_DATA)

    def test_the_summary_adds_one_predicate_clause_at_most(self):
        clause = devicerun.clause(raw_stats.CHECK)
        self.assertIn('eventMessage CONTAINS "raw "', clause)
        self.assertIn(SUBSYSTEM, clause)


if __name__ == '__main__':
    unittest.main()
