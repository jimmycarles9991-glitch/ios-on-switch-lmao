"""The selected kind's iOS 27 kernel names: names.py checked every pin and every kernel function a prelinked kext uses
against the shape of ours (shapes.py), found nothing, and the bindings that once reached a function with another
signature carry their own names. The iOS 17 kind has no fileset prelink, so nothing to check there.
"""

import json
import unittest
from pathlib import Path

from python.runfiles import runfiles

NAMES = '_main/kernel/build/xnu-12377/kernel.names.json'

# Each Apple address a kext reaches, with the function whose instructions it is and the kexts that import it.
SIGNATURES = {
    # lifs, AppleMobileFileIntegrity, AppleARMWatchdogTimer: kfree_type_impl read the var view as a zone
    '0xfffffff00a130414': '_kfree_type_var_impl',
    # lifs, AppleMatch: kfree_data took a garbage size
    '0xfffffff00a130430': '_kfree_data_addr',
    # IOSurface: port_name_to_task_name took name ports, skipped task evaluation, counted the kernel group
    '0xfffffff00a12c90c': '_port_name_to_task',
    # IOGPUFamily: copyClient allocated a client per query and returned its pointer as the IOReturn
    '0xfffffff00a8f3050': '__ZN19IOPerfControlClient18querySubmitterRoleEP9IOServiceP4taskPj',
    # IOGPUFamily: memcpy wrote to the page count as an address
    '0xfffffff00a227074': '_mach_vm_wire_level_monitor',
    # the sandbox's mount hook: vfs_vnodecovered blocks on a draining covered vnode
    '0xfffffff00a3218c8': '_vfs_vnodecovered_noblock',
}


class PrelinkShapesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        path = runfiles.Create().Rlocation(NAMES)
        if not path or not Path(path).exists():
            raise unittest.SkipTest('the selected kind has no fileset prelink')
        cls.names = json.loads(Path(path).read_text())

    def test_no_binding_differs_in_shape(self):
        self.assertEqual(self.names['shape_mismatches'], [])

    def test_bindings_by_signature(self):
        checked = set(self.names['shapes_checked'])
        for address, name in SIGNATURES.items():
            self.assertEqual(self.names['names'][address], name, address)
            self.assertIn(address, checked)


if __name__ == '__main__':
    unittest.main()
