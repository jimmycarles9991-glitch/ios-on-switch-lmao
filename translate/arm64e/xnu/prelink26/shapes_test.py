"""shapes.py against the bindings it exists for, with the instructions of 24B119's kernelcache and of our 12377 kernel.

Each case is a binding names.py once made: a kext's call reached a function of ours with another signature.
"""

import struct
import unittest

import shapes

# 24B119's kalloc.c frees and port_name_to_task_external, at their kernelcache addresses.
KFREE_TYPE_IMPL = 0xFFFFFFF00A1303D4
KFREE_TYPE_VAR_IMPL = 0xFFFFFFF00A130414
KFREE_DATA = 0xFFFFFFF00A13041C
KFREE_DATA_ADDR = 0xFFFFFFF00A130430
PORT_NAME_TO_TASK = 0xFFFFFFF00A12C90C
APPLE = {
    # the zone view check, then the heap from kt_flags into kfree_ext
    KFREE_TYPE_IMPL: [
        0xF9400008, 0xB4000048, 0x1401D6D8, 0xD0FED428, 0x911F8108, 0x2945240A, 0xF0FED70B, 0x912E816B,
        0x7211015F, 0xD0FED92C, 0x912BC18C, 0x9A880168, 0x721D015F, 0x9A8C0100, 0x92405D22, 0x17FFFED1,
    ],
    KFREE_TYPE_VAR_IMPL: [0xB2400000, 0x17FFFECF],  # orr x0, x0, #1; b kfree_ext
    KFREE_DATA: [0xAA0103E2, 0xAA0003E1, 0xD0FED920, 0x912BC000, 0x17FFFECA],
    KFREE_DATA_ADDR: [0xAA0003E1, 0xD0FED920, 0x912BC000, 0x17FFFF61],  # b kfree_addr_ext
    PORT_NAME_TO_TASK: [0x52800001, 0x52800002, 0x52800063, 0xD2800004, 0x14000001],
}
OURS = {
    0xFFFFFFF0074A8530: [  # kfree_type_impl
        0xA9BF7BFD, 0x910003FD, 0xF9400008, 0xB4000068, 0xA8C17BFD, 0x1401DD77, 0x29452009, 0x1400000E,
        0xF0FFEA6A, 0x9113014A, 0xF0FFEA6B, 0x9113816B, 0x7211013F, 0xF0FFEA6C, 0x910F018C, 0x9A8A016A,
        0x721D013F, 0x9A8C0140, 0x92405D02, 0xA8C17BFD, 0x17FFFE84, 0x372FFE69, 0x94274266, 0xD503201F,
    ],
    0xFFFFFFF0074A8590: [0xB2400000, 0x17FFFE7F],  # kfree_type_var_impl
    0xFFFFFFF0074A85A0: [0xAA0103E2, 0xAA0003E1, 0xF0FFEA60, 0x910F0000, 0x17FFFE78],  # kfree_data
    0xFFFFFFF0074A85C0: [0xF0FFEA68, 0x910F0108, 0xAA0003E1, 0xAA0803E0, 0x17FFFF34],  # kfree_data_addr
    0xFFFFFFF0074A4D40: [0x2A1F03E1, 0x2A1F03E2, 0x52800063, 0xAA1F03E4, 0x14000004],  # port_name_to_task
    0xFFFFFFF0074A4EF0: [0x52800061, 0x52800182, 0x2A1F03E3, 0xAA1F03E4, 0x17FFFF98],  # port_name_to_task_name
}
OUR_NAMES = {
    '_kfree_type_impl': 0xFFFFFFF0074A8530,
    '_kfree_type_var_impl': 0xFFFFFFF0074A8590,
    '_kfree_data': 0xFFFFFFF0074A85A0,
    '_kfree_data_addr': 0xFFFFFFF0074A85C0,
    '_port_name_to_task': 0xFFFFFFF0074A4D40,
    '_port_name_to_task_name': 0xFFFFFFF0074A4EF0,
}
# vfs_vnodecovered and vfs_vnodecovered_noblock: one instruction apart, the call to vnode_getwithref(_noblock).
VNODECOVERED = [
    0xF8438C10, 0xB40002F0, 0xAA0003F1, 0xF2EFCC11, 0xDAC11A30, 0xAA1003F1, 0xDAC147F1, 0xEB11021F, 0x54000040,
    0xD4388E40, 0xB40001D0, 0xD503237F, 0xA9BE4FF4, 0xA9017BFD, 0x910043FD, 0xAA1003E0, 0xAA1003F3, 0x97FF3716,
    0x7100001F, 0x9A9F0260, 0xA9417BFD, 0xA8C24FF4, 0xD50323FF, 0xD65F03C0, 0xD2800000, 0xD65F03C0,
]


