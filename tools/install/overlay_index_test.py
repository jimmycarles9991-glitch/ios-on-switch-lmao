"""The kernel's index of iOS 27's translated System files: the byte layout vfs_switch_pretranslated.c reads."""

import struct
import unittest

from tools.install import overlay_index

CD = bytes(range(20))
FILES = [
    ('/usr/libexec/b', 30, 300, bytes([3] * 20)),
    ('/System/Library/a', 10, 100, CD),
    ('/usr/libexec/a', 20, 200, bytes([2] * 20)),
]


class OriginalsTest(unittest.TestCase):
    def test_layout_sorted_by_original(self):
        raw = overlay_index.encode_originals([(bytes([9] * 20), bytes([1] * 20)), (bytes([4] * 20), bytes([2] * 20))])
        self.assertEqual(overlay_index.HEADER_ORIGINALS.unpack_from(raw), (b'SWOR', 1, 2, 0))
        body = raw[overlay_index.HEADER_ORIGINALS.size :]
        self.assertEqual(body, bytes([4] * 20) + bytes([2] * 20) + bytes([9] * 20) + bytes([1] * 20))

    def test_a_pair_given_twice_is_written_once(self):
        pair = (bytes([4] * 20), bytes([2] * 20))
        self.assertEqual(overlay_index.encode_originals([pair, pair]), overlay_index.encode_originals([pair]))

    def test_one_original_translated_two_ways_is_refused(self):
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.encode_originals([(bytes(20), bytes([1] * 20)), (bytes(20), bytes([2] * 20))])

    def test_short_cdhashes_are_refused(self):
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.encode_originals([(bytes(19), bytes(20))])


class OverlayIndexTest(unittest.TestCase):
    def test_layout(self):
        raw = overlay_index.encode(FILES, [7, 5])
        magic, version, count, dirs, _size, reserved = overlay_index.HEADER.unpack_from(raw)
        self.assertEqual((magic, version, count, dirs, reserved), (b'SWOV', 2, 3, 2, 0))
        records = [
            overlay_index.FILE.unpack_from(raw, overlay_index.HEADER.size + overlay_index.FILE.size * i)
            for i in range(3)
        ]
        self.assertEqual(
            [(s, d, o, p) for s, d, o, p, _ in records], [(10, 100, 0, 0), (20, 200, 18, 0), (30, 300, 33, 0)]
        )
        self.assertEqual(records[0][4], CD)
        at = overlay_index.HEADER.size + overlay_index.FILE.size * 3
        self.assertEqual(struct.unpack_from('<2Q', raw, at), (5, 7))
        self.assertEqual(raw[at + 16 :], b'/System/Library/a\0/usr/libexec/a\0/usr/libexec/b\0')

    def test_round_trip(self):
        files = [('/a', 1, 11, bytes([1] * 20)), ('/b/c', 2, 12, bytes([2] * 20)), ('/z', 4, 14, bytes([4] * 20))]
        self.assertEqual(overlay_index.decode(overlay_index.encode(files, [20, 21])), (files, [20, 21]))

    def test_refuses_paths_the_kernel_would_not_take(self):
        for bad in ['relative', '/trailing/', '/a//b', '/a/./b', '/a/../b', '/' + 'x' * 1024, '/.index']:
            with self.subTest(bad), self.assertRaises(overlay_index.OverlayIndexError):
                overlay_index.encode([(bad, 1, 2, CD)], [3])
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.encode([('/a', 1, 2, CD), ('/a', 3, 4, CD)], [5])

    def test_refuses_a_cdhash_that_is_not_20_bytes(self):
        for bad in [b'', b'\0' * 19, b'\0' * 21, '0' * 40]:
            with self.subTest(bad), self.assertRaises(overlay_index.OverlayIndexError):
                overlay_index.encode([('/a', 1, 2, bad)], [5])

    def test_refuses_ids_the_kernel_would_not_take(self):
        for files, dirs in [
            ([('/a', 0, 2, CD)], [3]),
            ([('/a', 1, 2, CD)], [0]),
            ([('/a', 1, 2, CD), ('/b', 1, 4, CD)], [5]),
            ([('/a', 1, 2, CD), ('/b', 1, 2, bytes(20))], [5]),
            ([('/a', 1, 2, CD), ('/b', 3, 2, CD)], [5]),
            ([('/a', 1, 2, CD)], [2]),
            ([('/a', 1, 2, CD)], [5, 5]),
        ]:
            with self.subTest(files=files, dirs=dirs), self.assertRaises(overlay_index.OverlayIndexError):
                overlay_index.encode(files, dirs)

    def test_hard_links_share_one_data_file(self):
        files = [('/usr/libexec/d', 1, 2, CD), ('/usr/sbin/d', 1, 2, CD), ('/usr/sbin/e', 3, 4, CD)]
        self.assertEqual(overlay_index.decode(overlay_index.encode(files, [5])), (files, [5]))

    def test_decode_refuses_unsorted_or_loose_tables(self):
        raw = bytearray(overlay_index.encode([('/a', 1, 2, CD), ('/b', 3, 4, CD)], [5]))
        off = overlay_index.HEADER.size + overlay_index.FILE.size + 16
        raw[off : off + 4] = struct.pack('<I', 0)
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.decode(bytes(raw))
        swapped = (
            struct.pack('<4sIIIII', b'SWOV', 2, 2, 1, 6, 0)
            + overlay_index.FILE.pack(1, 2, 0, 0, CD)
            + overlay_index.FILE.pack(3, 4, 3, 0, CD)
            + struct.pack('<Q', 5)
            + b'/b\0/a\0'
        )
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.decode(swapped)
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.decode(overlay_index.encode([('/a', 1, 2, CD)], [5])[:-1])


if __name__ == '__main__':
    unittest.main()
