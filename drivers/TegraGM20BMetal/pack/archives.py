"""archives: the pack's PackArchives section (runtime/pack.h PackArchive).

Per binary archive an IPSW's archive inventory lists for the identity's GPU (precompile/archives/<build>.tsv, family
device), the states it declares, each keyed as the runtime keys a creation: a render state by its key
(make_pack.state_key), a compute state by its archive key (compute_key.h computeArchiveKey). Tile, tessellation and
unspecialised rows are left out: the device makes none of them. Under kPackHeldKernels, the compute states the
pack holds kernels for at some dispatch shape (compute_key.h computeStateKey: a kernel key up to its shape).
"""

import re
import struct
from collections import Counter
from collections.abc import Collection

import packfile
from make_pack import state_key

HELD_KERNELS = 0  # pack.h kPackHeldKernels
KERNEL = re.compile(r' \| k (\S+)(?: lib=\S+)?(.*?) \|')


def compute_key(line: str) -> str:
    """Give an inventory C line's archive key (compute_key.h computeArchiveKey).

    Its kernel is the library function and its constants' text alone, never the line's spec mark of a function made with
    no constant values, whose text is empty (TegraGM20BMetalPipelineKey.h constantsText).
    """
    found = KERNEL.search(line)
    if not found:
        return ''
    fields = dict(f.split('=', 1) for f in line.split(' | ', 1)[0].split() if '=' in f)
    values = ' '.join(t for t in found[2].split() if t != 'spec')
    constants = f' {values}' if values else ''
    return f'k {found[1]}{constants} | max={fields.get("max", "0")} tgm={fields.get("tgm", "0")}'


def refused_keys(states: Collection[dict], refused: Collection[dict]) -> set[int]:
    """Give key hashes of render states refused by directpack whose default record was not made."""
    made = {packfile.fnv(rec['key'].encode()) for rec in states}
    return {packfile.fnv(rec['key'].encode()) for rec in refused} - made


def declared(inventory: str, refused: Collection[int] = ()) -> tuple[dict[str, set[int]], Counter[str]]:
    """Give each archive's declared states' key hashes, by its path on the device, and the rows taken by kind."""
    out: dict[str, set[int]] = {}
    kinds: Counter[str] = Counter()
    skipped = set(refused)
    for row in inventory.splitlines():
        kind, _owner, path, family, line = row.split('\t')
        key = state_key(line) if kind == 'render' else compute_key(line) if kind == 'compute' else ''
        if family == 'device' and key:
            key_hash = packfile.fnv(key.encode())
            if kind == 'render' and key_hash in skipped:
                continue
            out.setdefault(path, set()).add(key_hash)
            kinds[kind] += 1
    return out, kinds


def held_kernels(kernel_keys: list[str]) -> set[int]:
    """Give the key hashes of the compute states the kernels are of."""
    return {packfile.fnv(key.split(' | tg=', 1)[0].encode()) for key in kernel_keys}


def section(archives: dict[str, set[int]], held: set[int]) -> bytes:
    """Give the PackArchives section: the counts, the archives by path hash, then each one's keys ascending."""
    lists = sorted([(packfile.fnv(path.encode()), sorted(keys)) for path, keys in archives.items()])
    lists.insert(0, (HELD_KERNELS, sorted(held)))
    heads, keys = b'', []
    for path_hash, hashes in lists:
        heads += struct.pack('<QII', path_hash, len(keys), len(hashes))
        keys += hashes
    return struct.pack('<II', len(lists), len(keys)) + heads + struct.pack(f'<{len(keys)}Q', *keys)


def report(archives: dict[str, set[int]], kinds: Counter[str], held: set[int]) -> str:
    """Give the link report's line: the archives listed, the states they declare and the compute states held."""
    keys = sum(len(k) for k in archives.values())
    return (
        f'archive key lists: {len(archives)} archives, {keys} keys ({kinds["render"]} render rows, '
        f'{kinds["compute"]} compute rows); {len(held)} compute states held as kernels'
    )