def image(functions: dict[int, list[int]]) -> shapes.Code:
    """A Code holding each function at its address, back to back in one blob."""
    data, places = b'', {}
    for address, words in sorted(functions.items()):
        places[address] = (len(data), 4 * len(words))
        data += struct.pack('<%dI' % len(words), *words)

    def va2off(va: int) -> int | None:
        return next((off + va - a for a, (off, size) in places.items() if a <= va < a + size), None)

    ends = [a + size for a, (_, size) in places.items()]
    return shapes.Code(data, va2off, sorted(set(places) | set(ends)))


def mismatches(bound: dict[int, str]) -> list[tuple]:
    names = {address: (name, 'pinned') for address, name in bound.items()}
    return shapes.check(image(APPLE), image(OURS), list(bound), names, OUR_NAMES)


class ShapesTest(unittest.TestCase):
    def test_logical_immediates(self):
        self.assertEqual(shapes._logical_immediate(0xB2400000), 1)  # orr x0, x0, #0x1
        self.assertEqual(shapes._logical_immediate(0x121F7908), 0xFFFFFFFE)  # and w8, w8, #0xfffffffe

    def test_kfree_type_var_impl_tags_the_view(self):
        setup, _ = shapes.wrapper(APPLE[KFREE_TYPE_VAR_IMPL], KFREE_TYPE_VAR_IMPL)
        self.assertEqual(setup[0], ('orr', ('arg', 0), 1))
        self.assertEqual(setup, shapes.wrapper(OURS[0xFFFFFFF0074A8590], 0xFFFFFFF0074A8590)[0])

    def test_kfree_twins_with_one_name_are_refused(self):
        found = mismatches({
            KFREE_TYPE_IMPL: '_kfree_type_impl',
            KFREE_TYPE_VAR_IMPL: '_kfree_type_impl',
            KFREE_DATA: '_kfree_data',
            KFREE_DATA_ADDR: '_kfree_data',
        })
        self.assertEqual({int(a, 16) for a, _, _ in found}, set(APPLE) - {PORT_NAME_TO_TASK})

    def test_kfree_by_signature_passes(self):
        self.assertEqual(
            mismatches({
                KFREE_TYPE_IMPL: '_kfree_type_impl',
                KFREE_TYPE_VAR_IMPL: '_kfree_type_var_impl',
                KFREE_DATA: '_kfree_data',
                KFREE_DATA_ADDR: '_kfree_data_addr',
            }),
            [],
        )

    def test_port_name_to_task_flavor_and_group(self):
        found = mismatches({PORT_NAME_TO_TASK: '_port_name_to_task_name'})
        self.assertEqual(found, [(hex(PORT_NAME_TO_TASK), '_port_name_to_task_name', 'argument registers x1,x2,x3 differ')])
        self.assertEqual(mismatches({PORT_NAME_TO_TASK: '_port_name_to_task'}), [])

    def test_noblock_variant_is_another_function(self):
        noblock = list(VNODECOVERED)
        noblock[17] = 0x97FF485B
        at, noblock_at = 0xFFFFFFF00A321860, 0xFFFFFFF00A3218C8
        self.assertNotEqual(shapes.code_identity(VNODECOVERED, at), shapes.code_identity(noblock, noblock_at))
        moved = shapes.code_identity(noblock, noblock_at)
        self.assertEqual(moved, shapes.code_identity(noblock[:17] + [0x97FF485B - 0x1A] + noblock[18:], noblock_at + 0x68))


if __name__ == '__main__':
    unittest.main()
