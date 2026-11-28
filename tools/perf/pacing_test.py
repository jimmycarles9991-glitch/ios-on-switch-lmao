"""Tests for pacing.py's periodic hitch detection and periodic kernel-line correlation."""

import unittest
from datetime import UTC, datetime, timedelta

from tools.perf import pacing
from tools.perf.source import TICK_HZ, Rec

T0 = datetime(2026, 11, 26, 19, 48, 0, tzinfo=UTC)
TICKS_PER_MS = int(TICK_HZ) // 1000
PERIOD_TICKS = round(TICK_HZ / 60)
MACH0 = 100_000 * TICKS_PER_MS  # aligned to 100 s so modulo phase tests have predictable phase offsets
LATE_FRAME = (
    'SwitchFrame: latch {latch} after 2 frames (33333 us), cause ca; origin vsync {origin}; first submit 20500, '
    'last submit 21200 (2, exec 30 us), swap 21400, offer 21450, gpu done 24000, flip 24010, latch 33333 us; '
    'other submits 0; swap thread base 54 sched 54 mode 2 qos 0; cpu 1785 MHz at vsync 1785 at swap; imp 0 0 us, '
    'bind 0 0 us, alloc 0 0 us, free 0, clean 0 KiB 0 us; gpu busy 12.0%'
)


def make_rec(msg: str, offset_s: float) -> Rec:
    return Rec(
        T0 + timedelta(seconds=offset_s),
        0,
        proc='kernel',
        msg=msg,
        boot='B',
        pid=0,
        tid=1,
        subsystem='',
        category='',
        name='',
        kind='',
        ident=0,
    )


def slow_at(offsets_s: list[float]) -> list[pacing.Slow]:
    recs = []
    for s in offsets_s:
        latch = MACH0 + round(s * TICK_HZ)
        recs.append(make_rec(LATE_FRAME.format(latch=latch, origin=latch - 2 * PERIOD_TICKS), s))
    return pacing.slow_presents(recs)


class PeriodicCycleTest(unittest.TestCase):
    def test_subsecond_cycle_keeps_existing_format(self):
        slows = slow_at([i * 0.5 + 0.05 for i in range(30)])
        got = pacing.periodic(slows)
        self.assertIn('30 of 30 late presents keep to a 500 ms cycle', got)
        self.assertIn('30 cycles', got)

    def test_multisecond_cycles_are_detected_with_realistic_jitter(self):
        cases = (
            ('5 s cycle', [0.0, 5.08, 9.93, 15.05, 19.96]),
            ('10 s cycle', [0.0, 10.12, 19.91, 30.08]),
            ('30 s cycle', [1.0, 31.15, 60.90]),
            ('60 s cycle', [2.0, 62.20, 121.85]),
        )
        for expected, offsets in cases:
            with self.subTest(expected=expected):
                self.assertIn(expected, pacing.periodic(slow_at(offsets)))

    def test_phase_wraparound_near_zero_is_not_split(self):
        slows = slow_at([9.92, 19.95, 30.05, 40.02])
        got = pacing.periodic(slows)
        self.assertIn('4 of 4 late presents keep to a 10 s cycle', got)
        self.assertIn('4 cycles', got)

    def test_single_burst_does_not_masquerade_as_a_multisecond_cycle(self):
        tight_burst = [10.0 + i * 0.033333 for i in range(15)]
        self.assertEqual(pacing.periodic(slow_at(tight_burst)), '')

        spaced_burst_plus_strays = [10.0, 10.083333, 10.166667, 10.25, 20.0, 30.0]
        self.assertEqual(pacing.periodic(slow_at(spaced_burst_plus_strays)), '')

    def test_burst_buried_in_noise_does_not_fake_periodicity(self):
        offsets = [1.0 + i * 0.033333 for i in range(12)] + [
            4.37,
            7.81,
            12.19,
            16.64,
            23.08,
            28.53,
            34.92,
            41.27,
            49.61,
            55.04,
            63.48,
            71.83,
        ]
        self.assertEqual(pacing.periodic(slow_at(offsets)), '')


class PeriodicCorrelationTest(unittest.TestCase):
    def test_drifting_kernel_timer_is_correlated_even_when_fixed_phase_drifts(self):
        sched_times = [i * 10.09 for i in range(12)]
        msg = 'SwitchSched 1/2 (10090 ms): render-server[50] 54/54 FX - FIX: 100 runs 20/80/200 us'
        recs = [make_rec(msg, t) for t in sched_times]
        hit_offsets = [sched_times[i] + 0.03 for i in (1, 3, 5, 7, 9, 11)]
        noise_offsets = [13.7, 34.2, 56.1, 77.8]
        slows = slow_at(hit_offsets + noise_offsets)

        corrs = pacing.correlate_periodic(slows, recs)
        self.assertEqual(len(corrs), 1)
        self.assertEqual(corrs[0].family, 'SwitchSched')
        self.assertEqual((corrs[0].hits, corrs[0].episodes, corrs[0].events), (6, 10, 6))
        self.assertAlmostEqual(corrs[0].median_delta_ms, 30.0, delta=1.0)
        lines = pacing.periodic_correlations(slows, recs)
        self.assertIn('6 of 10 late presents (6/10 episodes', lines[0])
        self.assertIn('SwitchSched', lines[0])

    def test_every_periodic_kernel_family_prefix_is_matched(self):
        families = (
            ('SwitchPower', 'SwitchPower: 10000 ms: cpu0 c7 0.0%'),
            ('SwitchKprof', 'SwitchKprof: 10000 ms, 4000 samples'),
            ('SwitchDisplay stats', 'SwitchDisplay stats: 5000 ms: 300 DC A interrupts'),
            ('tegra-gm20b stats', 'tegra-gm20b: stats 5000 ms: 120 submits'),
        )
        for name, prefix in families:
            with self.subTest(family=name):
                times = [i * 10.0 for i in range(8)]
                recs = [make_rec(prefix, t) for t in times]
                slows = slow_at([times[i] + 0.02 for i in (1, 2, 4, 6)] + [15.5, 35.5, 55.5, 75.5])
                corrs = pacing.correlate_periodic(slows, recs)
                self.assertEqual([c.family for c in corrs], [name])

    def test_random_coincidences_single_event_burst_and_idle_suppression_are_rejected(self):
        # 10 idle minutes before SwitchDisplay stats becomes active for 1 minute at 5 s spacing.
        display_times = [600.0 + i * 5.0 for i in range(12)]
        recs = [make_rec('SwitchDisplay stats: 5000 ms: 300 DC A interrupts', t) for t in display_times]
        # 3 chance hits out of 25 episodes over the active minute (expected ~2.5 at p=0.10): must not trigger.
        chance_hits = [605.05, 625.10, 645.02]
        other_slows = [601.5 + i * 2.5 for i in range(22)]
        self.assertEqual(pacing.correlate_periodic(slow_at(chance_hits + other_slows), recs), [])

        # A burst near only one SwitchPower line must not trigger (>= 3 distinct lines required).
        power_recs = [make_rec('SwitchPower: 10000 ms: cpu0 c7 0.0%', i * 10.0) for i in range(10)]
        single_line_burst = [20.0 + i * 0.06 for i in range(6)]
        self.assertEqual(pacing.correlate_periodic(slow_at(single_line_burst), power_recs), [])


if __name__ == '__main__':
    unittest.main()
