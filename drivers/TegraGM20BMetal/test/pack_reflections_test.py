#!/usr/bin/env python3
"""The pack's PackReflections section (runtime/pack.h PackReflection) from directpack.mm's DRFL records.

A render state's reflection is filed under its key's hash, a compute state's under its computeStateKey's (a kernel key's
part before its dispatch shape): the hashes Pack::reflection looks up. Each distinct blob is held once.
"""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import directlink
import packfile

RENDER_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f'
KERNEL_KEY = 'kernel k AIR 0 0 | tg=64,1,1 | tgmem='


def drfl(compute: bool, key: str, blob: bytes) -> bytes:
    return (
        b'DRFL'
        + struct.pack('<I', compute)
        + struct.pack('<I', len(key))
        + key.encode()
        + struct.pack('<I', len(blob))
        + blob
    )


def entries(section: bytes) -> tuple[list[tuple[int, int, int]], list[tuple[int, int, int]]]:
    render, compute = struct.unpack_from('<II', section)
    rows = [struct.unpack_from('<QII', section, 8 + 16 * i) for i in range(render + compute)]
    return rows[:render], rows[render:]


class SectionTest(unittest.TestCase):
    def test_each_kind_ascends_by_hash_and_a_shared_blob_is_held_once(self) -> None:
        shared, other = b'SWRF' + bytes(21), b'SWRF' + bytes(40)
        section = packfile.reflection_bytes(
            {(False, 9): shared, (False, 3): shared, (True, 5): other, (True, 1): shared}
        )
        render, compute = entries(section)
        self.assertEqual([h for h, _, _ in render], [3, 9])
        self.assertEqual([h for h, _, _ in compute], [1, 5])
        offsets = {at for _, at, _ in render + compute}
        self.assertEqual(len(offsets), 2)
        for h, at, size in render + compute:
            self.assertEqual(at % 8, 0)
            self.assertEqual(section[at : at + size], other if (h == 5) else shared)

    def test_a_pack_carries_it_as_its_last_section(self) -> None:
        self.assertEqual(packfile.SECTIONS, 16)
        self.assertEqual(packfile.VERSION, 33)
        pack = packfile.Pack([], [], [], {}, [], bytearray(), [], reflections={(False, 7): b'SWRF'})
        data, _ = packfile.pack_bytes(pack)
        offset, size = struct.unpack_from('<QQ', data, 24 + 16 * 14)
        self.assertEqual(entries(data[offset : offset + size]), ([(7, 24, 4)], []))


class RecordsTest(unittest.TestCase):
    def test_drfl_records_file_a_reflection_under_the_hash_the_runtime_looks_up(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'records.bin')
            with open(path, 'wb') as f:
                f.write(drfl(False, RENDER_KEY, b'render') + drfl(True, KERNEL_KEY, b'compute'))
                f.write(drfl(True, KERNEL_KEY.replace('64,1,1', '32,1,1'), b'again'))
            states, kernels, reflections = directlink.load_records(path)
        self.assertEqual((states, kernels), ([], []))
        state = KERNEL_KEY.split(' | tg=', 1)[0]
        self.assertEqual(
            reflections,
            {(False, packfile.fnv(RENDER_KEY.encode())): b'render', (True, packfile.fnv(state.encode())): b'compute'},
        )


if __name__ == '__main__':
    unittest.main()
