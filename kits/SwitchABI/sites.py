"""A kind's declared instruction sites in Apple's kexts (kinds/<kind>/sites/<bundle>.json), checked and applied.

A table names one kext of one IPSW kind and its sites: the address, the word Apple's kexts have there, the word the
prelink writes instead and why. Every original word is checked before anything is written, so a kernelcache other
than the one the table was read from fails the build at the first site that differs, by name. A table for another
kind is refused.
"""

import json
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Site:
    bundle: str
    at: int
    original: int
    replacement: int
    why: str


def load(path: str | Path, kind: str) -> list[Site]:
    table = json.loads(Path(path).read_text())
    if table['kind'] != kind:
        raise SystemExit(f'{path}: sites for {table["kind"]}, not {kind}')
    return [
        Site(table['bundle'], int(s['at'], 16), int(s['from'], 16), int(s['to'], 16), s['why']) for s in table['sites']
    ]


def load_all(paths: Iterable[str | Path], kind: str) -> list[Site]:
    sites = [site for path in paths for site in load(path, kind)]
    seen = set()
    for site in sites:
        if site.at in seen:
            raise SystemExit(f'{site.at:#x}: declared twice')
        seen.add(site.at)
    return sites


def apply(sites: list[Site], read: Callable[[int], int], write: Callable[[int, int], None]) -> None:
    """Check every site's original word, then write every replacement."""
    for site in sites:
        found = read(site.at)
        if found != site.original:
            raise SystemExit(
                f'{site.bundle} {site.at:#x}: {found:08x} where the table has {site.original:08x} ({site.why})'
            )
    for site in sites:
        write(site.at, site.replacement)


def record(sites: list[Site], slide: int) -> list[dict[str, str]]:
    """Describe the sites written, for the prelink report: Apple's address and the prelinked kernel's."""
    return [
        {
            'bundle': site.bundle,
            'at': f'{site.at:#x}',
            'va': f'{(site.at + slide) & ((1 << 64) - 1):#x}',
            'from': f'{site.original:08x}',
            'to': f'{site.replacement:08x}',
        }
        for site in sites
    ]
