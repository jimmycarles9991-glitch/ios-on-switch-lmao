"""What of a packed seed directory (kits/SwitchSeedPack/SwitchSeedPack.h) goes to the device.

Its index and the packs of entries a device was seen to use, [0, observed packs). The reader takes an absent pack's
entries as misses; the derived entries' packs had no hit on a device, so they stay on the host.
"""

import functools
import struct
from pathlib import Path

MAGIC = b'SWSEEDPK'
VERSION = 1
HEADER = struct.Struct('<8sIIIIII')


@functools.cache
def observed_packs(index: Path) -> int:
    """Return the number of packs that hold only observed entries, from the index's header."""
    with index.open('rb') as file:
        header = file.read(HEADER.size)
    if len(header) < HEADER.size:
        raise ValueError(f'{index}: shorter than a seed pack header')
    magic, version, _, _, packs, observed, _ = HEADER.unpack(header)
    if magic != MAGIC or version != VERSION or observed > packs:
        raise ValueError(f'{index}: not a version {VERSION} seed pack index')
    return observed


def staged(directory: Path, relative: str) -> bool:
    """Return whether the file at `relative` below the packed `directory` goes to the device."""
    if relative == 'index':
        return True
    parts = relative.split('/')
    return (
        len(parts) == 2
        and parts[0] == 'packs'
        and parts[1].isdigit()
        and int(parts[1]) < observed_packs(directory / 'index')
    )
