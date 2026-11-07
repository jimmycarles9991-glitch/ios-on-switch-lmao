"""bl33.bin as this entry ships it (boot/loader/stage.h), proven on the host.

The composed file as hekate puts it at 0xAA000000 holds the boot script, loader, exporter and DTB where the patched
environment and the script look, satisfies every memory check of the script, and U-Boot's bytes are untouched apart
from its default environment. That the loader's own decoder (copy.c) reads the exact window back from the addresses it
was built for is //boot/loader/test:bl33_window_test.
"""

import random
import re
import struct
import unittest
from pathlib import Path

# //tools/package's bl33, boot_script and kernel_window (lz4 from //tools:lz4 on PATH); the pinned firmware and the
# script template from runfiles (the test runs in the main repository's runfiles directory).
import bl33
from bl33 import bl33_patch
from boot_script import boot_script, script_image
from build_native_loader import (
    BL33_BASE,
    STAGE_HEADER,
    STAGE_SOURCE,
    compress_window,
    pack_window,
    stage_id,
    stage_into_bl33,
)
from loader_abi import BOOT_ARGS_PAGE, DTB_SLOT, EXPORTER_SLOT, LOADER_SLOT, SCRIPT_SLOT
from uimage import LOAD, Header, legacy_image

BL33 = Path('bazel/pinned/firmware/bl33.bin')
EXPORTER = Path('bazel/pinned/firmware/exporter.uimg')
DTB = Path('bazel/pinned/firmware/oled-sku3.dtb')
TEMPLATE = Path('boot/u-boot-script.txt')


def window_and_stage():
    rng = random.Random(1104)
    window = bytearray(1536 * 4096)
    for page in rng.sample(range(1536), 500):
        at = page * 4096
        window[at : at + 4096] = rng.randbytes(64) * 64 if page % 3 else bytes([page & 255 or 1]) * 4096
    runs, _ = pack_window(bytes(window))
    blocks, stream = compress_window(bytes(window), runs)
    staged = stream + bytes(-len(stream) % 4096)
    return bytes(window), blocks, stream, staged


def itests(script: str):
    """Every `itest.l *ADDR == VALUE` in the script."""
    return [
        (int(a, 16), int(v, 0)) for a, v in re.findall(r'itest\.l \*(0x[0-9a-fA-F]+) == (0x[0-9a-fA-F]+|\d+)', script)
    ]


