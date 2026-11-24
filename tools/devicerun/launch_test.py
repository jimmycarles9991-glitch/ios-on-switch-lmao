"""launch.py's summaries over lines in the formats the kernel and the EMC driver log."""

import unittest

from tools.devicerun import devicerun, launch
from tools.devicerun.rules import INFO, NO_DATA

MOVE = 'SwitchEMC: {old} -> {new} kHz, ACTMON asked {new} kHz; display floor 204000 kHz for 312 MB/s'
LAUNCH = (
    'SwitchLaunch: Preferences[659] became the focal app {focal} ms before this line; its first commit {first} ms '
    'before it ({gap} ms after it became focal)'
)
POWER = (
    'SwitchPower: {ms} ms: cpu0 c7 0.0% (0, 0 skipped, 0 frame wakes) wfi 23.6% busy 76.4%; c7 timer exits 0, EMC 6 '
    'moves 204:{low}% 1331:0.8% 1600:{top}%, 1 raises: hint to top 718/718 us (BPMP max 654 us)'
)
PAGEINS = (
    'SwitchSched page-ins (10000 ms): 1200 reads, 96000 KiB asked, 40 ms reading; 1500 waits for pages being read, '
    '{wait} ms (longest 116 ms: SpringBoard[400] on dyld_shared_cache_arm64e); 12 other busy-page waits, 3 ms; '
    'untracked 0; XNU page-ins 3400, 120 evicted and paged in again (ghosts found, 900 added, the last 25600 evictions '
    'held); by process (reads/KiB asked/ms reading/waits/ms waiting): SpringBoard[400] 190/15200/12/210/1044, '
    'Preferences[659] 80/6400/{read}/40/100'
)
PAGEIN_FILES = (
    'SwitchSched page-ins by file (10000 ms; reads/KiB asked/ms reading/waits/ms waiting): dyld_shared_cache_arm64e '
    '900/72000/30/1100/1800, (unnamed) 0/0/0/3/40'
)
CRYPTEX = (
    'SwitchSched cryptex cache (10000 ms): 2900 requests (2000 all held, 100 left to the card); 800 card reads of '
    '100000 KiB, 60000 KiB of it read ahead; 70000 KiB served the first time (40000 KiB of the read-ahead); {evicted} '
    'windows reused holding blocks never served; 0 reads failed'
)


def record(message: str, second: float) -> dict:
    return {
        'timestamp': f'2026-11-23 00:15:{second:09.6f}+0000',
        'bootUUID': 'BOOTA000',
        'processImagePath': '/System/Library/Kernels/kernel.development',
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def rows(*timed: tuple[str, float]) -> dict:
    results, _ = devicerun.run_checks([record(m, t) for m, t in timed], launch.LAUNCHES)
    return {r.check.name: r for r in launch.launch_rows({r.check.name: r for r in results})}


class Launch(unittest.TestCase):
    def test_the_memory_clock_at_the_launch_its_first_commit_and_between(self):
        got = rows(
            (MOVE.format(old=1600000, new=1331200), 30.076),
            (MOVE.format(old=1331200, new=204000), 30.108),
            (MOVE.format(old=204000, new=1600000), 30.781),
            (MOVE.format(old=1600000, new=204000), 31.372),
            (LAUNCH.format(focal=9000, first=100, gap=8900), 35.234),
        )
        self.assertEqual(got['launch.emc'].verdict, INFO)
        # focal at 26.234 (1600 before any move), first commit at 35.134 (204 since 31.372)
        self.assertEqual(
            got['launch.emc'].observed,
            '1 launches: Preferences[659] 8900 ms to its first commit, memory clock 1600 MHz at focal, 204 MHz at the '
            'commit; between: 204 MHz 4435 ms, 1331 MHz 32 ms, 1600 MHz 4433 ms',
        )

    def test_the_memory_clock_residency_per_rate_over_the_windows(self):
        got = rows(
            (POWER.format(ms=10000, low='46.7', top='52.5'), 10), (POWER.format(ms=10000, low='0.0', top='99.2'), 20)
        )
        self.assertEqual(
            got['launch.emc-residency-sum'].observed,
            '2 windows, 20.0 s: 204 MHz 4.7 s, 1331 MHz 0.2 s, 1600 MHz 15.2 s',
        )

    def test_page_ins_by_process_and_file_across_windows(self):
        got = rows(
            (PAGEINS.format(wait=2300, read=300), 10),
            (PAGEIN_FILES, 10),
            (PAGEINS.format(wait=1700, read=900), 20),
            (PAGEIN_FILES, 20),
        )
        self.assertEqual(
            got['launch.pageins-top'].observed,
            '2 windows, 4000 ms waiting for file reads; XNU page-ins 6800, 240 of evicted pages paged in again; by '
            'process: SpringBoard[400] 2112 ms, Preferences[659] 1400 ms; by file: dyld_shared_cache_arm64e 3660 ms, '
            '(unnamed) 80 ms',
        )

    def test_a_line_cut_in_its_process_rows_still_counts_its_whole_rows(self):
        cut = PAGEINS.format(wait=2300, read=300).split(', Preferences[659] 80/64')[0] + ', Preferences[659] 80/64'
        got = rows((cut, 10))
        self.assertEqual(
            got['launch.pageins-top'].observed,
            '1 windows, 2300 ms waiting for file reads; XNU page-ins 3400, 120 of evicted pages paged in again; by '
            'process: SpringBoard[400] 1056 ms; by file: ',
        )

    def test_the_cryptex_cache_counts_add_up(self):
        got = rows((CRYPTEX.format(evicted=3), 10), (CRYPTEX.format(evicted=4), 20))
        self.assertEqual(
            got['launch.cryptex-cache-sum'].observed,
            '2 windows: 5800 requests, 4000 all held, 200 left to the card; 1600 card reads of 200000 KiB (120000 KiB '
            'ahead); 140000 KiB served (80000 KiB of the read-ahead); 7 windows reused unserved; 0 failed',
        )

    def test_a_kernel_without_the_lines_has_no_data(self):
        got = rows(('SwitchSched 1/2 (10000 ms; ...)', 1))
        self.assertEqual({r.verdict for r in got.values()}, {NO_DATA})


if __name__ == '__main__':
    unittest.main()
