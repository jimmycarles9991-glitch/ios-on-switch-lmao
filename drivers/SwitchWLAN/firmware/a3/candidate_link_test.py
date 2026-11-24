import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.a3 import payload_header

SWITCH_ROOTS = (
    'channel_gate',
    'pm_status_entry',
    'pm_prepare_entry',
    'pm_submit_entry',
    'pm_set_entry',
    'tx_entry',
    'tx_radio_status',
)


class CandidateLink(unittest.TestCase):
    def test_candidate_preserves_station_heap_and_thumb_hooks(self):
        for development in (0, 1):
            with self.subTest(development=development):
                elf = Path(__file__).with_name('candidate.elf' if development else 'candidate_release.elf')
                binary = elf.with_suffix('.bin')
                symbols = payload_header.switch_a3_symbols(elf.read_bytes())
                begin, end = symbols['switch_a3_island_start'], symbols['switch_a3_island_end']
                self.assertEqual(begin, 0x20E7DC)
                self.assertLessEqual(end - begin, 16384)
                self.assertGreaterEqual(0x23DF6C - end, 178064)
                self.assertEqual(symbols['switch_a3_tx_phy_chanspec'], 0x8A13D)
                for name in SWITCH_ROOTS:
                    address = symbols['switch_a3_' + name]
                    self.assertTrue(address & 1, name)
                    self.assertTrue(begin <= (address & ~1) < end, name)
                if not development:
                    self.assertNotIn(b'switch_awdl_deadline', binary.read_bytes())
                    self.assertNotIn('switch_a3_control_deadline_snapshot', symbols)
                    self.assertFalse(
                        [
                            name
                            for name in symbols
                            if name.startswith(('switch_a3_diagnostic', 'switch_a3_control_record'))
                            or 'ga3diag' in name.lower()
                        ]
                    )
                else:
                    self.assertIn('switch_a3_diagnostic', symbols)
                self.assertIn(
                    'switch_wlan_a3_candidate_payload',
                    payload_header.switch_a3_header(
                        elf.read_bytes(),
                        binary.read_bytes(),
                        candidate=True,
                    ),
                )


if __name__ == '__main__':
    unittest.main()
