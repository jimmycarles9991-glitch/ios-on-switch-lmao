"""Check the selected kind's prelink for the USB kexts.

Every USB kext an iPad's kernelcache has is prelinked, each with the personalities Apple's kernelcache gives it less
the ones SwitchXHCI adds back when the host comes up, and on iOS 27 no prelinked kext has an unnamed kernel import:
an unnamed one is a trampoline that panics when called.
"""

import json
import struct
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from native_prelink import read_xml
from python.runfiles import runfiles

from tools.common.macho import backed_offset, macho

REPORTS = ('_main/kernel/kernel.json', '_main/kernel/build/xnu-12377/kernel.json')
PRELINKED = ('_main/kernel/kernel.macho', '_main/kernel/build/xnu-12377/kernel.macho')
KERNELCACHE = ('_main/ipsw/kernelcache.decompressed',)

BOTH = {
    'com.apple.iokit.IOUSBDeviceFamily',
    'com.apple.driver.usb.AppleUSBCommon',
    'com.apple.driver.AppleUSBDeviceMux',
    'com.apple.driver.AppleUSBMike',
    'com.apple.driver.AppleUSBDeviceAudioController',
    'com.apple.driver.AppleIDAMInterface',
    'com.apple.driver.AppleUSBDeviceNCM',
    'com.apple.iokit.IOUSBHostFamily',
    'com.apple.driver.usb.AppleUSBXHCI',
    'com.apple.iokit.IOAccessoryManager',
    'com.apple.iokit.IOPortFamily',
    'com.apple.driver.usb.AppleUSBHostPacketFilter',
    'com.apple.driver.usb.AppleUSBHub',
    'com.apple.driver.usb.AppleUSBHostCompositeDevice',
    'com.apple.driver.usb.AppleUSBHostiOSDevice',
    'com.apple.driver.AppleUSBHostMergeProperties',
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
    'com.apple.driver.AppleInputDeviceSupport',
    'com.apple.driver.IOSlaveProcessor',
    'com.apple.driver.AppleGameControllerPersonality',
    'com.apple.driver.AppleUSBAudio',
    'com.apple.iokit.IOUSBMassStorageDriver',
    'com.apple.iokit.IOSCSIArchitectureModelFamily',
    'com.apple.iokit.IOSCSIBlockCommandsDevice',
    'com.apple.iokit.IOPCIFamily',
    'com.apple.driver.AppleUSBCardReader',
    'com.apple.driver.AppleUSBMassStorageInterfaceNub',
    'com.apple.driver.USBStorageDeviceSpecifics',
    'com.apple.driver.SCSIDeviceSpecifics',
    'com.apple.filesystems.lifs',
    'com.apple.driver.usb.cdc.ecm',
    'com.apple.driver.usb.cdc.ncm',
    'com.apple.driver.usb.ethernet.asix',
    'com.apple.driver.AppleUSBEthernetHost',
    'com.apple.iokit.IONetworkingFamily',
    'com.apple.iokit.IOSkywalkFamily',
    'com.apple.driver.mDNSOffloadUserClient-Embedded',
    'com.apple.driver.AppleOnboardSerial',
    'com.apple.iokit.IOAccessoryPortUSB',
}
SERIAL = {
    'com.apple.driver.usb.cdc',
    'com.apple.driver.usb.cdc.acm',
    'com.apple.driver.usb.serial',
    'com.apple.driver.usb.networking',
    'com.apple.iokit.IOSerialFamily',
    'com.apple.driver.driverkit.serial',
}
IOS17 = (
    BOTH
    | SERIAL
    | {
        'com.apple.driver.AppleTopCaseActuatorHIDDriver',
        'com.apple.driver.AppleFirmwareUpdateKext',
        'com.apple.filesystems.hfs.kext',
    }
)
IOS27 = (
    BOTH
    | SERIAL
    | {
        'com.apple.driver.usb.AppleUSBHostDeviceSupport',
        'com.apple.iokit.IOGameControllerFamily',
    }
)

DEFERRED17 = {
    'com.apple.iokit.IOUSBHostFamily': {'AppleUSBHostResources', 'AppleUSBHostResourcesTypeC'},
    'com.apple.iokit.IOUSBMassStorageDriver': {'IOUSBMassStorageResource'},
}
DEFERRED27 = DEFERRED17 | {'com.apple.driver.usb.AppleUSBHostPacketFilter': {'AppleUSBHostPacketFilter'}}


