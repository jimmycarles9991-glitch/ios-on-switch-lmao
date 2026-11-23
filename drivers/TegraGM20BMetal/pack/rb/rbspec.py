"""RenderBox's blend state as its fragment functions decode it from their constant block.

Every RenderBox function takes one uint4 function constant, RB::Shader::Constant::_shader_state at index 0
(" #0:36:<16 bytes>" on a corpus line), whose bits choose what the function computes. rb-<build>.json holds one
RenderBox library's reading of it, from the AIR of its fragment functions (their air.static_init and their composite
and blend functions): each field as (word, shift, mask), the blend modes of its blend() switch by the formula each case
computes, and the functions whose fetched destination feeds composite's blend and nothing else, with how their depth
and stencil tests run:
  early_tests    [[early_fragment_tests]]: the hardware tests before the program
  exports_depth  the function writes depth (air.depth), so the tests follow the program
  discards       it calls discard_fragment and declares no early tests, so the tests follow the program
A table is picked by the digest of the same IPSW's QuartzCore library, the one the pack's split step is given.
"""

import json
import re
from pathlib import Path
from typing import NamedTuple

STATE = re.compile(r'#0:36:([0-9a-f]{32})')
FRAGMENT = re.compile(r' \| f (\S+)')
COLOUR = re.compile(r' c(\d+)=(\d+):(\d+):(\S+)')
DEPTH_STENCIL = re.compile(r' ds=(\d+)/(\d+)')


class Tables(NamedTuple):
    """One RenderBox library's rb-<build>.json."""

    build: str
    functions: dict[str, str]
    fields: dict[str, tuple[int, int, int]]
    edit: tuple[int, int]
    coverage_target: int
    saturated_unless_extended: frozenset[int]
    modes: dict[int, str]


class Spec(NamedTuple):
    """A corpus line's fragment function, its constant block and its colour attachments."""

    function: str
    words: tuple[int, int, int, int]
    colours: dict[int, tuple[int, int, str]]  # slot: (MTLPixelFormat, write mask, the blend fields after them)
    tested: bool  # a depth or stencil attachment

    def field(self, tables: Tables, name: str) -> int:
        """Give a field of the constant block, 0 for one the build's library has none of."""
        if name not in tables.fields:
            return 0
        word, shift, mask = tables.fields[name]
        return self.words[word] >> shift & mask


def load(path: Path) -> Tables:
    """Read one rb-<build>.json."""
    with open(path) as f:
        t = json.load(f)
    return Tables(
        t['build'],
        t['functions'],
        {k: tuple(v) for k, v in t['fields'].items()},
        tuple(t['edit']),
        t['coverage_target'],
        frozenset(t['saturated_unless_extended']),
        {int(k): v for k, v in t['modes'].items()},
    )


def for_quartzcore(directory: Path, digest: str) -> Tables | None:
    """Give the tables of the IPSW whose QuartzCore library has SHA-256 `digest`, None where none is here."""
    for path in sorted(directory.glob('rb-*.json')):
        with open(path) as f:
            if json.load(f).get('quartzcore_metallib_sha256') == digest:
                return load(path)
    return None


def renderbox_line(line: str) -> bool:
    """Whether an R line's functions are RenderBox's library's."""
    return line.startswith('R ') and '/RenderBox.framework/default.metallib ' in line


def decode(line: str) -> Spec | None:
    """Give a RenderBox R line's fragment function and its constant block, None where it names neither."""
    fragment = FRAGMENT.search(line)
    if not fragment:
        return None
    state = STATE.search(line, fragment.end())
    end = line.find(' | ', fragment.end())
    if not state or (end >= 0 and state.start() > end):
        return None
    raw = bytes.fromhex(state[1])
    words = tuple(int.from_bytes(raw[4 * i : 4 * i + 4], 'little') for i in range(4))
    head = line.split(' | ', 1)[0]
    colours = {int(c): (int(f), int(m), rest) for c, f, m, rest in COLOUR.findall(head)}
    ds = DEPTH_STENCIL.search(head)
    return Spec(fragment[1], words, colours, bool(ds and (int(ds[1]) or int(ds[2]))))


def edited(spec: Spec, tables: Tables) -> str:
    """Give the constant block with the blend and the destination read cleared.

    That is composite's unblended arm, which writes the source times its coverage.
    """
    word, clear = tables.edit
    words = list(spec.words)
    words[word] &= ~clear
    return b''.join(w.to_bytes(4, 'little') for w in words).hex()
