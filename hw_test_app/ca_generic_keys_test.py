"""ca_generic_keys.py over each IPSW's QuartzCore tables: the keys are the ones the device's long spans name."""

import os
import unittest

from hw_test_app import ca_generic_keys

# Boot 22 (T-v6, 24B119): backboardd's long spans of the three generic states it drew, e.g. 13:47:09.113
# "41339 us draws kb0f4f3daabb01e2e 63696 instr ...", and the name the same state was logged under at 13:47:20.088
DRAWN_24B119 = {
    0xB0F4F3DAABB01E2E: 'PBGRAXmw_A3Xghfc',
    0x64ED02FD5D7A1865: 'PBGRAXm_A2Xghfc',
    0x12016B9F2603FAB8: 'PRGhAXm_A2Xghfc',
}


class GenericKeys(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.keys = {
            build: dict(ca_generic_keys.generic_keys(ca_generic_keys.caspec.Tables(os.environ[f'QC_{build}'])))
            for build in ('24B119', '21H461')
        }

    def test_the_states_ios_27_drew_on_the_device_are_its_generic_keys(self) -> None:
        for key, name in DRAWN_24B119.items():
            self.assertEqual(self.keys['24B119'].get(key), name, f'{key:016x}')

    def test_each_build_has_its_own(self) -> None:
        self.assertEqual(len(self.keys['24B119']), 19)
        self.assertEqual(len(self.keys['21H461']), 18)
        self.assertNotIn(0xB0F4F3DAABB01E2E, self.keys['21H461'], "iOS 17's PBGRAXmw_A3Xghfc is another state")

    def test_every_key_is_a_generic_state(self) -> None:
        for build, keys in self.keys.items():
            for name in keys.values():
                self.assertIn('Xg', name, build)


if __name__ == '__main__':
    unittest.main()
