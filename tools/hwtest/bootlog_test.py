"""tools/hwtest/bootlog.py over `log show` ndjson lines in boot 13's shape."""

import json
import unittest
from datetime import UTC, datetime
from pathlib import Path

from tools.hwtest import bootlog

BOOT = '6A25F6C4-A931-4851-9437-EDE961F57EC1'


def event(boot: str, timestamp: str) -> str:
    return json.dumps({'bootUUID': boot, 'timestamp': timestamp, 'eventMessage': 'Dismissing boot logo (pid:30)'})


class Dismissed(unittest.TestCase):
    def test_takes_this_boots_first_dismissal(self):
        lines = [
            'not json',
            event('11111111-0000-0000-0000-000000000000', '2026-11-08 13:00:00.000000-0800'),
            event(BOOT.lower(), '2026-11-08 14:06:37.046128-0800'),
            event(BOOT, '2026-11-08 14:09:00.000000-0800'),
            json.dumps({'count': 1, 'finished': 1}),
        ]
        when = bootlog.dismissed(lines, BOOT)
        boot_time = datetime(2026, 11, 8, 21, 58, 45, 300000, tzinfo=UTC)
        self.assertAlmostEqual((when - boot_time).total_seconds(), 471.746128)

    def test_none_without_a_dismissal(self):
        self.assertIsNone(bootlog.dismissed([json.dumps({'count': 0, 'finished': 1})], BOOT))

    def test_the_query_covers_the_boots_first_hour(self):
        argv = bootlog.command(Path('/a.logarchive'), datetime(2026, 11, 8, 21, 58, 45, tzinfo=UTC))
        start, end = argv[argv.index('--start') + 1], argv[argv.index('--end') + 1]
        self.assertEqual(
            datetime.strptime(end, '%Y-%m-%d %H:%M:%S') - datetime.strptime(start, '%Y-%m-%d %H:%M:%S'),
            datetime(2026, 1, 1, 1) - datetime(2026, 1, 1),
        )
        self.assertIn(bootlog.PREDICATE, argv)

    def test_no_archive_no_number(self):
        self.assertIsNone(bootlog.boot_seconds(Path('/nonexistent.logarchive'), BOOT, datetime.now(UTC)))


if __name__ == '__main__':
    unittest.main()
