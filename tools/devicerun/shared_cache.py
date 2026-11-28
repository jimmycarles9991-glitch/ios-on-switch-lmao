"""The dyld caches' read-only data stays in the shared region's nested pmap: the function-variant fill, judged per boot.

The shared region pager stores each pinned cache's function-variant slots (kernel/switch/include/switch/fvariant.h),
so dyld never remaps the cache's __DATA_CONST and __AUTH_CONST in a process. On a debug kernel, XNU logs "triggered
unnest" for a range below the region's lowest writable mapping, which is where those mappings lie. The region logs
`SwitchFvariant: cache <uuid>` once when it maps a pinned cache, and the pager logs `filled N ... in 0x<address>` for
each slot page it pages in. A filled page gives the region's slide: its address less the slot page's unslid one.

For each pinned cache whose line a boot has, that boot must also have a fill line, and no unnest may overlap the
cache's read-only data range slid by that slide. A boot that never mapped the cache (no dext started, for the DriverKit
one) or a release kernel says nothing. An `os_log_error` about an unpinned cache fails in any build.
"""

import re
from dataclasses import dataclass

from tools.devicerun.rules import FAIL, NO_DATA, PASS, REPORT, Check, Result

OWNER = 'fix-unnest'
KERNEL = 'kernel'
FVARIANT_SOURCE = 'kernel/switch/vm/fvariant.c'
UNNEST_SOURCE = 'kernel/xnu-12377.121.6/bsd/vm/vm_unix.c'
MAX_SLIDE = 0x20000000


@dataclass(frozen=True)
class Cache:
    name: str
    uuid: str  # the first four bytes, as SwitchFvariant prints them
    slot_pages: tuple[int, ...]  # unslid
    const: tuple[int, int]  # unslid read-only data range the region logs unnests in


# 24B119 (kernel/switch/include/switch/fvariant_24B119.h): the main cache's .25.dylddata __DATA_CONST and __AUTH_CONST,
# and the DriverKit cache's __DATA_CONST, __AUTH_CONST and data up to its first writable mapping.
CACHES = (
    Cache('main', 'a2dca908', (0x1ED9E8000, 0x2758D0000), (0x1E00E4000, 0x1E8F70000)),
    Cache('driverkit', '7f89cbd0', (0x183800000,), (0x181690000, 0x18377C000)),
)

SHARED_CACHE = [
    Check(
        'cache.fvariant',
        OWNER,
        'SwitchFvariant: ',
        REPORT,
        process=KERNEL,
        regex=r'SwitchFvariant: (?:cache (?P<cache>[0-9a-f]{8})(?P<rest>.*)|filled (?P<filled>\d+) function-variant '
        r'slots in 0x(?P<page>[0-9a-f]+))',
        show=('cache', 'filled'),
        source=FVARIANT_SOURCE,
    ),
    Check(
        'cache.unnest',
        OWNER,
        'triggered unnest of range',
        REPORT,
        process=KERNEL,
        regex=r'(?P<who>\S+)\[(?P<pid>\d+)\] triggered unnest of range 0x(?P<start>[0-9a-f]+)->0x(?P<end>[0-9a-f]+)',
        show=('who',),
        source=UNNEST_SOURCE,
    ),
]


def nested_check(cache: Cache) -> Check:
    lo, hi = cache.const
    return Check(
        f'cache.nested.{cache.name}',
        OWNER,
        'SwitchFvariant: ',
        'count == 0',
        source=FVARIANT_SOURCE,
        note=f'each boot that maps cache {cache.uuid}: a fill of slot page '
        f'{", ".join(hex(p) for p in cache.slot_pages)} and no unnest of {lo:#x}..{hi:#x} (+ slide)',
    )


def slide_of(cache: Cache, line: dict) -> int | None:
    """Return the slide a fill line gives for `cache`, or None when the page is none of its slot pages."""
    if not line.get('page'):
        return None
    page = int(line['page'], 16)
    for slot in cache.slot_pages:
        if 0 <= page - slot <= MAX_SLIDE and (page - slot) % 0x4000 == 0:
            return page - slot
    return None


def boot_verdict(cache: Cache, fvariant: list[dict], unnests: list[dict]) -> tuple[str, str]:
    """Return one boot's verdict and observed text for `cache`; NO_DATA when the boot never mapped it."""
    if not any(line.get('cache') == cache.uuid and 'pager fills' in (line.get('rest') or '') for line in fvariant):
        return NO_DATA, 'not mapped'
    slides = sorted({s for line in fvariant if (s := slide_of(cache, line)) is not None})
    if not slides:
        return FAIL, 'mapped, but the pager filled none of its slot pages'
    lo, hi = cache.const
    inside = [u for u in unnests if any(int(u['start'], 16) < hi + s and int(u['end'], 16) > lo + s for s in slides)]
    if inside:
        named = ', '.join(f'{u["who"]}[{u["pid"]}] {u["start"]}->{u["end"]}' for u in inside[:4])
        return FAIL, f'slide {slides[0]:#x}: {len(inside)} unnests of its read-only data: {named}'
    return PASS, f'slide {slides[0]:#x}: filled, no unnest of its read-only data'


def cache_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return one row per pinned cache, judging every boot of the pull, and one for an unpinned cache's error."""

    def lines(name: str) -> list[dict]:
        return by_check[name].matches if name in by_check else []

    fvariant, unnests = lines('cache.fvariant'), lines('cache.unnest')
    boots = sorted({line['boot'] for line in fvariant + unnests})
    rows = []
    for cache in CACHES:
        check = nested_check(cache)
        verdicts = []
        for boot in boots:
            verdict, observed = boot_verdict(
                cache, [f for f in fvariant if f['boot'] == boot], [u for u in unnests if u['boot'] == boot]
            )
            if verdict != NO_DATA:
                verdicts.append((verdict, f'boot {boot[:8]} {observed}'))
        if not verdicts:
            rows.append(Result(check, NO_DATA, 'no boot mapped it (or a release kernel)'))
            continue
        failed = [v for v in verdicts if v[0] == FAIL]
        shown = failed or verdicts
        rows.append(Result(check, FAIL if failed else PASS, '; '.join(observed for _, observed in shown)))
    unpinned = [f for f in fvariant if f.get('cache') and 'pins none' in (f.get('rest') or '')]
    unpinned_check = Check(
        'cache.unpinned',
        OWNER,
        'SwitchFvariant: ',
        'count == 0',
        source=FVARIANT_SOURCE,
        note='a cache with function-variant slots this kernel pins none for (logged in release too)',
    )
    observed = ', '.join(sorted({f'cache {f["cache"]}' for f in unpinned})) or 'none'
    rows.append(Result(unpinned_check, FAIL if unpinned else PASS, observed, unpinned))
    return rows
