"""The raw path's direct-compiled records of states past 32 set-0 entries (direct/exec.metal), executed on the host.

wide_set reads twenty storage buffers a stage, each holding a distinct power of two, so a descriptor at the wrong
set-0 byte or a duplicate shows in the sum. far_uniform adds a fragment uniform buffer whose descriptor is past set-0
byte 496, which NVK promotes to a constant buffer: its bank is the buffer at that descriptor (raw_pack.h
programUboOffset). field_bound binds every argument the A10X's tables let a stage bind (31 buffers, 31 textures, 16
samplers), field_bound_wide every argument the A13's do (31 buffers, 128 textures, 16 samplers): each buffer is read
once and each texture through its own header with its sampler's. The build's identity draws the state of its own bound
(156 arguments on 21H461, 350 on 24B119) and refuses a wider one.

usage: direct_wide_test.py <direct_runner dir> <shader tools dir>
"""

import struct
import sys
import unittest
from pathlib import Path
from typing import ClassVar

from tools.gpu.maxwell import direct_records as dr
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import run_quad
from tools.gpu.maxwell.stage import Stage

RECORDS: dict = {}
ATTR_POSITION, ATTR_GENERIC, ATTR_INSTANCE_ID, ATTR_VERTEX_ID = 0x70, 0x80, 0x2F8, 0x2FC
ENTRY_SAMPLER_ONLY, ENTRY_UBO, ENTRY_TEXTURE, ENTRY_SPILL_TABLE = 0, 1, 2, 10
SAMPLER_SLOT, CBUF_UBO = 1, 5
VERTEX, FRAGMENT = 1, 2
SPILL_ADDRESS = 0xF2_0000_0000
PACK_ENTRIES = 32  # raw_pack.h kPackEntries: a record's entries in its PackPipeline, the rest out of line


def tic(stage: int, slot: int) -> int:
    return 0x100 + 0x100 * stage + slot


def tsc(stage: int, slot: int) -> int:
    return 0x10 + 0x20 * stage + slot


class Draw:
    """A draw of `record`: buffer (stage, slot) bound to `buffers`' bytes, each texture and sampler its own header."""

    def __init__(self, record: dict, buffers: dict[tuple[int, int], bytes]) -> None:
        self.record = record
        self.binding = dr.Binding()
        for key, data in buffers.items():
            self.binding.bind(key, data)
        self.fetched: list[tuple[int, int, int]] = []
        set0 = bytearray(max(256, record['set_bytes']))
        for offset, kind, stage, slot, sampler_kind, sampler, pad in record['entries']:
            handle = 0
            if kind == ENTRY_UBO:
                set0[offset : offset + 16] = self.binding.descriptor(
                    (stage, slot), bool(sampler_kind & 1), (sampler | pad << 8) * 16
                )
                continue
            if kind == ENTRY_SPILL_TABLE:
                words = record['spill'][stage - 1]
                table = SPILL_ADDRESS + stage * 0x1000
                self.binding.memory.add(table, struct.pack(f'<{len(words)}I', *words).ljust(256, b'\0'))
                set0[offset : offset + 16] = struct.pack('<IIII', table & dr.MASK, table >> 32, 256, 0)
                continue
            if kind == ENTRY_TEXTURE:
                handle |= tic(stage, slot)
            elif kind != ENTRY_SAMPLER_ONLY:
                raise ValueError(f'a set-0 entry of kind {kind} the test bound nothing at')
            if sampler_kind == SAMPLER_SLOT:
                handle |= tsc(stage, sampler) << 20
            struct.pack_into('<I', set0, offset, handle)
        self.set0 = bytes(set0)
        self.binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), self.set0)

    def descriptor_at(self, offset: int) -> tuple[int, int]:
        lo, hi, size, _ = struct.unpack_from('<IIII', self.set0, offset)
        return lo | hi << 32, size

    def cbuf(self, program: dict, bank: int, offset: int) -> int:
        far = dict(program['ubo_offsets'])
        kinds = program['cbufs']
        if bank in far or (bank < len(kinds) and kinds[bank] & 7 == CBUF_UBO):
            at = far.get(bank, (kinds[bank] >> 3) * 16)
            base, size = self.descriptor_at(at)
            if offset + 4 > size:
                raise UnsupportedError(f'c{bank}[{offset:#x}] past its uniform buffer of {size} bytes')
            return struct.unpack('<I', self.binding.memory.read(base + offset, 4))[0]
        banks = dr.banks(program, self.binding.root, self.set0)
        if bank not in banks or offset + 4 > len(banks[bank]):
            raise UnsupportedError(f'c{bank}[{offset:#x}] read where no constant buffer has it')
        return struct.unpack_from('<I', banks[bank], offset)[0]

    def texture(self, key: tuple, stage: int) -> list[int]:
        if key[0] == 'ldg':
            (lo, hi), off = key[1], key[2]
            address = (lo | hi << 32) + (off - (1 << 24) if off & 1 << 23 else off)
            return list(struct.unpack('<4I', self.binding.memory.read(address, 16).ljust(16, b'\0')))
        if key[0] in ('tex', 'tld'):
            self.fetched.append((stage, key[1] & 0xFFFFF, key[1] >> 20))
            return [dr.f32(1.0)] * 4
        raise UnsupportedError(f'a fetch {key[0]}')

    def vertex(self, vid: int) -> list[float]:
        program = self.record['programs'][0]
        draw = self

        class VertexStage(Stage):
            def inputs(self) -> Inputs:
                def no_ipa(_address: int) -> int:
                    raise UnsupportedError('IPA in a vertex stage')

                return Inputs(
                    no_ipa,
                    lambda bank, offset: draw.cbuf(program, bank, offset),
                    lambda key: draw.texture(key, VERTEX),
                    program['bound_textures'] - 1,
                )

        words = {ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}

        def attribute(address: int, _vertex: int, _patch: bool) -> int:
            if address not in words:
                raise UnsupportedError(f'ALD {address:#x}, an attribute the test gave no word for')
            return words[address]

        environment = VertexStage({}, self.binding.memory, attribute, {})
        dr.execute(program, environment)
        return [dr.value(environment.outputs[(0, ATTR_GENERIC + 4 * c)]) for c in range(4)]

    def fragment(self, colour: list[float]) -> list[float]:
        program = self.record['programs'][1]
        words = {ATTR_POSITION + 4 * c: dr.f32(v) for c, v in enumerate((0.5, 0.5, 0.5, 1.0))}
        words.update({ATTR_GENERIC + 4 * c: dr.f32(v) for c, v in enumerate(colour)})

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        lanes = [
            Inputs(
                attribute,
                lambda bank, offset: self.cbuf(program, bank, offset),
                lambda key: self.texture(key, FRAGMENT),
                program['bound_textures'] - 1,
            )
            for _ in range(4)
        ]
        colour_out, killed, _ = run_quad(dr.code(program), lanes, 4)[0]
        if killed:
            raise UnsupportedError('the fragment was killed')
        return [dr.value(w) for w in colour_out]


