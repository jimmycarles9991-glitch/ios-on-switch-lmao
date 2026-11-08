#!/usr/bin/env python3
"""Make an IPSW's iPad wallpapers' Wallpaper.plist load at every UI scale, from its own OS volume.

WallpaperKit loads a wallpaper bundle only when the logicalScreenClass in its Wallpaper.plist is the device's or
WKWallpaperRepresentingLogicalScreenClassAny, 'any' (+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]).
WKLogicalScreenClassString builds the device's from MobileGestalt's standard display zoom size (ADT /product
artwork-device-subtype 2732 and artwork-scale-factor 2: 1024x1366) and UIScreen.scale as an NSNumber, so it follows
switch-ui-scale ('1024w-1366h@1x~ipad', '1024w-1366h@1.6x~ipad': kernel/xnu-10063.141.1/bsd/vm/vm_unix.c,
SwitchUIScale), while an IPSW ships its iPad bundles at @2x only. Without these the iPadOS 17 and Lake collections load
empty, the iPad's default wallpaper (7570) is "not found on disk", and WKDefaultWallpaperManager falls back to its
synthesized 'Solid Black' (400).

Each IPSW kind has the bundles of its own iPad (KINDS): those at that iPad's screen class whose productTypes list it
(or have none) and whose unsupportedProductTypes do not. 21H461's for iPad7,1 (J120) are the _Flat ones (one 2x HEIC
drawn by a .ca); its animated ones list J120 in unsupportedProductTypes. 24B119 (an iPad12,1 IPSW, J181AP) ships only
the 10.2-inch iPad's 810w-1080h bundles, with no product lists. This copies each one's Wallpaper.plist with
logicalScreenClass set to 'any' and nothing else changed. The overlay (overlay/manifest.json, per kind) shows it over
the sealed bundle's own plist, with the bundle directory as its own union, so its .ca and images are the sealed
volume's: no artwork is copied, and the 2x artwork is drawn into the 720x1280 panel (scaled down, never up).

The output is metadata only (identifiers, names, file names) and is pinned in bazel/pinned/overlay/wallpaper-1x/<kind>/.

Usage: wallpaper_1x.py <kind> <OS volume .dmg or its mounted root> <output directory>
       (the kind's OS member: @ipsw//:os, ipsw/pins.bzl)
"""

import contextlib
import plistlib
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path
from typing import NamedTuple

COLLECTIONS = 'Library/Wallpaper/Collections'
TO_CLASS = 'any'


class Kind(NamedTuple):
    product: str
    screen_class: str
    bundles: tuple[tuple[str, str], ...]  # (collection, bundle), each pinned as <bundle without its class>.plist


KINDS = {
    '21H461': Kind(
        'iPad7,1',
        '1024w-1366h@2x~ipad',
        (
            ('iPadOS_17~ipad.wallpaperCollection', '7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper'),
            ('Lake~ipad.wallpaperCollection', '7560.Lake_Flat-1024w-1366h@2x~ipad.wallpaper'),
        ),
    ),
    '24B119': Kind(
        'iPad12,1',
        '810w-1080h@2x~ipad',
        (
            ('iPadOS_17~ipad.wallpaperCollection', '7570.iPadOS_17-810w-1080h@2x~ipad.wallpaper'),
            ('Lake~ipad.wallpaperCollection', '7560.Lake-810w-1080h@2x~ipad.wallpaper'),
        ),
    ),
}


def pinned_name(kind: Kind, bundle: str) -> str:
    """'7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper' -> '7570.iPadOS_17_Flat.plist'."""
    return bundle.removesuffix('.wallpaper').removesuffix('-' + kind.screen_class) + '.plist'


def installs_on(plist: dict, product: str) -> bool:
    """Whether WallpaperKit's product lists let `product` load the bundle."""
    listed = plist.get('productTypes')
    return (listed is None or product in listed) and product not in plist.get('unsupportedProductTypes', ())


def one_x(data: bytes, name: str, kind: Kind) -> bytes:
    """Return the plist `data` at logicalScreenClass 'any'; refuse anything but a bundle of the kind's own iPad."""
    plist = plistlib.loads(data)
    if plist.get('logicalScreenClass') != kind.screen_class:
        raise SystemExit(f'{name}: logicalScreenClass {plist.get("logicalScreenClass")!r}, not {kind.screen_class!r}')
    if not installs_on(plist, kind.product):
        raise SystemExit(f'{name}: not a {kind.product} bundle')
    plist['logicalScreenClass'] = TO_CLASS
    return plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)


@contextlib.contextmanager
def os_root(volume: Path) -> Iterator[Path]:
    """Yield the OS volume's root: `volume` itself when it is a mounted root, else the .dmg attached read-only."""
    if volume.is_dir():
        yield volume
        return
    with tempfile.TemporaryDirectory() as mount:
        subprocess.run(
            ['hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-mountpoint', mount, str(volume)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        try:
            yield Path(mount)
        finally:
            subprocess.run(['hdiutil', 'detach', mount], check=True, stdout=subprocess.DEVNULL)


def main() -> None:
    if len(sys.argv) != 4 or sys.argv[1] not in KINDS:
        raise SystemExit(__doc__)
    kind, volume, out = KINDS[sys.argv[1]], Path(sys.argv[2]), Path(sys.argv[3])
    with os_root(volume) as root:
        sources = {
            bundle: (root / COLLECTIONS / collection / 'Wallpapers' / bundle / 'Wallpaper.plist').read_bytes()
            for collection, bundle in kind.bundles
        }
    out.mkdir(parents=True, exist_ok=True)
    for collection, bundle in kind.bundles:
        name = pinned_name(kind, bundle)
        (out / name).write_bytes(one_x(sources[bundle], name, kind))
        print(f'{out / name}: /{COLLECTIONS}/{collection}/Wallpapers/{bundle}/Wallpaper.plist at {TO_CLASS}')


if __name__ == '__main__':
    main()
