"""shared_cache.py's rows over lines in the formats kernel/switch/vm/fvariant.c and XNU's log_unnest_badness log."""

import unittest

from tools.devicerun import devicerun, shared_cache
from tools.devicerun.rules import FAIL, NO_DATA, PASS

MAIN_SLIDE = 0xDC44000  # emu-tip16k's
DK_SLIDE = 0x4000000
# The lines as the emulator's kernel logged them (evidence/emu-tip16k-kernel.log)
MAIN_MAPPED = 'SwitchFvariant: cache a2dca908: the pager fills 7 function-variant slots'
DK_MAPPED = 'SwitchFvariant: cache 7f89cbd0: the pager fills 1 function-variant slots'
UNPINNED = (
    'SwitchFvariant: cache 0badcafe has function-variant slots and 24B119 pins none for it: dyld will remap its '
    'read-only data in every process'
)
# The 25 Nov 14:12 boot's line, before the fix
UNNEST = (
    '{who}[{pid}] triggered unnest of range 0x{start:x}->0x{end:x} of DYLD shared region in VM map 0x1d3f1f215683195f. '
    'While not abnormal for debuggers, this increases system memory footprint almost permanently (until the shared '
    'region is re-slid).'
)


def filled(count: int, page: int) -> str:
    return f'SwitchFvariant: filled {count} function-variant slots in 0x{page:x}'


def unnest(who: str, start: int, end: int, pid: int = 91) -> str:
    return UNNEST.format(who=who, pid=pid, start=start, end=end)


def record(message: str, second: float, boot: str = 'BOOT0001') -> dict:
    return {
        'timestamp': f'2026-11-26 00:{int(second) // 60:02d}:{second % 60:09.6f}+0000',
        'bootUUID': boot,
        'processImagePath': '/kernel.development',
        'subsystem': '',
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def rows(*messages: str | tuple[str, str]) -> dict:
    records = [record(m[0], i, m[1]) if isinstance(m, tuple) else record(m, i) for i, m in enumerate(messages, start=1)]
    results, _ = devicerun.run_checks(records, shared_cache.SHARED_CACHE)
    return {r.check.name: r for r in shared_cache.cache_rows({r.check.name: r for r in results})}


class Nested(unittest.TestCase):
    def test_a_filled_boot_without_unnests_passes_with_its_slide(self):
        got = rows(
            MAIN_MAPPED,
            filled(6, 0x1ED9E8000 + MAIN_SLIDE),
            filled(1, 0x2758D0000 + MAIN_SLIDE),
            DK_MAPPED,
            filled(1, 0x183800000 + DK_SLIDE),
        )
        self.assertEqual(got['cache.nested.main'].verdict, PASS)
        self.assertIn(f'slide {MAIN_SLIDE:#x}', got['cache.nested.main'].observed)
        self.assertEqual(got['cache.nested.driverkit'].verdict, PASS)
        self.assertIn(f'slide {DK_SLIDE:#x}', got['cache.nested.driverkit'].observed)
        self.assertEqual(got['cache.unpinned'].verdict, PASS)

    def test_an_unnest_of_the_driverkit_read_only_data_fails_named(self):
        # dyld's remap of DriverKit's __DATA_CONST, as XNU rounds it to the nesting size
        got = rows(
            DK_MAPPED,
            filled(1, 0x183800000 + DK_SLIDE),
            unnest('com.apple.DriverKit-AppleUSBHostDriver', 0x181600000 + DK_SLIDE, 0x181800000 + DK_SLIDE),
        )
        self.assertEqual(got['cache.nested.driverkit'].verdict, FAIL)
        self.assertIn('com.apple.DriverKit-AppleUSBHostDriver[91]', got['cache.nested.driverkit'].observed)
        self.assertEqual(got['cache.nested.main'].verdict, NO_DATA)

    def test_the_14_12_boots_main_cache_unnests_fail(self):
        slide = 0xBB1C000  # 0x1ebc00000 is .25's __DATA_CONST slid by it, rounded down to 2 MB
        got = rows(
            MAIN_MAPPED,
            filled(6, 0x1ED9E8000 + slide),
            unnest('xpcproxy', 0x1EBC00000, 0x1EDC00000),
            unnest('xpcproxy', 0x1EDC00000, 0x1F4C00000),
        )
        self.assertEqual(got['cache.nested.main'].verdict, FAIL)
        self.assertIn('2 unnests', got['cache.nested.main'].observed)

    def test_an_unnest_of_another_cache_or_range_is_not_counted(self):
        got = rows(
            DK_MAPPED,
            filled(1, 0x183800000 + DK_SLIDE),
            unnest('xpcproxy', 0x1EBC00000, 0x1EDC00000),
        )
        self.assertEqual(got['cache.nested.driverkit'].verdict, PASS)

    def test_a_mapped_cache_the_pager_never_filled_fails(self):
        got = rows(DK_MAPPED)
        self.assertEqual(got['cache.nested.driverkit'].verdict, FAIL)
        self.assertIn('filled none', got['cache.nested.driverkit'].observed)

    def test_a_boot_without_a_dext_says_nothing_and_another_boot_is_judged(self):
        self.assertEqual(rows(MAIN_MAPPED, filled(6, 0x1ED9E8000))['cache.nested.driverkit'].verdict, NO_DATA)
        got = rows((DK_MAPPED, 'BOOT0001'), (DK_MAPPED, 'BOOT0002'), (filled(1, 0x183800000), 'BOOT0002'))
        self.assertEqual(got['cache.nested.driverkit'].verdict, FAIL)
        self.assertIn('boot BOOT0001', got['cache.nested.driverkit'].observed)
        self.assertNotIn('BOOT0002', got['cache.nested.driverkit'].observed)

    def test_an_unpinned_cache_fails(self):
        got = rows(UNPINNED)
        self.assertEqual(got['cache.unpinned'].verdict, FAIL)
        self.assertIn('cache 0badcafe', got['cache.unpinned'].observed)


if __name__ == '__main__':
    unittest.main()
