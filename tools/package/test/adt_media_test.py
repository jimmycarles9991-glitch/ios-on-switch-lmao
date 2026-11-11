"""adt_edit.py over a minimal Switch tree: the IPSW's own iPad's answers to what iOS reads before it uses a feature."""

import importlib.util
import struct
import unittest
from pathlib import Path
from types import ModuleType

import native_oled_adt
from python.runfiles import runfiles

from tools.trustcache.trustcache import trust_module


def load_adt_edit() -> ModuleType:
    path = runfiles.Create().Rlocation('_main/tools/package/adt_edit.py')
    spec = importlib.util.spec_from_file_location('adt_edit', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prop(name: str, value: bytes) -> bytes:
    return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(-len(value) % 4)


def node(props: list[bytes], children: list[bytes]) -> bytes:
    return struct.pack('<II', len(props), len(children)) + b''.join(props) + b''.join(children)


def switch_tree() -> bytes:
    arm_io = node([prop('name', b'arm-io\0'), prop('device_type', b'nvidia,tegra210b01\0')], [])
    return node([prop('name', b'device-tree\0'), prop('compatible', b'nvidia,fric\0nintendo,nx\0')], [arm_io])


def apple_tree() -> bytes:
    """Apple's device tree of the selected IPSW kind's iPad (//ipsw:device_tree)."""
    return Path(runfiles.Create().Rlocation('_main/ipsw/device_tree.adt')).read_bytes()


def properties(node: list) -> dict[str, bytes]:
    return {p[0]: p[2] for p in node[0]}


def find(tree: list, *path: str) -> list:
    for name in path:
        tree = next(c for c in tree[1] if any(p[0] == 'name' and p[2] == name.encode() + b'\0' for p in c[0]))
    return tree


# The iPads the pinned IPSW kinds are for: model -> (platform-name, chip-id, soc-generation).
SOCS = {b'iPad7,1\0': (b't8011\0', 0x8011, b'H9\0'), b'iPad12,1\0': (b't8030\0', 0x8030, b'H12\0')}


class MediaKeys(unittest.TestCase):
    def test_arm_io_has_iommu_present(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        props = properties(find(tree, 'arm-io'))
        self.assertEqual(props['iommu-present'], b'')
        self.assertEqual(props['device_type'], b'nvidia,tegra210b01\0')


class Identity(unittest.TestCase):
    def setUp(self):
        self.adt_edit = load_adt_edit()
        self.apple, _ = self.adt_edit.adt_parse(apple_tree())
        adt = self.adt_edit.with_ipad_identity(self.adt_edit.ipad_adt(switch_tree()), apple_tree())
        self.tree, _ = self.adt_edit.adt_parse(adt)

    def test_root_is_apples_ipad_before_the_switch(self):
        root, apple = properties(self.tree), properties(self.apple)
        for key in ('model', 'target-type', 'target-sub-type'):
            self.assertEqual(root[key], apple[key], key)
        board = apple['compatible'].split(b'\0')[0]
        self.assertEqual(root['compatible'], board + b'\0nvidia,fric\0nintendo,nx\0')

    def test_soc_is_the_ipads(self):
        platform, chip, generation = SOCS[properties(self.apple)['model']]
        self.assertEqual(properties(self.tree)['platform-name'], platform)
        self.assertEqual(properties(find(self.tree, 'chosen'))['chip-id'], struct.pack('<I', chip))
        self.assertEqual(properties(find(self.tree, 'arm-io'))['soc-generation'], generation)
        self.assertEqual(properties(find(self.apple, 'arm-io'))['soc-generation'], generation)

    def test_product_is_apples_but_the_gamut(self):
        product, apple = properties(find(self.tree, 'product')), properties(find(self.apple, 'product'))
        for key in self.adt_edit.PRODUCT_IDENTITY:
            self.assertEqual(product.get(key), apple.get(key), key)
        self.assertEqual(product['model'], properties(self.apple)['model'])
        self.assertEqual(product['artwork-display-gamut'], b'sRGB\0')

    def test_backlight_white_point_type_is_apples(self):
        chosen, apple = properties(find(self.tree, 'chosen')), properties(find(self.apple, 'backlight'))
        self.assertEqual(apple['default-whitepoint-type'], struct.pack('<I', 1), 'J120 and J181 both say D65')
        self.assertEqual(chosen[self.adt_edit.WHITEPOINT_TYPE], apple['default-whitepoint-type'])
        self.assertFalse(any(p[0] == 'name' and p[2] == b'backlight\0' for c in self.tree[1] for p in c[0]))

    def test_usb_host_port_identity_is_apples(self):
        xhci = properties(find(self.tree, 'arm-io', 'usb-xhci'))
        host = self.adt_edit.apple_usb_host(self.apple)
        for key in self.adt_edit.USB_HOST_PORT_IDENTITY:
            self.assertEqual(xhci.get(key), host.get(key), key)
        if properties(self.apple)['model'] == b'iPad12,1\0':
            self.assertEqual(host['device_type'], b'usb-drd\0')
            self.assertEqual(xhci['port-type'], struct.pack('<I', 12))
            self.assertEqual(xhci['port-number'], struct.pack('<I', 1))
        else:
            self.assertEqual(host['device_type'], b'usb-xhci\0')
            self.assertNotIn('port-type', xhci)

    def test_audio_is_apples_acoustic_design(self):
        audio, apple = properties(find(self.tree, 'product', 'audio')), properties(find(self.apple, 'product', 'audio'))
        for key in self.adt_edit.AUDIO_IDENTITY:
            self.assertEqual(audio.get(key), apple.get(key), key)
        if properties(self.apple)['model'] == b'iPad12,1\0':
            self.assertEqual(audio['acoustic-id'], struct.pack('<I', 2003))
        else:
            self.assertNotIn('acoustic-id', audio)
        self.assertEqual(audio['wireless-splitter'], struct.pack('<I', 1))


class FramebufferFormat27(unittest.TestCase):
    """iOS 27's Core Animation renders into a BGRA8 framebuffer, the raw pack's states, only while three inputs hold.

    TegraVIC answers IOSurfaceAcceleratorCapabilitiesHDR 0, the booted tree has no /defaults/vmm-present, and /arm-io
    soc-generation is H12. If one changes, the framebuffer format changes and every iOS 27 Core Animation state
    misses the pack.
    """

    def test_inputs(self):
        adt_edit = load_adt_edit()
        apple = apple_tree()
        if properties(adt_edit.adt_parse(apple)[0])['model'] != b'iPad12,1\0':
            self.skipTest('the iOS 27 kind (J181, iPad12,1) only')
        booted = native_oled_adt.generate_adt(
            bytes(native_oled_adt.SEED_BYTES),
            native_oled_adt.Profile(storage_probe=True, root_boot=True, full_ram=True),
        )
        tree, _ = adt_edit.adt_parse(adt_edit.with_ipad_identity(adt_edit.ipad_adt(booted), apple))
        self.assertNotIn('vmm-present', properties(find(tree, 'defaults')))
        self.assertEqual(properties(find(tree, 'arm-io'))['soc-generation'], b'H12\0')
        vic = Path(runfiles.Create().Rlocation('_main/drivers/TegraVIC/TegraVICUserClient.cpp')).read_text()
        self.assertIn('putNumber(d, "IOSurfaceAcceleratorCapabilitiesHDR", 0);', vic)


class Capabilities(unittest.TestCase):
    def test_buttons_and_maps(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        buttons = properties(find(tree, 'buttons'))
        self.assertIn('function-button_volup', buttons)
        self.assertNotIn('function-button_halleffect', buttons)
        self.assertEqual(properties(find(tree, 'product', 'maps'))['3d-maps'], struct.pack('<I', 0))

    def test_syscfg_is_there_and_empty(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        syscfg = properties(find(tree, 'chosen'))['syscfg-v2-data']
        self.assertEqual(syscfg[:4], b'gfCS')
        self.assertEqual(len(syscfg), 24)
        self.assertEqual(struct.unpack_from('<I', syscfg, 0x14)[0], 0)


def h(n: int) -> bytes:
    return bytes([n]) * 20


class CdhashLists(unittest.TestCase):
    def test_each_role_lists_its_modules_cdhashes_ascending_and_once(self):
        adt_edit = load_adt_edit()
        system, translated = trust_module([h(9), h(3)]), trust_module([h(3), h(5)])
        adt = adt_edit.with_cdhash_lists(
            adt_edit.ipad_adt(switch_tree()),
            {'os-cryptex': [system, translated], 'app-cryptex': [trust_module([h(1)])]},
        )
        chosen = properties(find(adt_edit.adt_parse(adt)[0], 'chosen'))
        self.assertEqual(chosen['switch-os-cryptex-cdhashes'], h(3) + h(5) + h(9))
        self.assertEqual(chosen['switch-app-cryptex-cdhashes'], h(1))
        self.assertNotIn('switch-pretranslated-cdhashes', chosen)

    def test_refuses_what_is_not_a_trust_module_an_unknown_role_or_an_empty_list(self):
        adt_edit = load_adt_edit()
        adt = adt_edit.ipad_adt(switch_tree())
        for modules in ({'os-cryptex': [b'\x01' * 24]}, {'kernel': [trust_module([h(1)])]}, {'os-cryptex': []}):
            with self.subTest(modules=modules), self.assertRaises((SystemExit, ValueError)):
                adt_edit.with_cdhash_lists(adt, modules)


if __name__ == '__main__':
    unittest.main()
