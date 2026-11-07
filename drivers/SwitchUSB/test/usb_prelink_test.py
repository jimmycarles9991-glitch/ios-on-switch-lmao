"""Check the selected kind's prelink report for the host-side USB class kexts.

Every host-side USB class kext an iPad has is prelinked, and on iOS 27 no prelinked kext gains an unnamed kernel import:
an unnamed one is a trampoline that panics when called.
"""

import json
import unittest
from pathlib import Path

from python.runfiles import runfiles

REPORTS = ('_main/kernel/kernel.json', '_main/kernel/build/xnu-12377/kernel.json')

BOTH = {
    'com.apple.iokit.IOUSBHostFamily',
    'com.apple.driver.usb.AppleUSBXHCI',
    'com.apple.driver.usb.AppleUSBHub',
    'com.apple.driver.usb.AppleUSBHostCompositeDevice',
    'com.apple.driver.usb.AppleUSBHostiOSDevice',
    'com.apple.driver.AppleUSBHostMergeProperties',
    'com.apple.driver.usb.AppleUSBHostPacketFilter',
    'com.apple.driver.usb.IOUSBHostHIDDevice',
    'com.apple.driver.usb.IOUSBHostHIDDeviceSafeBoot',
    'com.apple.iokit.IOHIDEventDriver',
    'com.apple.iokit.IOHIDEventDriverSafeBoot',
    'com.apple.driver.AppleHIDKeyboard',
    'com.apple.driver.AppleHIDKeyboardEmbedded',
    'com.apple.driver.AppleTopCaseDriverV2',
    'com.apple.driver.AppleTopCaseHIDEventDriver',
    'com.apple.driver.AppleUSBTopCaseDriver',
    'com.apple.driver.AppleMultitouchDriver',
    'com.apple.driver.AppleMultitouchSPI',
    'com.apple.driver.AppleActuatorDriver',
    'com.apple.driver.ApplePencilDriver',
    'com.apple.driver.AppleGameControllerPersonality',
    'com.apple.driver.AppleUSBAudio',
    'com.apple.iokit.IOUSBMassStorageDriver',
    'com.apple.iokit.IOSCSIArchitectureModelFamily',
    'com.apple.iokit.IOSCSIBlockCommandsDevice',
    'com.apple.driver.AppleUSBCardReader',
    'com.apple.driver.AppleUSBMassStorageInterfaceNub',
    'com.apple.driver.USBStorageDeviceSpecifics',
    'com.apple.driver.SCSIDeviceSpecifics',
}
SERIAL = {
    'com.apple.driver.usb.cdc',
    'com.apple.driver.usb.cdc.acm',
    'com.apple.driver.usb.serial',
    'com.apple.driver.usb.networking',
    'com.apple.iokit.IOSerialFamily',
}
IOS17 = (
    BOTH
    | SERIAL
    | {
        'com.apple.driver.AppleTopCaseActuatorHIDDriver',
        'com.apple.filesystems.lifs',
        'com.apple.filesystems.hfs.kext',
    }
)
IOS27 = (
    BOTH
    | SERIAL
    | {
        'com.apple.filesystems.lifs',
        'com.apple.driver.usb.AppleUSBHostDeviceSupport',
        'com.apple.iokit.IOGameControllerFamily',
    }
)

# iOS 27 imports still unnamed, (functions, data), for every prelinked kext: a merge may only lower these. Every kext
# not listed, lifs, IOGameControllerFamily and IOSerialFamily among them, has none.
UNNAMED_27 = {
    'com.apple.filesystems.apfs': (98, 4),
    'com.apple.driver.AppleMobileFileIntegrity': (55, 2),
    'com.apple.security.sandbox': (39, 3),
    'com.apple.security.Image4': (6, 0),
    'com.apple.iokit.IOGPUFamily': (4, 2),
    'com.apple.iokit.IOSurface': (4, 2),
    'com.apple.driver.AppleM2ScalerCSCDriver': (3, 1),
    'com.apple.driver.ApplePMGR': (3, 0),
    'com.apple.iokit.IOHIDFamily': (3, 1),
    'com.apple.kec.corecrypto': (3, 0),
    'com.apple.driver.DiskImages': (2, 0),
    'com.apple.iokit.CoreAnalyticsFamily': (2, 0),
    'com.apple.iokit.IOAccessoryManager': (2, 1),
    'com.apple.iokit.IOReportFamily': (2, 0),
    'com.apple.iokit.IOUSBHostFamily': (2, 1),
    'com.apple.security.AppleImage4': (2, 1),
    'com.apple.driver.AppleARMPlatform': (1, 1),
    'com.apple.iokit.IOMobileGraphicsFamily': (1, 0),
    'com.apple.iokit.IOStorageFamily': (1, 1),
    'com.apple.driver.AppleARMWatchdogTimer': (0, 1),
    'com.apple.driver.usb.AppleUSBCommon': (0, 2),
    'com.apple.driver.usb.AppleUSBXHCI': (0, 1),
    'com.apple.iokit.IOSCSIArchitectureModelFamily': (0, 1),
    'com.apple.iokit.IOUSBMassStorageDriver': (0, 1),
}


def report() -> dict:
    """Read the one prelink report the selected kind built."""
    found = [Path(p) for p in map(runfiles.Create().Rlocation, REPORTS) if p and Path(p).exists()]
    if len(found) != 1:
        raise AssertionError(f'expected one prelink report, found {found}')
    return json.loads(found[0].read_text())


class UsbPrelinkTest(unittest.TestCase):
    """The host-side USB roots against the report."""

    @classmethod
    def setUpClass(cls) -> None:
        """Read the report once."""
        cls.report = report()
        cls.ios27 = 'unresolved_by_component' in cls.report
        cls.kexts = set(cls.report['components']) | set(cls.report['codeless_components'])

    def test_every_usb_class_kext_is_prelinked(self) -> None:
        """An accidental drop from either kind's roots fails here."""
        expected = IOS27 if self.ios27 else IOS17
        self.assertEqual(sorted(expected - self.kexts), [])

    def test_ios27_imports_stay_named(self) -> None:
        """No prelinked kext gains an unnamed kernel import."""
        if not self.ios27:
            self.skipTest('iOS 17 prelinks only when every import binds')
        unnamed = self.report['unresolved_by_component']
        for kext in sorted(self.kexts | set(unnamed)):
            entry = unnamed.get(kext, {'functions': [], 'data': []})
            counts = (len(entry['functions']), len(entry['data']))
            allowed = UNNAMED_27.get(kext, (0, 0))
            with self.subTest(kext=kext):
                self.assertLessEqual(counts[0], allowed[0], entry['functions'])
                self.assertLessEqual(counts[1], allowed[1], entry['data'])


if __name__ == '__main__':
    unittest.main()
