import hashlib
import importlib.util
import struct
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('rom_capture', Path(__file__).with_name('rom_capture.py'))
rom_capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(rom_capture)


def capture_log(data: bytes) -> str:
    digest = hashlib.sha256(data).hexdigest()
    chunks = [
        f'SwitchWLAN: ROM chunk: {offset:08x} {data[offset : offset + 64].hex()}' for offset in range(0, len(data), 64)
    ]
    return '\n'.join(
        [
            f'SwitchWLAN: ROM capture begin: {len(data)} bytes, overwritten prefix 4, sha256 {digest}',
            *chunks,
            f'SwitchWLAN: ROM capture end: {digest}',
        ]
    )


class ImageTest(unittest.TestCase):
    def setUp(self):
        self.image = (rom_capture.FIRMWARE / 'brcm/brcmfmac4356A3-pcie.bin').read_bytes()

    def test_fingerprint_and_only_the_two_patch_regions_change(self):
        patched = rom_capture.build_image(self.image, 0xA0000, 0xC0000)
        code = rom_capture.header_bytes('switch_wlan_rom_start_code')
        self.assertEqual(len(patched), len(self.image))
        self.assertEqual(patched[:0x80], self.image[:0x80])
        self.assertEqual(patched[0x80 : 0x80 + len(code)], code)
        self.assertEqual(patched[0x80 + len(code) : 0x100], self.image[0x80 + len(code) : 0x100])
        self.assertEqual(struct.unpack_from('<6I', patched, 0x100), (rom_capture.MAGIC, 0, 0xA0000, 0xC0000, 0, 0))
        self.assertEqual(patched[0x118:], self.image[0x118:])
        self.assertEqual(rom_capture.header_bytes('switch_wlan_rom_image_sha256').hex(), rom_capture.IMAGE_SHA256)

    def test_rejects_a_different_image(self):
        changed = bytearray(self.image)
        changed[-1] ^= 1
        for data in (self.image[:-1], changed):
            with self.assertRaisesRegex(ValueError, 'fingerprint'):
                rom_capture.build_image(data, 0xA0000, 0xC0000)

    def test_rejects_empty_unaligned_and_out_of_bounds_sizes(self):
        for rom, ram in ((0, 0xC0000), (3, 0xC0000), (0xC0000, 0xC0000), (4, 0x1000), (4, 0x100000000)):
            with self.assertRaisesRegex(ValueError, 'measured RAM range'):
                rom_capture.build_image(self.image, rom, ram)


class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.data = bytes(range(256)) + b'last'
        self.log = capture_log(self.data)

    def test_complete_capture_preserves_the_overwritten_prefix_metadata(self):
        result, metadata = rom_capture.extract(self.log)
        self.assertEqual(result, self.data)
        self.assertEqual(metadata['overwritten_prefix_bytes'], 4)
        self.assertEqual(metadata['sha256'], hashlib.sha256(result).hexdigest())

    def test_duplicate_identical_chunks_are_accepted(self):
        rows = self.log.splitlines()
        rows.insert(2, rows[1])
        self.assertEqual(rom_capture.extract('\n'.join(rows))[0], self.data)

    def test_missing_end_chunk_conflict_and_wrong_hash_are_rejected(self):
        rows = self.log.splitlines()
        cases = [
            '\n'.join(rows[:-1]),
            '\n'.join(rows[:2] + rows[3:]),
            self.log.replace(rows[1], rows[1] + '\nSwitchWLAN: ROM chunk: 00000000 ff'),
            self.log.replace(rows[1][-2:], 'ff', 1),
        ]
        for text in cases:
            with self.assertRaises(ValueError):
                rom_capture.extract(text)

    def test_odd_hex_and_out_of_range_chunks_are_rejected(self):
        for chunk in ('00000000 f', '00001000 ff', '00000000 ' + 'ff' * 65):
            text = self.log.replace(self.log.splitlines()[1], f'SwitchWLAN: ROM chunk: {chunk}')
            with self.assertRaises(ValueError):
                rom_capture.extract(text)


if __name__ == '__main__':
    unittest.main()
