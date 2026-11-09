"""tools/hwtest/metrics.py over the keys boot 13's run logged."""

import unittest

from tools.hwtest import metrics


class Direction(unittest.TestCase):
    def test_times_go_down(self):
        for key in ('raw_gpu_ms0', 'raw_t_ds4', 'raw_us_pbgraxm_tds4a2xhf_isrc', 'wall_us_x', 'alu_ms', 'compile_ms'):
            self.assertEqual(metrics.direction(key), -1, key)
        self.assertEqual(metrics.direction('raw_ms_radius2'), -1)
        self.assertEqual(metrics.direction('link_missed'), -1)
        self.assertEqual(metrics.direction('throttle_pct'), -1)
        self.assertEqual(metrics.direction('m2v_pct'), -1)
        self.assertEqual(metrics.direction('clear_submit_us_p99'), -1)

    def test_rates_go_up(self):
        for key in (
            'gflops',
            'copy_gbps',
            'fill_gpix_s',
            'fps',
            'read_mb_s',
            'mhz_end',
            'mops',
            'speedup',
            'link_mbps',
        ):
            self.assertEqual(metrics.direction(key), 1, key)

    def test_counts_and_settings_are_not_performance(self):
        for key in ('raw_wrong', 'threads_per_group', 'max_buffer_mb', 'raw_bias_4096', 'raw_cbs', 'status'):
            self.assertEqual(metrics.direction(key), 0, key)

    def test_flags_are_not_numbers(self):
        self.assertIsNone(metrics.number(True))
        self.assertIsNone(metrics.number('1.5'))
        self.assertEqual(metrics.number(3), 3.0)


class Band(unittest.TestCase):
    def test_ten_percent_until_history_is_noisier(self):
        self.assertAlmostEqual(metrics.band(10.0, []), 1.0)
        self.assertAlmostEqual(metrics.band(10.0, [10.0, 10.0]), 1.0)
        self.assertAlmostEqual(metrics.band(10.0, [6.0, 10.0, 14.0]), 2 * (32 / 3) ** 0.5)

    def test_near_zero_has_a_floor(self):
        self.assertEqual(metrics.band(0.0, []), metrics.ABSOLUTE_FLOOR)


class Comparable(unittest.TestCase):
    def entry(self, subsystem: str = 'Metal / GPU', **values: object) -> dict:
        return {'subsystem': subsystem, 'metrics': values}

    def test_nak_debug_change_stops_gpu_comparisons_only(self):
        identities = ({'nak_debug': ''}, {'nak_debug': 'sm53_probe'})
        self.assertEqual(
            metrics.not_comparable(self.entry(), self.entry(), identities), 'nak_debug changed (sm53_probe -> -)'
        )
        self.assertIsNone(metrics.not_comparable(self.entry('Storage'), self.entry('Storage'), identities))
        same = ({'nak_debug': 'sm53_probe'}, {'nak_debug': 'sm53_probe'})
        self.assertIsNone(metrics.not_comparable(self.entry(), self.entry(), same))

    def test_a_gpu_clock_change_stops_the_test(self):
        identities = ({}, {})
        now = self.entry(raw_gpu_clock='ns')
        self.assertEqual(
            metrics.not_comparable(now, self.entry(raw_gpu_clock='host'), identities), 'raw_gpu_clock changed'
        )
        self.assertEqual(metrics.not_comparable(now, self.entry(), identities), 'raw_gpu_clock changed')
        self.assertIsNone(metrics.not_comparable(now, self.entry(raw_gpu_clock='ns'), identities))


if __name__ == '__main__':
    unittest.main()
