"""cpu.py's summaries over lines in the formats the kernel probes log."""

import unittest

from tools.devicerun import cpu, devicerun
from tools.devicerun.rules import INFO, NO_DATA

CORES = (
    "SwitchSched cores (10000 ms; ms: a core's threads, its idle thread split; 310 us to take): "
    'cpu0 threads {t0} (irq 300), idle {i0} = asleep {a0} + irq 150 + rest 150, 12000 sleeps; '
    'cpu1 threads 1000 (irq 10), idle 9000 = asleep 8800 + irq 100 + rest 100, 8000 sleeps; '
    'a64x traps {traps}, unhandled 0, text pages 12'
)
TASKS = (
    'SwitchSched tasks (10000 ms; ms user+system, top 8 by both): SpringBoard[393] 1200+300, '
    'backboardd[69] {bb}+200; all 312 processes 3900+1800, exited 2 (410+30, most prngseedd[1234] 80+2), untracked 0'
)
IRQS = (
    'SwitchPower interrupts: 10000 ms: 10900 SPIs from 2 INTIDs (1090/s): 119 (spi 87) 8000 (800/s), '
    '67 (spi 35) 2900 (290/s)'
)


def record(message: str, second: float) -> dict:
    return {
        'timestamp': f'2026-11-23 09:{int(second) // 60:02d}:{second % 60:09.6f}+0000',
        'bootUUID': 'BOOTA000',
        'processImagePath': '/System/Library/Kernels/kernel.development',
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def rows(*records: dict) -> dict:
    results, _ = devicerun.run_checks(list(records), cpu.CPU)
    return {r.check.name: r for r in cpu.cpu_rows({r.check.name: r for r in results})}


def window(second: int, busy0: int, bb: int, traps: int = 0) -> list[dict]:
    t0 = busy0 - 600
    return [
        record(CORES.format(t0=t0, i0=10000 - t0, a0=10000 - busy0, traps=traps), second),
        record(TASKS.format(bb=bb), second + 0.001),
    ]


class Cpu(unittest.TestCase):
    def test_the_floor_is_the_quietest_quarter_split_by_where_its_time_went(self):
        lines = [r for i in range(8) for r in window(10 * i, 5600 + 500 * i, 900)]
        got = rows(*lines)
        self.assertEqual(got['cpu.floor'].verdict, INFO)
        self.assertIn('8 windows, busy cores p25 0.78 p50 0.88', got['cpu.floor'].observed)
        self.assertIn('the floor (3 windows at or below p25): threads 0.65', got['cpu.floor'].observed)
        self.assertIn('processes account for 0.61 cores', got['cpu.floor'].observed)

    def test_the_floor_names_its_processes(self):
        got = rows(*window(0, 5600, 900), *window(10, 5600, 2000), *window(20, 9000, 5000), *window(30, 9500, 5000))
        self.assertEqual(
            got['cpu.floor-tasks'].observed,
            '2 floor windows, top by user+system: backboardd 165 ms/s, SpringBoard 150 ms/s',
        )

    def test_the_busiest_interrupt_sources_by_rate(self):
        got = rows(record(IRQS, 0), record(IRQS.replace('8000 (800/s)', '2000 (200/s)'), 10))
        self.assertEqual(
            got['cpu.interrupt-sources'].observed,
            '2 windows, the busiest INTIDs: INTID 119 (SPI 87) 500/s, INTID 67 (SPI 35) 290/s',
        )

    def test_translation_traps_are_summed(self):
        got = rows(*window(0, 5600, 900, traps=3), *window(10, 5600, 900, traps=1))
        self.assertEqual(got['cpu.a64x'].observed, '2 windows: 4 traps, 0 not emulated, 24 text pages translated')

    def test_a_kernel_without_the_lines_has_no_data(self):
        got = rows(record('SwitchSched 1/2 (10000 ms; ...)', 0))
        self.assertEqual({r.verdict for r in got.values()}, {NO_DATA})


if __name__ == '__main__':
    unittest.main()