class Bl33StageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.window, cls.blocks, cls.stream, cls.staged = window_and_stage()
        cls.original = BL33.read_bytes()
        cls.constants = {
            'STREAM_BYTES': len(cls.stream),
            'STAGE_ID': stage_id(cls.staged),
            'PACKED_BYTES': len(cls.staged),
        }
        cls.dtb = DTB.read_bytes()
        cls.script = boot_script(TEMPLATE.read_text(), cls.constants, len(cls.dtb))
        cls.loader = legacy_image(
            random.Random(5).randbytes(0x1D000), Header(os_type=5, kind=2, name='Loader', load=LOAD, entry=LOAD)
        )
        cls.slots = [
            (*SCRIPT_SLOT, script_image(cls.script)),
            (*LOADER_SLOT, cls.loader),
            (*EXPORTER_SLOT, EXPORTER.read_bytes()),
            (*DTB_SLOT, cls.dtb),
        ]
        cls.bl33 = stage_into_bl33(bl33_patch(cls.original), cls.staged, len(cls.stream), cls.slots)

    def word(self, image: bytes, address: int):
        return struct.unpack_from('<I', image, address - BL33_BASE)[0]

    def test_boot_script_memory_checks_hold_on_the_composed_file(self):
        inside = [(a, v) for a, v in itests(self.script) if BL33_BASE <= a < BL33_BASE + len(self.bl33)]
        # 6 U-Boot fingerprint words, 8 header words, 6 trailer words.
        self.assertEqual(len(inside), 20)
        for address, value in inside:
            self.assertEqual(self.word(self.bl33, address), value, hex(address))
        # A window that is not whole, or not this build's, fails a check.
        for at in (STAGE_HEADER + 0x18, STAGE_SOURCE + len(self.staged) + 0x14, STAGE_SOURCE + len(self.staged)):
            damaged = bytearray(self.bl33)
            damaged[at - BL33_BASE] ^= 1
            self.assertTrue(any(self.word(damaged, a) != v for a, v in inside), hex(at))
        truncated = self.bl33[:-4096]
        self.assertTrue(any(a + 4 > BL33_BASE + len(truncated) or self.word(truncated, a) != v for a, v in inside))

    def test_everything_the_script_reads_is_in_its_slot(self):
        at = SCRIPT_SLOT[0] - BL33_BASE
        image = script_image(self.script)
        self.assertEqual(self.bl33[at : at + len(image)], image)
        booted = re.findall(r'bootm start (0x[0-9a-f]+)', self.script)
        self.assertEqual(booted, [hex(EXPORTER_SLOT[0]), hex(LOADER_SLOT[0])])
        for address, content in ((EXPORTER_SLOT[0], EXPORTER.read_bytes()), (LOADER_SLOT[0], self.loader)):
            self.assertEqual(self.bl33[address - BL33_BASE : address - BL33_BASE + len(content)], content)
            self.assertEqual(self.word(self.bl33, address), 0x56190527)
        copy = re.search(r'cp\.b (0x[0-9a-f]+) 0x8D000000 (0x[0-9a-f]+);', self.script)
        source, size = int(copy.group(1), 16), int(copy.group(2), 16)
        self.assertEqual(self.bl33[source - BL33_BASE : source - BL33_BASE + size], self.dtb)
        self.assertIn(
            f'load mmc ${{devnum}}:${{distro_bootpart}} {BOOT_ARGS_PAGE:#x} /switchroot/ios/bootargs.txt', self.script
        )
        self.assertNotRegex(self.script, r'@[A-Z_0-9]+@|boot\.scr|loader\.uboot\.bin|exporter\.uimg|oled-sku3\.dtb')
        self.assertEqual(set(re.findall(r'/switchroot/[^ ;]+', self.script)), {'/switchroot/ios/bootargs.txt'})

    def test_a_slot_that_does_not_fit_or_overlaps_is_refused(self):
        too_big = [(*SCRIPT_SLOT[:1], 0x10, bytes(0x11))]
        overlapping = [(SCRIPT_SLOT[0], 0x20000, b'x'), (SCRIPT_SLOT[0] + 0x10000, 0x10, b'y')]
        below_u_boot = [(BL33_BASE + 0x40000, 0x10, b'z')]
        for slots in (too_big, overlapping, below_u_boot):
            with self.assertRaises(ValueError):
                stage_into_bl33(bl33_patch(self.original), self.staged, len(self.stream), slots)

    def test_u_boot_is_untouched_but_never_scans_the_emmc(self):
        image = self.bl33
        start = self.original.index(bl33.ENV_START) + 1
        end = self.original.index(b'\0\0', start) + 2
        differing = [i for i in range(len(self.original)) if image[i] != self.original[i]]
        self.assertTrue(differing and all(start <= i < end for i in differing))  # only the default environment
        environment = image[start:end].split(b'\0')
        self.assertIn(b'boot_targets=mmc1', environment)
        self.assertNotIn(b'mmc1 mmc0', image)
        self.assertFalse(any(e.startswith(b'bootcmd_mmc0=') for e in environment))
        self.assertIn(b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd', environment)
        self.assertTrue(
            any(e.startswith(b'mmc_boot=') and b'setexpr.l ios_u_sd *0x60005010;' in e for e in environment)
        )
        self.assertIn(b'distro_bootcmd=for target in ${boot_targets}', image)
        source = b'scan_dev_for_scripts=source %#x; echoe SCRIPT FAILED: continuing...' % SCRIPT_SLOT[0]
        self.assertIn(source, environment)
        self.assertFalse(any(e.startswith((b'boot_scripts=', b'boot_a_script=')) for e in environment))
        # Every other variable is as shipped, in order.
        original = self.original[start:end].split(b'\0')
        kept = [bl33.ENV_EDITS.get(e, e) for e in original if e not in bl33.ENV_DROPS]
        self.assertEqual([e for e in environment if e], [e for e in kept if e])
        _, bss_end = struct.unpack_from('<QQ', image, 0x18)
        self.assertLessEqual(BL33_BASE + bss_end, STAGE_HEADER)
        self.assertLessEqual(BL33_BASE + len(image), 0xB0000000)
        with self.assertRaises(SystemExit):
            bl33_patch(image)  # already patched: refuse rather than guess

    def test_a_loader_built_for_another_window_is_refused(self):
        built_for = {'STAGE_ID': stage_id(self.staged), 'PACKED_BYTES': len(self.staged), 'BLOCKS': [[0, 4096, 0, 64]]}
        bl33.check_loader_constants(built_for, dict(built_for))
        for other in ({'STAGE_ID': 1}, {'BLOCKS': [[4096, 4096, 0, 64]]}, {'EXTRA': 0}):
            with self.assertRaises(SystemExit) as refused:
                bl33.check_loader_constants(built_for, built_for | other)
            self.assertIn(next(iter(other)), str(refused.exception))


if __name__ == '__main__':
    unittest.main()
