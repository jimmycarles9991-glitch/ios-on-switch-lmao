"""adt_edit.py's ipad_adt over a minimal Switch tree: J120's answers to what iOS reads before it uses a feature."""

import importlib.util
import struct
import unittest
from types import ModuleType

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


def find(tree: list, *path: str) -> list:
    for name in path:
        tree = next(c for c in tree[1] if any(p[0] == 'name' and p[2] == name.encode() + b'\0' for p in c[0]))
    return tree


class MediaKeys(unittest.TestCase):
    def test_arm_io_has_j120s_iommu_present_and_soc_generation(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        props = {p[0]: p[2] for p in find(tree, 'arm-io')[0]}
        self.assertIn('iommu-present', props)
        self.assertEqual(props['iommu-present'], b'')
        self.assertEqual(props['soc-generation'], b'H9\0')
        self.assertEqual(props['device_type'], b'nvidia,tegra210b01\0')


class Capabilities(unittest.TestCase):
    def test_root_and_product_carry_j120s_answers(self):
        adt_edit = load_adt_edit()
        tree, _ = adt_edit.adt_parse(adt_edit.ipad_adt(switch_tree()))
        root = {p[0]: p[2] for p in tree[0]}
        self.assertEqual(root['platform-name'], b't8011\0')
        self.assertEqual(root['target-sub-type'], b'J120AP\0')
        product = {p[0]: p[2] for p in find(tree, 'product')[0]}
        self.assertEqual(product['bluetooth-le'], struct.pack('<I', 1))
        for key in ('ui-pip', 'ui-pinned-app', 'ui-floating-live-app', 'ui-overlay-app'):
            self.assertEqual(product[key], b'', key)
        self.assertEqual(product['itunes-min-ver'], struct.pack('<I', 0xC0500))
        self.assertEqual(product['wifi-chipset'], b'4355\0')
        self.assertEqual(product['product-name'], b'iPad Pro (12.9-inch) (2nd generation)\0')
        buttons = {p[0]: p[2] for p in find(tree, 'buttons')[0]}
        self.assertIn('function-button_volup', buttons)
        self.assertNotIn('function-button_halleffect', buttons)
        maps = {p[0]: p[2] for p in find(tree, 'product', 'maps')[0]}
        self.assertEqual(maps['3d-maps'], struct.pack('<I', 0))


if __name__ == '__main__':
    unittest.main()
