"""What the crawl takes from hand-made files: fat CPU binaries and fat libraries of several AIR slices."""

import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.gpu.shaders.crawl import Crawl
from tools.gpu.shaders.metallib import FRAGMENT, KERNEL
from tools.gpu.shaders.metallib_test import _library

AIR64, ARM64 = 0x01000017, 0x0100000C


def _fat(slices: list[tuple[int, int, bytes]]) -> bytes:
    """Make a fat file of (cputype, cpusubtype, bytes) slices, each at a 16-byte boundary."""
    offset, table, body = 8 + 20 * len(slices), b'', b''
    for cputype, subtype, data in slices:
        pad = -(offset + len(body)) % 16
        body += bytes(pad)
        table += struct.pack('>5I', cputype, subtype, offset + len(body), len(data), 4)
        body += data
    return b'\xca\xfe\xba\xbe' + struct.pack('>I', len(slices)) + table + body


class CrawlTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.crawl = Crawl(self.dir / 'out')

    def take(self, name: str, data: bytes) -> dict:
        path = self.dir / name
        path.write_bytes(data)
        self.crawl.file(path, name)
        return json.loads(json.dumps(self.crawl.inventory()))

    def test_a_library_inside_a_fat_cpu_binary_is_found(self):
        library = _library([('embedded', KERNEL, [])])
        code = b'\xcf\xfa\xed\xfe' + bytes(60) + library + bytes(64)
        inventory = self.take('tool', _fat([(ARM64, 0, code), (ARM64, 2, bytes(256))]))
        self.assertEqual(inventory['files'], 0)
        self.assertEqual(inventory['embedded_seeded'], 1)
        self.assertEqual(inventory['embedded_libraries'][0]['types'], {str(KERNEL): 1})

    def test_every_air_slice_of_a_fat_library_is_listed(self):
        old = _library([('shared', FRAGMENT, [])])
        new = _library([('shared', FRAGMENT, []), ('only_new', KERNEL, [])])
        inventory = self.take('lib.metallib', _fat([(AIR64, 8, old), (AIR64, 13, new)]))
        entry = inventory['libraries'][0]
        self.assertEqual(entry['types'], {str(FRAGMENT): 2, str(KERNEL): 1})
        self.assertEqual(entry['slices'], [{'cpusubtype': 8, 'functions': 1}, {'cpusubtype': 13, 'functions': 2}])
        self.assertTrue((self.dir / 'out/root/lib.metallib').exists())

    def test_a_thin_library_has_no_slice_list(self):
        inventory = self.take('one.metallib', _library([('k', KERNEL, [])]))
        self.assertNotIn('slices', inventory['libraries'][0])


if __name__ == '__main__':
    unittest.main()
