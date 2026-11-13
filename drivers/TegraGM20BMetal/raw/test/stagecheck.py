#!/usr/bin/env python3
"""stagecheck.cpp's set 0 of a pack state, run through the state's programs on the host (tools/gpu/maxwell).

usage: stagecheck.py <pack.bin> <stagecheck json>...

Each stage's program runs on seeded attributes and constant buffers with set 0's bank holding, in turn:
  encoder  the words the raw render encoder wrote
  m2v      each handle entry's word made from what its own stage and slot bind (the probe's assignment of the word to a
           stage and slot, which the record's entries carry) and the sampler its entry names
  merged   m2v's, a vertex handle taking what the fragment stage binds at its slot number: one table for both stages
  narrow   m2v's, a texture slot past 15 bound to nothing: the 16-slot table
The vertex program runs one invocation to EXIT and gives its attribute stores, the pixel program one quad and gives its
outputs; both give every texture fetch's key. encoder has to fetch and write exactly what m2v does; merged and narrow
show the comparison sees a texture taken from the other stage or past the old table.
"""

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))

from tools.gpu.maxwell import gate, isa, nakstat
from tools.gpu.maxwell.execute import run
from tools.gpu.maxwell.interp import Inputs, Lane, UnsupportedError
from tools.gpu.maxwell.quad import LANES

CBUF_SET = 3  # PackCbufType CbufSet
HANDLES = (0, 2, 4)  # raw_pack.h EntrySamplerOnly, EntryTexture, EntryTexture3d
SAMPLER_SLOT, SAMPLER_CONSTEXPR = 1, 2
SEEDS = 4


class Pack:
    def __init__(self, path: Path):
        self.bytes = path.read_bytes()
        version, _, self.count, _ = struct.unpack_from('<4I', self.bytes, 8)
        self.size, self.program_bytes, sections, self.head = nakstat.LAYOUTS[version]
        self.sections = [struct.unpack_from('<QQ', self.bytes, 24 + 16 * i) for i in range(sections)]

    def programs(self, key: int) -> tuple[int, int]:
        for i in range(self.count):
            t = self.head.unpack_from(self.bytes, self.sections[1][0] + self.size * i)
            if t[0] == key and not t[-1] & (2 | 16 | 32):
                return t[2], t[3]
        raise KeyError(f'{key:016x}')

    def program(self, index: int) -> tuple[bytes, int, int]:
        """Give the program's code, the constant buffer slot it reads set 0 at, and the one its bound textures read."""
        at = self.sections[0][0] + self.program_bytes * index
        heap, _, size, data, _ = struct.unpack_from('<5I', self.bytes, at)
        cbufs, types, _, bound = nakstat.program_fields(self.bytes, at, self.program_bytes)
        image = self.bytes[self.sections[7][0] + heap + 0x30 : self.sections[7][0] + heap + (data or size)]
        set_slot = next((c for c in range(1, min(cbufs, len(types))) if types[c] & 7 == CBUF_SET), -1)
        return image, set_slot, bound - 1


def sets(state: dict) -> dict[str, list[int]]:
    textures = {(stage, slot): tic for stage, slot, tic in state['textures']}
    samplers = {(stage, slot): tsc for stage, slot, tsc in state['samplers']}
    m2v, merged, narrow = list(state['words']), list(state['words']), list(state['words'])
    for offset, kind, stage, slot, sampler_kind, sampler, word in state['entries']:
        if kind not in HANDLES:
            continue
        for words, from_stage, slots in ((m2v, stage, 31), (merged, 2, 31), (narrow, stage, 16)):
            tic = textures.get((from_stage, slot), 0) if kind and slot < slots else 0
            tsc = word >> 20 if sampler_kind == SAMPLER_CONSTEXPR else 0
            if sampler_kind == SAMPLER_SLOT:
                tsc = samplers.get((from_stage, sampler), 0)
            words[offset // 4] = tic | tsc << 20
    return {'encoder': list(state['words']), 'm2v': m2v, 'merged': merged, 'narrow': narrow}


def execute(program: tuple[bytes, int, int], words: list[int], vertex: bool, seed: int) -> tuple:
    image, set_slot, bound = program
    pinned = {(set_slot, 4 * i): w for i, w in enumerate(words)} if set_slot >= 0 else {}
    fetches = []

    def recording(inputs: Inputs) -> Inputs:
        texture = inputs.texture

        def fetch(operands: tuple) -> list[int]:
            fetches.append(operands)
            return texture(operands)

        inputs.texture = fetch
        return inputs

    code = image[isa.HEADER_BYTES :]
    if vertex:
        lane = Lane()
        _, killed = run(code, recording(gate.Seeded(seed, 0, pinned).inputs(0, bound)), 0, lane)
        result = (killed, [s for s in lane.stores if s[0] == 'ast'])
    else:
        quad = [recording(gate.Seeded(seed, 0, pinned).inputs(i, bound)) for i in range(LANES)]
        result = tuple(gate.run_pixel(image, quad))
    return result, fetches


def check(pack: Pack, state: dict) -> list[str]:
    key = int(state['key'], 16)
    lines = []
    words = sets(state)
    for stage, index in zip(('vertex', 'pixel'), pack.programs(key), strict=True):
        program = pack.program(index)
        try:
            runs = {
                name: [execute(program, w, stage == 'vertex', s) for s in range(SEEDS)] for name, w in words.items()
            }
        except UnsupportedError as e:
            lines.append(f'{key:016x} {stage}: unsupported {e}')
            continue
        fetches = sum(len(f) for _, f in runs['encoder'])
        same = runs['encoder'] == runs['m2v']
        controls = ', '.join(
            f'{name} {"differs" if runs[name] != runs["m2v"] else "the same"}' for name in ('merged', 'narrow')
        )
        lines.append(
            f'{key:016x} {stage}: {fetches} fetches over {SEEDS} seeds; encoder '
            f'{"the same as" if same else "DIFFERS from"} m2v; {controls}'
        )
    return lines


def main() -> None:
    pack = Pack(Path(sys.argv[1]))
    for path in sys.argv[2:]:
        for line in check(pack, json.loads(Path(path).read_text())):
            print(line)


if __name__ == '__main__':
    main()
