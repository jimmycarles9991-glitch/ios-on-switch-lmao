"""cachecheck.py's rules on synthetic logs: what a controller write and read each need from the guest."""

import tempfile
import unittest
from pathlib import Path

from tools.emu import cachecheck
from tools.emu.cachecheck import RECORD

OPS = {name: code for code, name in cachecheck.OPS.items()}
PAGE = 0x86E14000
CACHED, UNCACHED = 0xFF, 0x44


class Log:
    def __init__(self) -> None:
        self.raw, self.seq = b'', 0

    def put(self, kind: str, *, op: int = 0, attrs: int = 0, length: int = 64, va: int = PAGE) -> 'Log':
        self.raw += RECORD.pack(ord(kind), op, 0, attrs, length, self.seq, 0, 0, PAGE, va)
        self.seq += 1
        return self

    def dma(self, kind: str, length: int = 64) -> 'Log':
        return self.put(kind, length=length)

    def dc(self, name: str) -> 'Log':
        return self.put('S' if name.endswith('SW') else 'D', op=OPS[name], attrs=CACHED, va=0xFFFFFFF012340000)

    def cpu(self, attrs: int = CACHED) -> 'Log':
        return self.put('M', attrs=attrs, length=0x1000, va=0xFFFFFFF012340000)

    def rules(self) -> list[str]:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'cache.bin'
            path.write_bytes(self.raw)
            return [v.rule for v in cachecheck.judge(cachecheck.records(path))]


WRITE = 'the CPU touched it through a cacheable mapping with no invalidate after the controller wrote it'
READ = 'read with no clean after the CPU touched it through a cacheable mapping'


class Rules(unittest.TestCase):
    def test_an_invalidate_before_the_cpu_touches_the_data_passes(self):
        self.assertEqual(Log().dma('W').dc('IVAC').cpu().dma('W').dc('CIVAC').cpu().rules(), [])

    def test_a_cacheable_touch_with_no_invalidate_fails(self):
        self.assertEqual(Log().dma('W').cpu().rules(), [WRITE])

    def test_an_invalidate_after_the_touch_or_before_the_write_does_not_count(self):
        self.assertEqual(Log().dma('W').cpu().dc('IVAC').rules(), [WRITE])
        self.assertEqual(Log().dc('IVAC').dma('W').cpu().rules(), [WRITE])

    def test_a_non_cacheable_touch_or_none_needs_nothing(self):
        self.assertEqual(Log().dma('W').cpu(UNCACHED).rules(), [])
        self.assertEqual(Log().dma('W').dma('W').rules(), [])

    def test_a_clean_only_after_a_write_fails(self):
        rules = Log().dma('W').dc('CVAC').dc('CIVAC').cpu().rules()
        self.assertEqual(rules, ['clean-only of a line the controller wrote, before any invalidate'])

    def test_a_read_needs_a_clean_after_the_cpu_last_touched_the_page(self):
        self.assertEqual(Log().dma('R').cpu().dc('CVAC').dma('R').rules(), [])
        self.assertEqual(Log().dma('R').dc('CVAC').cpu().dma('R').rules(), [READ])
        self.assertEqual(Log().cpu().dma('R').rules(), [], "a page's first transfer is not judged for a read")

    def test_set_way_maintenance_counts_for_every_line(self):
        self.assertEqual(Log().dma('W').dc('CISW').cpu().rules(), [])


if __name__ == '__main__':
    unittest.main()
