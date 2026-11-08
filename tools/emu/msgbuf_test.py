"""msgbuf.py finding XNU's msgbuf, through _msgbufp or in raw kernel memory when the kernel file cannot say."""

import struct
import unittest
from pathlib import Path
from unittest import mock

from tools.emu import msgbuf

BASE = 0xB0000000
RING = 0xFFFFFFF0DEAD0000
REGISTERS = 'CPU#0\n PC=fffffff007452aa8 X00=0\nPSTATE=800003c4 N--- NS EL1t\n'


def fields(magic: int = msgbuf.MSG_MAGIC, size: int = 0x20000, write: int = 0x100, ring: int = RING) -> bytes:
    return struct.pack('<iiiiQ', magic, size, write, 0, ring)


class FindPhysicalTest(unittest.TestCase):
    def test_the_header_at_an_aligned_offset(self):
        window = bytes(0x1000) + fields() + bytes(64)
        self.assertEqual(msgbuf.find_physical(window, BASE), BASE + 0x1000)
        self.assertEqual(msgbuf.header(fields()), (0x20000, 0x100, RING))

    def test_lookalikes_are_skipped(self):
        unaligned = bytes(4) + fields()
        not_a_ring = fields(ring=0x80000000)
        write_past_end = fields(write=0x20000)
        window = unaligned + bytes(4) + not_a_ring + write_past_end + fields()
        self.assertEqual(msgbuf.find_physical(window, BASE), BASE + len(window) - 24)

    def test_none_without_a_header(self):
        self.assertIsNone(msgbuf.find_physical(bytes(0x1000), BASE))


class FakeMonitor:
    """Memory as QEMU's pmemsave (physical) and memsave (a core's virtual addresses) see it."""

    def __init__(self, physical: dict[int, bytes], virtual: dict[int, bytes]) -> None:
        self.memory = {'pmemsave': physical, 'memsave': virtual}

    def command(self, name: str, val: int, size: int, filename: str, **_: object) -> None:
        for base, data in self.memory[name].items():
            if base <= val and val + size <= base + len(data):
                Path(filename).write_bytes(data[val - base : val - base + size])
                return
        raise RuntimeError(f'{name} {val:#x}: unmapped')


class ReadTest(unittest.TestCase):
    def setUp(self):
        window = mock.patch.object(msgbuf, 'KERNEL_PHYS', (BASE, 0x2000))
        window.start()
        self.addCleanup(window.stop)
        ring = b'second' + b'first '
        self.monitor = FakeMonitor({BASE: bytes(0x1000) + fields(size=len(ring), write=6) + bytes(0xFE8)}, {RING: ring})

    def test_no_kernel_file_scans_the_window(self):
        ring = msgbuf.read(self.monitor, None, REGISTERS)
        self.assertEqual(ring, msgbuf.Ring(f'physical {BASE + 0x1000:#x}', b'first second'))
        self.assertEqual(ring.describe(), f'msgbuf at physical {BASE + 0x1000:#x}: 12 of 12 bytes written')

    def test_a_symbol_from_another_kernel_falls_back_to_the_scan(self):
        with mock.patch.object(msgbuf, 'symbol', return_value=0xFFFFFFF007B00000):
            ring = msgbuf.read(self.monitor, Path('kernel.macho'), REGISTERS)
        self.assertEqual(ring.data, b'first second')

    def test_nothing_without_a_core_in_the_kernel(self):
        self.assertIsNone(msgbuf.read(self.monitor, None, REGISTERS.replace('EL1t', 'EL0t')))


if __name__ == '__main__':
    unittest.main()