def runfile(candidates: tuple[str, ...]) -> Path:
    """Find the one of `candidates` the selected kind built."""
    found = [Path(p) for p in map(runfiles.Create().Rlocation, candidates) if p and Path(p).exists()]
    if len(found) != 1:
        raise AssertionError(f'expected one of {candidates}, found {found}')
    return found[0]


def unserialized(value: object) -> object:
    """Give `value` as OSUnserializeXML makes it: every integer 64-bit (24B119's -10000 is the prelink's 0x...d8f0)."""
    if isinstance(value, dict):
        return {key: unserialized(child) for key, child in value.items()}
    if isinstance(value, list):
        return [unserialized(child) for child in value]
    if isinstance(value, int) and not isinstance(value, bool):
        return value & ((1 << 64) - 1)
    return value


def prelink_info(path: Path) -> dict[str, dict]:
    """Read a kernel's or kernelcache's prelink info, by bundle identifier."""
    data = path.read_bytes()
    segment = next(s for s in macho(data, subtypes=None)['segments'] if s['name'] == '__PRELINK_INFO')
    xml = data[segment['fileoff'] : segment['fileoff'] + segment['filesize']].split(b'\0', 1)[0]
    root = ET.fromstring(xml)
    info = unserialized(read_xml(root[0] if root.tag == 'plist' else root, {}))
    return {entry['CFBundleIdentifier']: entry for entry in info['_PrelinkInfoDictionary']}


class UsbPrelinkTest(unittest.TestCase):
    """The USB kexts against the selected kind's prelink."""

    @classmethod
    def setUpClass(cls) -> None:
        """Read the report, the prelinked kernel's prelink info and the kernelcache's once."""
        cls.report = json.loads(runfile(REPORTS).read_text())
        cls.ios27 = 'unresolved_by_component' in cls.report
        cls.kexts = set(cls.report['components']) | set(cls.report['codeless_components'])
        cls.expected = IOS27 if cls.ios27 else IOS17
        cls.deferred = DEFERRED27 if cls.ios27 else DEFERRED17
        cls.prelinked = prelink_info(runfile(PRELINKED))
        cls.apple = prelink_info(runfile(KERNELCACHE))

    def test_every_usb_kext_is_prelinked(self) -> None:
        """An accidental drop from either kind's lists fails here."""
        self.assertEqual(sorted(self.expected - self.kexts), [])
        self.assertEqual(sorted(self.expected - set(self.prelinked)), [])

    def test_personalities_are_apples_less_the_deferred(self) -> None:
        """Each USB kext carries every personality of Apple's but the deferred ones, each as Apple has it."""
        for kext in sorted(self.expected):
            apple = self.apple[kext].get('IOKitPersonalities', {})
            deferred = self.deferred.get(kext, set())
            with self.subTest(kext=kext):
                self.assertLessEqual(deferred, set(apple), 'a deferred personality Apple does not have')
                wanted = {name: p for name, p in apple.items() if name not in deferred}
                self.assertEqual(self.prelinked[kext].get('IOKitPersonalities', {}), wanted)

    def test_the_declared_sites_are_written(self) -> None:
        """Each reported site holds its replacement in the prelinked kernel.

        AppleUSBXHCI runs on the falcon's one interrupter only with the kind's declared sites in its code
        (kits/SwitchABI/kinds/<kind>/sites).
        """
        reported = self.report['declared_sites']
        self.assertIn('com.apple.driver.usb.AppleUSBXHCI', {site['bundle'] for site in reported})
        if self.ios27:
            self.assertIn('com.apple.iokit.IOUSBHostFamily', {site['bundle'] for site in reported})
        image = runfile(PRELINKED).read_bytes()
        segments = macho(image, subtypes=None)['segments']
        for site in reported:
            with self.subTest(site=site['at']):
                self.assertIn(site['bundle'], self.kexts)
                at = backed_offset(segments, int(site['va'], 16), 4, len(image))
                self.assertEqual(f'{struct.unpack_from("<I", image, at)[0]:08x}', site['to'])

    def test_ios27_imports_are_named(self) -> None:
        """No prelinked kext has a kernel import that nothing names."""
        if not self.ios27:
            self.skipTest('iOS 17 prelinks only when every import binds')
        self.assertEqual(self.report['unresolved_by_component'], {})


if __name__ == '__main__':
    unittest.main()
