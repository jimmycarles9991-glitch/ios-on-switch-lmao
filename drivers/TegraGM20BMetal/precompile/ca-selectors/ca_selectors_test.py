"""precompile/ca_selectors.py: each IPSW kind's selector table parses, and its header carries the layout."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ca_selectors

TABLES = Path(__file__).resolve().parent
# boot 21's b0f4f3daabb01e2e config with stale arena bytes in 49-51, which no selector compares
STALE = bytes.fromhex(
    '0000000000000000000000000101ff000000000000000000000000000000ff000001030001000100000000000000000000003c003c'
)


class CaSelectorsTest(unittest.TestCase):
    def test_every_kind_has_a_table_of_a_52_byte_config_at_fragment_buffer_0(self):
        for kind in ('21H461', '24B119'):
            s = ca_selectors.parse((TABLES / f'{kind}.txt').read_text())
            self.assertEqual((s.slot, s.length), (0, 52), kind)
            self.assertTrue({49, 50, 51} <= s.padding, kind)

    def test_the_ios27_padding_is_the_bytes_device_dumps_showed_stale_or_always_zero(self):
        s = ca_selectors.parse((TABLES / '24B119.txt').read_text())
        self.assertEqual(sorted(s.padding), [3, 15, 19, 31, 49, 50, 51])
        self.assertEqual(s.padding_mask() & (1 << 49 | 1 << 50 | 1 << 51), 1 << 49 | 1 << 50 | 1 << 51)
        self.assertTrue(any(STALE[i] for i in s.padding))

    def test_the_ios27_table_lists_boot_21s_configs_of_the_three_generic_states(self):
        s = ca_selectors.parse((TABLES / '24B119.txt').read_text())
        self.assertEqual({k for k, _ in s.configs}, {0xB0F4F3DAABB01E2E, 0x64ED02FD5D7A1865, 0x12016B9F2603FAB8})
        self.assertEqual(len(s.configs), 81)
        self.assertIn((0xB0F4F3DAABB01E2E, STALE[:49] + bytes(3)), s.configs)

    def test_ios17_lists_none_until_a_device_draws_them(self):
        self.assertEqual(ca_selectors.parse((TABLES / '21H461.txt').read_text()).configs, [])

    def test_the_header_names_the_layout(self):
        s = ca_selectors.parse('layout slot=2 length=8 padding=1,4-5\n')
        h = ca_selectors.header(s, 'x.txt')
        self.assertIn('kCaSelectorSlot = 2, kCaSelectorLength = 8;', h)
        self.assertIn('kCaSelectorPadding = 0x32ull;', h)

    def test_config_lines_are_kept_and_checked_against_the_layout(self):
        s = ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a0b\n')
        self.assertEqual(s.configs, [(0xB0F4F3DAABB01E2E, b'\x0a\x0b')])
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('b0f4f3daabb01e2e 0a0b\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=65 padding=\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=1\nb0f4f3daabb01e2e 0a0b\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a0b\nb0f4f3daabb01e2e 0a0b\n')

    def test_global_ids_are_shared_between_states_and_padding_never_selects_another_config(self):
        s = ca_selectors.parse(
            'layout slot=0 length=3 padding=1\n'
            '0000000000000001 010003\n0000000000000002 010003\n0000000000000002 020004\n'
        )
        self.assertEqual(ca_selectors.selector_id(s, b'\x01\xff\x03'), 1)
        self.assertEqual(ca_selectors.selector_id(s, b'\x02\x00\x04'), 2)
        self.assertEqual(ca_selectors.selector_id(s, b'\x01\x00\x04'), 0)
        self.assertEqual(ca_selectors.selector_id(s, b'\x01\x00'), 0)

    def test_every_observed_config_has_its_own_direct_id_and_unseen_fields_keep_the_generic(self):
        s = ca_selectors.parse((TABLES / '24B119.txt').read_text())
        configs, _, _ = ca_selectors.dispatch(s)
        self.assertEqual(len(configs), 73)
        for index, value in enumerate(configs, 1):
            self.assertEqual(ca_selectors.selector_id(s, value), index)
        unknown = bytearray(configs[0])
        unknown[48] ^= 1
        self.assertEqual(ca_selectors.selector_id(s, bytes(unknown)), 0)

    def test_a_kind_with_no_observations_has_only_the_generic_id(self):
        s = ca_selectors.parse((TABLES / '21H461.txt').read_text())
        self.assertEqual(ca_selectors.selector_id(s, STALE), 0)
        self.assertIn('kCaSelectorConfigs = 0;', ca_selectors.header(s, '21H461.txt'))


if __name__ == '__main__':
    unittest.main()