class Wide(unittest.TestCase):
    def state(self, name: str) -> dict:
        self.assertIn(name, RECORDS['states'], RECORDS['refused'].get(name, 'not made'))
        return RECORDS['states'][name]

    def buffer_entries(self, record: dict, stage: int) -> dict[int, int]:
        """Give the stage's buffer entries: slot -> set-0 byte."""
        return {e[3]: e[0] for e in record['entries'] if e[1] == ENTRY_UBO and e[2] == stage}


class WideSet(Wide):
    """Twenty storage buffers a stage: 40 Metal arguments, every one its own set-0 entry."""

    @staticmethod
    def buffers() -> dict[tuple[int, int], bytes]:
        out = {(VERTEX, i): dr.floats(float(1 << i)) for i in range(20)}
        out.update({(FRAGMENT, i): dr.floats(float(1 << i) / (1 << 20)) for i in range(20)})
        return out

    def test_the_record_holds_every_buffer_past_32_entries(self):
        record = self.state('wide_set')
        self.assertGreater(len(record['entries']), PACK_ENTRIES)
        self.assertEqual(len(record['entries']), record['entry_count'])
        for stage in (VERTEX, FRAGMENT):
            self.assertEqual(sorted(self.buffer_entries(record, stage)), list(range(20)), f'stage {stage}')
        offsets = [e[0] for e in record['entries']]
        self.assertEqual(len(offsets), len(set(offsets)), 'each entry its own set-0 bytes')

    def test_each_stage_reads_every_buffer_through_its_own_descriptor(self):
        draw = Draw(self.state('wide_set'), self.buffers())
        colour = draw.vertex(0)
        self.assertEqual(colour[1], float((1 << 20) - 1))
        self.assertEqual(draw.fragment(colour), [((1 << 20) - 1) / (1 << 20), float((1 << 20) - 1), 0.0, 1.0])


class FarUniform(Wide):
    """WideSet's fragment stage and a uniform buffer after its twenty storage buffers: its descriptor past byte 496."""

    TINT = (2.0, 0.5, 3.0, 1.0)

    def test_the_fragment_program_binds_the_uniform_buffer_past_byte_496(self):
        record = self.state('far_uniform')
        uniform = [e[0] for e in record['entries'] if e[1] == ENTRY_UBO and e[2] == FRAGMENT and e[3] == 20]
        self.assertEqual(len(uniform), 1)
        self.assertGreater(uniform[0], 496)
        self.assertIn(uniform[0], [offset for _, offset in record['programs'][1]['ubo_offsets']])

    def test_the_fragment_program_reads_the_uniform_buffer_as_a_constant_buffer(self):
        buffers = WideSet.buffers()
        buffers[(FRAGMENT, 20)] = dr.floats(*self.TINT)
        draw = Draw(self.state('far_uniform'), buffers)
        colour = draw.vertex(0)
        want = [((1 << 20) - 1) / (1 << 20) * 2.0, float((1 << 20) - 1) * 0.5, 0.0, 1.0]
        self.assertEqual(draw.fragment(colour), want)


