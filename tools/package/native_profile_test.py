"""Each kind's pinned kernel window base, the boot base the loader and the kernel derive from it, and the exporter."""

import ast
import os
import re
import unittest
from pathlib import Path

from build_native_loader import FULL_RAM_BANK, _check_exporter, boot_virt_base
from loader_abi import EXPORTER_RUN, KERNEL_PHYS_BASE, WINDOW_BASE, WINDOW_BYTES_MAX, WINDOW_SIZE
from native_xnu_layout import kernel_phys_base

BLOCK = 0x200000
BOOT_LAYOUT = 'kernel/switch/include/switch/boot_layout.h'
GEOMETRY_SOURCES = (
    'kernel/switch/ram/ram.c',
    'kernel/xnu-10063.141.1/osfmk/arm64/start.s',
    'kernel/xnu-12377.121.6/osfmk/arm64/start.s',
)


def checkout_file(relative: str) -> Path:
    """Find a source file: in the runfiles under Bazel, in the checkout otherwise."""
    for base in (os.environ.get('TEST_SRCDIR', ''), str(Path(__file__).resolve().parents[2])):
        for root in (Path(base) / '_main', Path(base)):
            if (root / relative).exists():
                return root / relative
    raise FileNotFoundError(relative)


def pinned_bases() -> dict[str, int]:
    """Return every built kind's derived kernel_virt_base, read from ipsw/pins.bzl as ipsw/pin.py reads it."""
    kinds = ast.literal_eval(checkout_file('ipsw/pins.bzl').read_text().partition('IPSWS = ')[2])
    return {kind: int(pin['derived']['kernel_virt_base'], 16) for kind, pin in kinds.items() if pin.get('xnu')}


def boot_layout() -> dict[str, str]:
    """Return boot_layout.h's #defines, name to replacement text."""
    return dict(re.findall(r'^#define (SWITCH_\w+)\s+(.+)$', checkout_file(BOOT_LAYOUT).read_text(), re.MULTILINE))


def evaluate(text: str, names: dict[str, str]) -> int:
    """Evaluate a #define's integer + and - expression over the other #defines, modulo 2**64 as the kernel's is."""

    def value(node: ast.expr) -> int:
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            return evaluate(names[node.id], names)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add | ast.Sub):
            left, right = value(node.left), value(node.right)
            return left + right if isinstance(node.op, ast.Add) else left - right
        raise ValueError(f'not an integer + and - expression: {ast.dump(node)}')

    return value(ast.parse(text, mode='eval').body) % (1 << 64)


class NativeProfileTest(unittest.TestCase):
    def test_every_built_kind_pins_a_base_congruent_with_the_window_modulo_2_mib(self):
        bases = pinned_bases()
        self.assertEqual(set(bases), {'21H461', '24B119'})
        for kind, base in bases.items():
            with self.subTest(kind=kind):
                self.assertEqual(base % BLOCK, KERNEL_PHYS_BASE % BLOCK)

    def test_the_trust_caches_room_below_each_kinds_kernel(self):
        room = KERNEL_PHYS_BASE - WINDOW_BASE
        self.assertEqual((WINDOW_BASE % BLOCK, room % 0x4000), (0, 0))
        self.assertEqual(WINDOW_BASE + WINDOW_SIZE, 0xE2200000)
        for kind, base in pinned_bases().items():
            with self.subTest(kind=kind):
                self.assertEqual(kernel_phys_base(WINDOW_BASE, base, room), KERNEL_PHYS_BASE)
                self.assertEqual(kernel_phys_base(WINDOW_BASE, base, room + 1), KERNEL_PHYS_BASE + BLOCK)

    def test_the_loader_decodes_the_largest_window_into_its_copy_tables(self):
        copy_h = checkout_file('boot/loader/copy.h').read_text()
        self.assertEqual(
            int(re.search(r'#define LOADER_WINDOW_BYTES_MAX UINT64_C\((0x[0-9a-f]+)\)', copy_h).group(1), 16),
            WINDOW_BYTES_MAX,
        )
        self.assertLessEqual(WINDOW_BASE + WINDOW_BYTES_MAX, 0xC0000000)
        self.assertLessEqual(WINDOW_BYTES_MAX, WINDOW_SIZE - 0x200000)

    def test_the_rule_reproduces_the_measured_21h461_boot_base(self):
        base = boot_virt_base(pinned_bases()['21H461'], KERNEL_PHYS_BASE, FULL_RAM_BANK[0])
        self.assertEqual(base, 0xFFFFFFEFD2200000)

    def test_24b119_boot_base_follows_from_its_prelink(self):
        base = boot_virt_base(pinned_bases()['24B119'], KERNEL_PHYS_BASE, FULL_RAM_BANK[0])
        self.assertEqual(base, 0xFFFFFFEFD1000000)

    def test_the_kernel_expects_the_geometry_the_loader_hands_over(self):
        names = boot_layout()
        self.assertEqual(evaluate(names['SWITCH_BOOT_PHYS_BASE'], names), FULL_RAM_BANK[0])
        self.assertEqual(evaluate(names['SWITCH_BOOT_MEM_SIZE'], names), FULL_RAM_BANK[1])
        self.assertEqual(evaluate(names['SWITCH_KERNEL_PHYS_BASE'], names), KERNEL_PHYS_BASE)
        for kind, base in pinned_bases().items():
            with self.subTest(kind=kind):
                expected = boot_virt_base(base, KERNEL_PHYS_BASE, FULL_RAM_BANK[0])
                given = names | {'SWITCH_KERNEL_VIRT_BASE': hex(base)}
                self.assertEqual(evaluate(names['SWITCH_BOOT_VIRT_BASE'], given), expected)

    def test_no_kernel_geometry_check_names_one_kinds_base(self):
        literals = set()
        for base in pinned_bases().values():
            literals |= {base, boot_virt_base(base, KERNEL_PHYS_BASE, FULL_RAM_BANK[0])}
        for relative in GEOMETRY_SOURCES:
            numbers = {int(n, 16) for n in re.findall(r'0x[0-9a-fA-F]+', checkout_file(relative).read_text())}
            with self.subTest(source=relative):
                self.assertFalse(numbers & literals, 'use <switch/boot_layout.h>')


class ExporterTest(unittest.TestCase):
    def test_the_largest_window_the_loader_copies_clears_the_exporter(self):
        _check_exporter(WINDOW_BASE, WINDOW_BYTES_MAX)

    def test_a_window_over_the_exporter_is_refused(self):
        with self.assertRaises(ValueError):
            _check_exporter(EXPORTER_RUN[0] - 0x1000, 0x2000)


if __name__ == '__main__':
    unittest.main()
