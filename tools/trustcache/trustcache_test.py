"""tools/trustcache: the module's layout, order, deduplication and refusals."""

import hashlib
import struct
import unittest

from tools.trustcache.trustcache import cdhash_list, merged_module, module_entries, trust_module


def h(n: int) -> bytes:
    """Return a CDHash of `n` repeated."""
    return bytes([n]) * 20


class TrustModuleTest(unittest.TestCase):
    def test_layout_sorted_with_default_uuid(self):
        module = trust_module([h(3), h(1), h(2)])
        version, uuid, count = struct.unpack_from('<I16sI', module)
        body = module[24:]
        self.assertEqual((version, count, len(body)), (2, 3, 3 * 24))
        self.assertEqual([body[i * 24 : i * 24 + 20] for i in range(3)], [h(1), h(2), h(3)])
        self.assertEqual(body[20:24], bytes([2, 0, 0, 0]))
        self.assertEqual(uuid, hashlib.sha256(body).digest()[:16])

    def test_repeated_cdhash_is_one_entry(self):
        self.assertEqual(trust_module([h(1), h(2), h(1)]), trust_module([h(2), h(1)]))
        self.assertEqual(trust_module([(h(1), 1, 3), (h(1), 1, 3)]), trust_module([(h(1), 1, 3)]))

    def test_flags_and_category(self):
        body = trust_module([(h(9), 1, 7)])[24:]
        self.assertEqual(body, h(9) + bytes([2, 1, 7, 0]))

    def test_conflicting_duplicates_refused(self):
        with self.assertRaises(ValueError):
            trust_module([(h(1), 0, 0), (h(1), 0, 2)])

    def test_explicit_uuid_and_bad_input(self):
        self.assertEqual(trust_module([h(1)], uuid=b'U' * 16)[4:20], b'U' * 16)
        with self.assertRaises(ValueError):
            trust_module([b'short'])
        with self.assertRaises(ValueError):
            trust_module([h(1)], uuid=b'x')


def raw(entries: list[tuple[bytes, int, int, int]], uuid: bytes = b'u' * 16) -> bytes:
    """Return a module written field by field, hash types included, as Apple's are."""
    body = b''.join(cdhash + bytes([kind, flags, category, 0]) for cdhash, kind, flags, category in entries)
    return struct.pack('<I16sI', 2, uuid, len(entries)) + body


class MergeTest(unittest.TestCase):
    def test_entries_are_read_back_whole(self):
        module = raw([(h(1), 1, 0, 0), (h(2), 2, 1, 4)])
        self.assertEqual(module_entries(module), [h(1) + bytes([1, 0, 0, 0]), h(2) + bytes([2, 1, 4, 0])])

    def test_malformed_modules_are_refused(self):
        good = raw([(h(1), 2, 0, 0), (h(2), 2, 0, 0)])
        for module in (
            good[:23],
            good[:-1],
            struct.pack('<I', 1) + good[4:],
            raw([(h(2), 2, 0, 0), (h(1), 2, 0, 0)]),
            raw([(h(1), 2, 0, 0), (h(1), 2, 0, 0)]),
        ):
            with self.subTest(length=len(module)), self.assertRaises(ValueError):
                module_entries(module)

    def test_merge_keeps_the_first_uuid_every_entry_and_its_hash_type(self):
        static = raw([(h(5), 1, 0, 0), (h(9), 2, 0, 0)], uuid=b'S' * 16)
        cryptex = raw([(h(2), 2, 0, 1), (h(9), 2, 0, 0)], uuid=b'C' * 16)
        merged = merged_module([static, cryptex])
        self.assertEqual(merged[4:20], b'S' * 16)
        self.assertEqual(
            module_entries(merged),
            [h(2) + bytes([2, 0, 1, 0]), h(5) + bytes([1, 0, 0, 0]), h(9) + bytes([2, 0, 0, 0])],
        )

    def test_merge_refuses_one_cdhash_with_two_entries_and_no_modules(self):
        with self.assertRaises(ValueError):
            merged_module([raw([(h(1), 2, 0, 0)]), raw([(h(1), 2, 0, 3)])])
        with self.assertRaises(ValueError):
            merged_module([])

    def test_cdhash_list_is_ascending_without_repeats(self):
        listed = cdhash_list([raw([(h(7), 2, 0, 0), (h(9), 2, 0, 0)]), raw([(h(1), 2, 0, 0), (h(7), 2, 0, 0)])])
        self.assertEqual(listed, h(1) + h(7) + h(9))
        self.assertEqual(cdhash_list([]), b'')


if __name__ == '__main__':
    unittest.main()
