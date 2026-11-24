"""commit.py's summaries over lines in the formats kernel/switch/sched/watch.c logs."""

import unittest

from tools.devicerun import commit, devicerun
from tools.devicerun.rules import INFO, NO_DATA

LATE = (
    'SwitchCommit: {name}[393] thread 0x142f 47/47 FX UI: {ms} ms since its idle wait ended (the one before 50 ms '
    'earlier), mostly {cause}: on core 5100 us, blocked 17900 us in 3 waits (page busy 17000 us/2, ulock 900 us/1), '
    'waiting for a core 20 us, 0 preemptions; longest waits: 9000 us page busy, user pc dsc 0x1a2b lr dsc 0x1a00, '
    'kernel 0x0, event 0x0;'
)
WINDOW = (
    'SwitchSched watch (10000 ms; events late past 16 ms for a commit, 8 ms for a touch delivery): '
    'commits {commits}, {late} late (on core 1, waiting for a core 0, blocked {blocked}); touch deliveries 120, '
    '1 late (on core 0, waiting for a core 1, blocked 0); 0 late not logged; threads watched 9 (12 taken, 3 freed '
    'since boot; 0 refused a slot, once a window each)'
)
PATH = (
    "SwitchSched commit path (10000 ms): {n} commits to the frame's wake, p50/p99 us: received 16/64, applied "
    '{a50}/2048, waited 8192/16384, all 8192/{t99} (31 taken on core, 0 skipped; applied at 800 port-set receives, 12 '
    'single-port)'
)
TOUCH = (
    'SwitchTouchSend: backboardd[69] thread 0x18e0 63/63 FX - to pid 393: report to send 9100 us, the stretch began '
    '-200 us after the report; 9.3 ms since its previous one (the one before 9 ms earlier), mostly waiting for a core: '
    'on core 300 us, blocked 0 us in 0 waits (), waiting for a core 9000 us, 4 preemptions; longest waits:'
)


def record(message: str, second: int) -> dict:
    return {
        'timestamp': f'2026-11-23 09:00:{second:02d}.000000+0000',
        'bootUUID': 'BOOTA000',
        'processImagePath': '/System/Library/Kernels/kernel.development',
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def run(*messages: str) -> tuple[dict, dict]:
    results, _ = devicerun.run_checks([record(m, i) for i, m in enumerate(messages)], commit.COMMIT)
    by_check = {r.check.name: r for r in results}
    return by_check, {r.check.name: r for r in commit.commit_rows(by_check)}


class Commit(unittest.TestCase):
    def test_late_commits_are_counted_by_process_and_cause(self):
        checks, rows = run(
            LATE.format(name='SpringBoard', ms='23.0', cause='blocked'),
            LATE.format(name='SpringBoard', ms='19.0', cause='on core'),
            LATE.format(name='Preferences', ms='40.0', cause='blocked'),
        )
        self.assertEqual(checks['commit.late'].verdict, INFO)
        self.assertEqual(
            rows['commit.wait-by-cause'].observed,
            '3 late commits, 27.3 ms mean: SpringBoard 2 (blocked 1, on core 1); Preferences 1 (blocked 1); blocked '
            'in: page busy 51 ms, ulock 2 ms',
        )

    def test_a_wait_kind_with_a_comma_in_its_name_stays_whole(self):
        kinds = commit.blocked_kinds({'kinds': 'mach_msg send, in transit 300 us/2, page busy 17000 us/2'})
        self.assertEqual(kinds, {'mach_msg send, in transit': 300, 'page busy': 17000})

    def test_the_windows_late_share(self):
        _, rows = run(WINDOW.format(commits=800, late=2, blocked=1), WINDOW.format(commits=200, late=1, blocked=1))
        self.assertEqual(
            rows['commit.late-share'].observed,
            '2 windows: 3 of 1000 commits late (on core 2, waiting for a core 0, blocked 2); 2 of 240 touch deliveries '
            'late; 0 late not logged',
        )

    def test_the_path_split_takes_the_median_window(self):
        _, rows = run(
            PATH.format(n=10, a50=128, t99=32768),
            PATH.format(n=0, a50=0, t99=0),
            PATH.format(n=20, a50=256, t99=65536),
            PATH.format(n=30, a50=512, t99=16384),
        )
        self.assertEqual(
            rows['commit.path-split'].observed,
            "3 windows, 60 commits; median of the windows' p50 us: received 16, applied 256, "
            'waited for the frame 8192, all 8192; worst window p99 all 65536 us',
        )

    def test_a_late_touch_delivery_names_its_thread_and_cause(self):
        checks, _ = run(TOUCH)
        self.assertEqual(checks['touch.send-late'].matches[0]['cause'], 'waiting for a core')
        self.assertEqual(checks['touch.send-late'].matches[0]['thread'], '0x18e0')

    def test_a_kernel_without_the_lines_has_no_data(self):
        _, rows = run('SwitchSched 1/2 (10000 ms; ...)')
        self.assertEqual({r.verdict for r in rows.values()}, {NO_DATA})


if __name__ == '__main__':
    unittest.main()