class TwoFarUniforms(Wide):
    """Two fragment uniform buffers after twenty storage buffers, both past byte 496: each its own constant buffer."""

    TINT, SHIFT = (2.0, 0.5, 3.0, 1.0), (1.0, 2.0, 3.0, 4.0)

    def test_the_fragment_program_binds_each_uniform_buffer_at_its_own_offset(self):
        record = self.state('two_far')
        fragment = {
            e[3]: e[0] for e in record['entries'] if e[1] == ENTRY_UBO and e[2] == FRAGMENT and e[3] in (20, 21)
        }
        self.assertEqual(sorted(fragment), [20, 21])
        self.assertTrue(all(offset > 496 for offset in fragment.values()), fragment)
        self.assertLessEqual(set(fragment.values()), {o for _, o in record['programs'][1]['ubo_offsets']})

    def test_the_fragment_program_reads_both_uniform_buffers(self):
        buffers = WideSet.buffers()
        buffers[(FRAGMENT, 20)] = dr.floats(*self.TINT)
        buffers[(FRAGMENT, 21)] = dr.floats(*self.SHIFT)
        draw = Draw(self.state('two_far'), buffers)
        colour = draw.vertex(0)
        fsum, vsum = ((1 << 20) - 1) / (1 << 20), float((1 << 20) - 1)
        want = [fsum * 2.0 + 1.0, vsum * 0.5 + 2.0, 3.0, 5.0]
        self.assertEqual(draw.fragment(colour), want)


class FieldBound(Wide):
    """Every argument the identity's tables let a stage bind.

    A stage has 31 buffers, 31 (A10X) or 128 (A13) textures and 16 samplers.
    """

    BUFFERS, SAMPLERS = 31, 16
    STATES: ClassVar[dict[int, str]] = {31: 'field_bound', 128: 'field_bound_wide'}

    def bound(self) -> tuple[str, int]:
        textures = RECORDS['stage_textures']
        self.assertIn(textures, self.STATES)
        return self.STATES[textures], textures

    def test_the_record_binds_the_identitys_metal_arguments(self):
        name, textures = self.bound()
        entries = self.state(name)['entries']
        arguments = 0
        for stage in (VERTEX, FRAGMENT):
            arguments += len(self.buffer_entries(self.state(name), stage))
            arguments += len({e[3] for e in entries if e[1] == ENTRY_TEXTURE and e[2] == stage})
            arguments += len({e[5] for e in entries if e[4] == SAMPLER_SLOT and e[2] == stage})
        self.assertEqual(arguments, 156 if textures == 31 else 350)

    def test_the_record_holds_every_argument(self):
        name, textures = self.bound()
        record = self.state(name)
        self.assertGreaterEqual(len(record['entries']), 2 * self.BUFFERS + 2 * textures)
        for stage in (VERTEX, FRAGMENT):
            self.assertEqual(sorted(self.buffer_entries(record, stage)), list(range(self.BUFFERS)), f'stage {stage}')
            bound = {e[3] for e in record['entries'] if e[1] == ENTRY_TEXTURE and e[2] == stage}
            self.assertEqual(bound, set(range(textures)), f'stage {stage}')

    def test_each_stage_reads_every_buffer_and_samples_every_texture_with_its_sampler(self):
        name, textures = self.bound()
        buffers = {(stage, i): dr.floats(float(i + 1)) for stage in (VERTEX, FRAGMENT) for i in range(self.BUFFERS)}
        draw = Draw(self.state(name), buffers)
        colour = draw.vertex(0)
        self.assertEqual(colour[:2], [496.0, float(textures)])
        self.assertEqual(draw.fragment(colour), [992.0, 2.0 * textures, 0.0, 1.0])
        for stage in (VERTEX, FRAGMENT):
            fetched = {(t, s) for st, t, s in draw.fetched if st == stage}
            want = {(tic(stage, i), tsc(stage, i % self.SAMPLERS)) for i in range(textures)}
            self.assertEqual(fetched, want, f'stage {stage}')

    def test_a_state_past_the_identitys_textures_is_refused(self):
        _, textures = self.bound()
        if textures == 128:
            self.skipTest('24B119 binds the widest state the runner makes')
        self.assertNotIn('field_bound_wide', RECORDS['states'])
        self.assertIn('past the', RECORDS['refused']['field_bound_wide'])


if __name__ == '__main__':
    RECORDS.update(dr.load(Path(sys.argv[1])))
    unittest.main(argv=sys.argv[:1])
