#!/usr/bin/env python3
"""Every pack program that reads the pixel interlock's ticket waits for it with a load from L2 and says it reads it.

usage: ticketcheck.py <pack.bin>

A program reading SR_ORDERING_TICKET (S2R 0x0f) must wait with LDG .CG and no atomic: an atomic add of 0 queues on
L2's atomic unit beside every release (6.6% slower at 720p on T-v4c), and NAK's earlier uncached load (.CV) spun 10-25x
slower (T-v3i). The section's own loads are not the wait and may take any cache op. Its header must set
READS_INTERLOCK_TICKET (SPH bit 610), without which the device hung 30 s (T-v3i). Prints one line per program that
breaks either and a total; exits 1 on any.
"""

import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import isa

SR_ORDERING_TICKET = 0x0F
LDG_CACHE_CG = 1
SPH_READS_INTERLOCK_TICKET = 610
HEADER_AT = 0x30  # packfile.py program(): the header at +0x30 of a program's heap slot, its code at +0x80
CODE_AT = HEADER_AT + isa.HEADER_BYTES
PROGRAM_SECTION, HEAP_SECTION = 0, 7
S2R_MASK, S2R_VALUE = 0xFFF8 << 48 | 0xFF << 20, 0xF0C8 << 48 | SR_ORDERING_TICKET << 20


def reads_ticket(words: list[int]) -> bool:
    return any(w & S2R_MASK == S2R_VALUE for w in words)


def faults(header: bytes, code: bytes) -> list[str]:
    """Give what a ticket-reading program gets wrong; nothing for one that reads no ticket."""
    words = isa.instructions(code)
    if not reads_ticket(words):
        return []
    out = []
    if not header[SPH_READS_INTERLOCK_TICKET // 8] >> (SPH_READS_INTERLOCK_TICKET % 8) & 1:
        out.append('no READS_INTERLOCK_TICKET')
    names = [isa.name(w) for w in words]
    if 'ATOM' in names:
        out.append('an atomic ticket wait')
    if not any(n == 'LDG' and isa.field(w, 46, 2) == LDG_CACHE_CG for w, n in zip(words, names, strict=True)):
        out.append('no LDG.CG ticket wait')
    return out


def programs(pack: bytes) -> list[tuple[int, bytes, bytes]]:
    """Give each program of a raw pack as (index, header, code)."""
    nprog = struct.unpack_from('<I', pack, 12)[0]
    sections = [struct.unpack_from('<QQ', pack, 24 + 16 * i) for i in range(HEAP_SECTION + 1)]
    stride = sections[PROGRAM_SECTION][1] // nprog if nprog else 0
    heap = sections[HEAP_SECTION][0]
    out = []
    for i in range(nprog):
        at = heap + struct.unpack_from('<I', pack, sections[PROGRAM_SECTION][0] + stride * i)[0]
        _, _, size, data, _ = struct.unpack_from('<5I', pack, sections[PROGRAM_SECTION][0] + stride * i)
        out.append((i, pack[at + HEADER_AT : at + CODE_AT], pack[at + CODE_AT : at + (data or size)]))
    return out


def main() -> int:
    pack = Path(sys.argv[1]).read_bytes()
    if pack[:8] != b'GM20BPK1':
        raise SystemExit(f'{sys.argv[1]}: not a raw pack')
    readers = bad = 0
    for i, header, code in programs(pack):
        found = faults(header, code)
        readers += reads_ticket(isa.instructions(code))
        bad += bool(found)
        for f in found:
            print(f'program {i}: {f}')
    print(f'{readers} ticket-reading programs, {bad} wrong')
    return 1 if bad or not readers else 0


if __name__ == '__main__':
    sys.exit(main())
