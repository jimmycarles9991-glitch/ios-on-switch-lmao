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


if __name__ == '__main__':
    unittest.main()
