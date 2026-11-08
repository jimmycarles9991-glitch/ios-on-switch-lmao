"""adt_edit.py over a minimal Switch tree: the IPSW's own iPad's answers to what iOS reads before it uses a feature."""

import importlib.util
import struct
import unittest
from pathlib import Path
from types import ModuleType

import native_oled_adt
from python.runfiles import runfiles


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


def trust_module(uuid: bytes) -> bytes:
    return struct.pack('<I', 2) + uuid + struct.pack('<I', 0)


def im4p(kind: bytes, payload: bytes) -> bytes:
    def der(tag: int, value: bytes) -> bytes:
        assert len(value) < 0x80
        return bytes([tag, len(value)]) + value

    return der(0x30, der(0x16, b'IM4P') + der(0x16, kind) + der(0x16, b'1') + der(0x04, payload))


class CryptexTrust(unittest.TestCase):
    def test_lists_each_modules_uuid_whether_apples_im4p_or_a_module(self):
        adt_edit = load_adt_edit()
        system, translated, app = bytes(range(16)), bytes(range(16, 32)), bytes(range(32, 48))
        adt = adt_edit.ipad_adt(switch_tree())
        adt = adt_edit.with_cryptex_trust(adt, 'os', [im4p(b'trcs', trust_module(system)), trust_module(translated)])
        adt = adt_edit.with_cryptex_trust(adt, 'app', [im4p(b'trca', trust_module(app))])
        chosen = properties(find(adt_edit.adt_parse(adt)[0], 'chosen'))
        self.assertEqual(chosen['switch-os-cryptex-trust'], system + translated)
        self.assertEqual(chosen['switch-app-cryptex-trust'], app)

    def test_refuses_what_is_not_a_trust_module(self):
        adt_edit = load_adt_edit()
        with self.assertRaises(SystemExit):
            adt_edit.with_cryptex_trust(adt_edit.ipad_adt(switch_tree()), 'os', [b'\x01' * 24])


if __name__ == '__main__':
    unittest.main()
