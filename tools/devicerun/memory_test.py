"""memory.py's rows over lines in the formats the Metal driver and XNU's memorystatus log."""

import unittest

from tools.devicerun import devicerun, memory
from tools.devicerun.rules import FAIL, NO_DATA, PASS

HELD = (
    'SwitchMetal: memory held {held} KB: chunks {chunks} KB in 2 (pinned {pinned} KB in 1, kept for reuse 0 KB), '
    'write-combined 256 KB, heaps {heaps} KB, kept imports 240 KB, tables 1200 KB; GPU-only heaps 8192 KB, volatile '
    'heaps 0 KB; footprint {footprint} KB {limit}, IOKit 2400 KB'
)
# xnu-12377's line (kern_memorystatus.c memorystatus_kill_specific_process)
KILL = (
    '1437.812 memorystatus: killing_specific_process pid {pid} [{name}] ({cause} 0 12s rf:- type:daemon) {kb}KB - '
    'memorystatus_available_pages: 40211'
)
# Boot 2's first kill, as its log archive holds it (22 Nov 23:58:16, process /kernel.development)
BOOT2_KILL = (
    '431.030 memorystatus: killing_specific_process pid 353 [iconservicesagent] (per-process-limit 0 141s rf:- '
    'type:daemon) 22071KB - memorystatus_available_pages: 92047'
)


def record(message: str, second: float, process: str, subsystem: str = '') -> dict:
    return {
        'timestamp': f'2026-11-23 00:{int(second) // 60:02d}:{second % 60:09.6f}+0000',
        'bootUUID': 'BOOT2000',
        'processImagePath': f'/usr/libexec/{process}' if process != 'kernel' else '/kernel.development',
        'subsystem': subsystem,
        'category': 'gm20b' if subsystem else '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def held(second: float, process: str, held_kb: int, limit_kb: int | None = 15360, pinned: int = 1024) -> dict:
    limit = f'of limit {limit_kb} KB' if limit_kb else 'of no limit'
    message = HELD.format(
        held=held_kb, chunks=held_kb - 1696, pinned=pinned, heaps=0, footprint=held_kb + 6000, limit=limit
    )
    return record(message, second, process, 'com.brunostjohn.switch.TegraGM20BMetal')


def kill(second: float, name: str, kb: int, cause: str = 'per-process-limit') -> dict:
    return record(KILL.format(pid=312, name=name, cause=cause, kb=kb), second, 'kernel')


def rows(*records: dict) -> dict:
    results, _ = devicerun.run_checks(list(records), memory.MEMORY)
    return {r.check.name: r for r in memory.memory_rows({r.check.name: r for r in results})}


class Memory(unittest.TestCase):
    def test_a_kill_past_its_own_limit_fails_with_the_driver_line_before_it(self):
        got = rows(
            held(10, 'iconservicesagent', 3600),
            held(20, 'iconservicesagent', 14500, pinned=8192),
            kill(22, 'iconservicesagent', 22528),
        )
        row = got['memory.limit-kills']
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('iconservicesagent[312] 22528 KB: held 14500 KB (chunks 12804 pinned 8192', row.observed)
        self.assertIn('at 00:00:20', row.observed)

    def test_boot_2s_kill_line_is_read(self):
        got = rows(record(BOOT2_KILL, 4, 'kernel'))
        self.assertEqual(got['memory.limit-kills'].verdict, FAIL)
        self.assertIn('iconservicesagent[353] 22071 KB: no driver line', got['memory.limit-kills'].observed)

    def test_a_daemon_is_judged_by_its_lowest_limit_and_its_most_held(self):
        got = rows(held(1, 'iconservicesagent', 7000, limit_kb=40960), held(30, 'iconservicesagent', 3000))
        row = got['memory.small-daemon-held']
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('iconservicesagent held 7000 KB', row.observed)
        self.assertIn('(lowest limit 15360 KB)', row.observed)

    def test_a_kill_of_a_process_without_the_driver_says_so(self):
        got = rows(kill(5, 'mediaanalysisd', 30000))
        self.assertEqual(got['memory.limit-kills'].verdict, FAIL)
        self.assertIn('mediaanalysisd[312] 30000 KB: no driver line', got['memory.limit-kills'].observed)

    def test_other_kills_are_counted_and_pass(self):
        got = rows(held(1, 'Maps', 9000, None), kill(2, 'Maps', 90000, cause='fc-thrashing'))
        self.assertEqual(got['memory.limit-kills'].verdict, PASS)
        self.assertIn('0 per-process-limit kills; 1 other kills', got['memory.limit-kills'].observed)

    def test_a_small_daemon_over_the_ceiling_fails_named_with_its_split(self):
        got = rows(held(1, 'iconservicesagent', 6200), held(2, 'widgetd', 2000), held(3, 'backboardd', 90000, None))
        row = got['memory.small-daemon-held']
        self.assertEqual(row.verdict, FAIL)
        self.assertIn('2 small processes, 1 over 5120 KB: iconservicesagent held 6200 KB', row.observed)
        self.assertNotIn('backboardd', row.observed)

    def test_boot_2s_floor_passes_the_ceiling(self):
        got = rows(held(1, 'iconservicesagent', 3600))
        self.assertEqual(got['memory.small-daemon-held'].verdict, PASS)

    def test_no_small_daemon_line_is_no_data(self):
        got = rows(held(1, 'backboardd', 90000, None))
        self.assertEqual(got['memory.small-daemon-held'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
