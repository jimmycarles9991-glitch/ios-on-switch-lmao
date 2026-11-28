"""report.kernel_log reading the kernel's ring: wrapped, duplicated and kextlog-coloured lines."""

import unittest

from tools.emu import report


class KernelLogTest(unittest.TestCase):
    def test_oldest_first_and_each_line_once(self):
        ring = b'[     2.000000]: second\n[     1.500000]: first\n[     2.000000]: second\n'
        self.assertEqual(report.kernel_log(ring), ['[     1.500000] first', '[     2.000000] second'])

    def test_a_kextlog_line_keeps_its_text_without_the_colour(self):
        text = 'Requesting daemon launch for com.apple.AppleUserHIDDrivers with serverName x-10000033a and tag 7'
        ring = b'[    16.873000]: \x1b[35m' + text.encode() + b'\x1b[0m\n'
        self.assertEqual(report.kernel_log(ring), [f'[    16.873000] {text}'])

    def test_text_after_a_message_that_starts_with_a_newline_takes_the_stamp_before_it(self):
        ring = (
            b'[    10.000000]: tcp listen: x\nso_gencnt: 62 SwitchBootPrefetch: 0 MiB paged in ahead\n'
            b'[    10.500000]: next\n'
        )
        self.assertEqual(
            report.kernel_log(ring),
            [
                '[    10.000000] tcp listen: x',
                '[    10.000000] so_gencnt: 62 SwitchBootPrefetch: 0 MiB paged in ahead',
                '[    10.500000] next',
            ],
        )

    def test_a_tab_or_a_non_ascii_byte_stays_inside_its_line(self):
        ring = b'[     4.000000]: col\tumn \xe2\x86\x92 end\n'
        self.assertEqual(report.kernel_log(ring), ['[     4.000000] col\tumn \u2192 end'])

    def test_polls_merge_by_stamp_and_keep_the_order_within_one(self):
        first = ['[     1.000000] b', '[     2.000000] z']
        second = ['[     1.000000] b', '[     1.000000] a', '[     3.000000] c']
        self.assertEqual(
            report.merge(first, second),
            ['[     1.000000] b', '[     1.000000] a', '[     2.000000] z', '[     3.000000] c'],
        )

    def test_a_wrapped_ring_drops_the_partial_line_before_its_first_stamp(self):
        ring = b'half a line\n[     3.000000]: whole\n'
        self.assertEqual(report.kernel_log(ring), ['[     3.000000] whole'])


if __name__ == '__main__':
    unittest.main()
