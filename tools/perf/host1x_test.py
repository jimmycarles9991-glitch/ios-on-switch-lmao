"""Tests for host1x.py's reader of the TegraHost1x stats line."""

import unittest
from datetime import UTC, datetime

from tools.perf import host1x
from tools.perf.source import Rec

HEAD = 'TegraHost1x: 5s NVDEC/VIC/NVENC/NVJPG/DC gathered=0/40/0/0/60 copied=0/0/0/0/0 arena_high=0/3/0/0/1KB '


def line(msg: str, second: int) -> Rec:
    at = datetime(2026, 11, 25, 19, 24, second, tzinfo=UTC)
    fields = {'boot': 'B', 'pid': 0, 'tid': 1, 'subsystem': '', 'category': '', 'name': '', 'kind': '', 'ident': 0}
    return Rec(at, 0, proc='kernel', msg=msg, **fields)


class Host1xTest(unittest.TestCase):
    def test_vic_waiting_on_its_producer_is_a_share_of_clocked_time(self):
        recs = [
            line(HEAD + 'room_waits=0/0/0/0/0 clocked=0/5000000/0/0/5000000us stalled=0/1000000/0/0/4500000us', 31),
            line(HEAD + 'room_waits=0/0/0/0/0 clocked=0/5000000/0/0/5000000us stalled=0/3000000/0/0/4500000us', 36),
        ]
        text = '\n'.join(host1x.lines(host1x.read(recs)))
        self.assertIn(
            'VIC: 80 jobs over 2 windows (10 s); host1x clocked 10.0 s, waited on a syncpoint 4.00 s (40.0% of '
            'clocked, 50.00 ms a job), worst window 60% at 19:24:36; the rest is idle, queued or running',
            text,
        )
        self.assertIn('DC: 120 jobs', text)
        self.assertNotIn('NVDEC', text)

    def test_counters_that_never_ran_are_not_read_as_no_waiting(self):
        stopped = line(HEAD + 'room_waits=0/0/0/0/0 clocked=0/0/0/0/0us stalled=0/0/0/0/0us', 31)
        self.assertIn(
            'VIC: 40 jobs over 1 windows (5 s); counters not running', host1x.lines(host1x.read([stopped]))[0]
        )
        off = line(HEAD + 'room_waits=0/0/0/0/0 counters=off', 31)
        self.assertEqual(
            host1x.lines(host1x.read([off])),
            ['host1x channels: no tick or stall counters in this kernel (counters=off or an older kernel)'],
        )
        older = line(HEAD + 'room_waits=0/0/0/0/0', 31)
        self.assertEqual(host1x.read([older])['VIC'].counted, 0)


if __name__ == '__main__':
    unittest.main()
