"""The install spec parsed from the current kernel sources matches what the boot path needs."""

import json
import unittest
from pathlib import Path

from python.runfiles import runfiles

from tools.install import spec

TREE = '_main/'


class SpecTest(unittest.TestCase):
    def test_current_sources(self):
        r = runfiles.Create()
        s = spec.parse(
            r.Rlocation(TREE + 'diagnostics/switch/SwitchRawMount.h'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/SwitchCryptex.h'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/SwitchCryptex.cpp'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/NativeSwitchBlock.cpp'),
            r.Rlocation(TREE + 'kernel/build/ipsw-volumes/switch_ipsw_volumes.h'),
        )
        self.assertNotIn('partition_bytes', s)
        self.assertEqual(s['partition_name'], 'iOS')
        self.assertEqual(s['partition_type'], '7C3457EF-0000-11AA-AA11-00306543ECAC')
        self.assertEqual(s['volumes']['data']['uuid'], 'D1B1D053-D0A6-496C-8625-A947A3DA863E')
        self.assertEqual(s['volumes']['preboot']['uuid'], '4AA4A761-5138-439D-9A30-D4138F6887FA')

    def test_the_selected_kind_volumes(self):
        """The System volume and cryptexes the kernel mounts are the ones the selected IPSW kind's image holds."""
        r = runfiles.Create()
        volumes = json.loads(Path(r.Rlocation(TREE + 'install/ipsw.json')).read_text())['volumes']
        s = spec.parse(
            r.Rlocation(TREE + 'diagnostics/switch/SwitchRawMount.h'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/SwitchCryptex.h'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/SwitchCryptex.cpp'),
            r.Rlocation(TREE + 'drivers/NativeSwitchBlock/NativeSwitchBlock.cpp'),
            r.Rlocation(TREE + 'kernel/build/ipsw-volumes/switch_ipsw_volumes.h'),
        )
        self.assertEqual(s['volumes']['system']['uuid'], volumes['os_volume_uuid'].upper())
        self.assertEqual(
            [(c['member'], c['path'], c['volume_uuid']) for c in s['cryptexes']],
            [
                (
                    member,
                    '/private/preboot/Switch-Staging/' + volumes[member + '_image'],
                    volumes[member + '_uuid'].upper(),
                )
                for member in ('system_cryptex', 'app_cryptex')
            ],
        )


if __name__ == '__main__':
    unittest.main()
