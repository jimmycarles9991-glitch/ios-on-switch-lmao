"""Maxwell (SM50-SM53) machine code: instruction identification and the field reads the tools here share.

Instructions come four 64-bit words at a time, the first a scheduling word (NAK's sm50.rs encode_instr). The opcode
patterns are opcodes.txt's: yuzu's shader recompiler's maxwell.inc (GPL-2.0-or-later), kept verbatim: sixteen
characters for bits 63-48, '-' a bit the pattern leaves free.
"""

import re
import struct
from dataclasses import dataclass
from pathlib import Path

TABLE = Path(__file__).with_name('opcodes.txt')
ENTRY = re.compile(r'INST\((\w+),\s*"([^"]*)",\s*"([01\- ]+)"\)')
HEADER_BYTES = 0x50  # a graphics program's shader program header (NAK sph.rs), ahead of its code


@dataclass(frozen=True)
class Pattern:
    name: str
    mask: int
    value: int


def _load(path: Path = TABLE) -> list[Pattern]:
    out = []
    for name, _, spaced in ENTRY.findall(path.read_text()):
        bits = spaced.replace(' ', '')
        mask = value = 0
        for i, c in enumerate(bits):
            shift = 63 - i
            if c != '-':
                mask |= 1 << shift
                value |= int(c) << shift
        out.append(Pattern(name, mask, value))
    # The most specific pattern first: ATOM_cas before ATOM, and so on
    out.sort(key=lambda p: -p.mask.bit_count())
    return out


PATTERNS = _load()


def name(word: int) -> str:
    for p in PATTERNS:
        if word & p.mask == p.value:
            return p.name
    return 'UNKNOWN'


def field(word: int, at: int, n: int) -> int:
    return (word >> at) & ((1 << n) - 1)


def words_of(code: bytes) -> list[int]:
    """Give the 64-bit words of `code`, the trailing zero words (a slot's padding) left out."""
    words = [struct.unpack_from('<Q', code, i)[0] for i in range(0, len(code) - len(code) % 8, 8)]
    while words and words[-1] == 0:
        words.pop()
    return words


def instructions(code: bytes) -> list[int]:
    """`code`'s instructions in order, without the scheduling words."""
    return [w for i, w in enumerate(words_of(code)) if i % 4]


def from_hex_dump(text: str) -> bytes:
    """Give the bytes of NAK_DEBUG=print's "Encoded shader:" lines: 32-bit words, low word first."""
    return b''.join(struct.pack('<I', int(t, 16)) for t in text.split())
