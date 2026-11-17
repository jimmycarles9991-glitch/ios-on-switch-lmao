"""The raw path's direct-compiled records of direct/exec.metal's states and kernels, executed on the host.

directrun (direct/directrun.mm) makes each record as the runtime makes it at a state's creation: m2v's translation,
NVK's stage compile, raw_direct.cpp's record. Each program then runs here (stage.py over interp.py), bound as the raw
path binds it (direct_records.py) and fed the words a draw or dispatch writes from the record's own fields (control
word, amplification id word, pulled stride words, stage-in region word). Every expected value is the Metal source's
arithmetic on the CPU, on inputs a zero or a stub would not pass: patch factors culled, clamped and kept, tessellation
coordinates off the corners, amplification 1, a stage-in origin past 0, a stride past 4095 bytes, buffers across a
4 GiB boundary, counter spans across 2^32, a fragment quad's storage image texels in each layer of a layered pass
(quad.py), every layer's its own, tessellation factors up to 64, eight amplifications, 32 float4 varyings, and a kernel
of 48 KiB of threadgroup memory under its QMD's shared memory size.

usage: direct_exec_test.py <direct_runner dir> <shader tools dir>
"""

import math
import struct
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import ClassVar

from tools.gpu.maxwell import direct_records as dr
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import run_quad

RECORDS: dict = {}
ATTR_LAYER, ATTR_VIEWPORT, ATTR_POSITION, ATTR_GENERIC = 0x64, 0x68, 0x70, 0x80
ATTR_TESS_COORD, ATTR_INSTANCE_ID, ATTR_VERTEX_ID = 0x2F0, 0x2F8, 0x2FC
ATTR_FRONT_FACE, MASK = 0x3FC, 0xFFFFFFFF
TESS_LEVELS = {'outer': (0x000, 0x004, 0x008), 'inner': 0x010}
SV_LANE_ID, SV_INVOCATION_ID, SV_INVOCATION_INFO = 0x00, 0x11, 0x1D
SV_COMBINED_TID, SV_TID_X, SV_CTAID_X = 0x20, 0x21, 0x25
# a tessellation stage's one invocation of a patch of one vertex: SV_INVOCATION_INFO's byte 0 the patch in the batch,
# byte 2 its vertices (NAK's per-vertex input index, nak_nir_lower_vtg_io.c)
CONTROL_SYSTEM = {SV_LANE_ID: 0, SV_INVOCATION_ID: 0, SV_INVOCATION_INFO: 1 << 16}


def outputs(environment: dr.Stage, base: int, count: int, vertex: int = 0) -> list[float]:
    return [dr.value(environment.outputs[(vertex, base + 4 * k)]) for k in range(count)]


def attributes(words: dict[int, int]) -> object:
    def attribute(address: int, _vertex: int, _patch: bool) -> int:
        if address not in words:
            raise UnsupportedError(f'ALD {address:#x}, an attribute the test gave no word for')
        return words[address]

    return attribute


class Records(unittest.TestCase):
    def state(self, name: str) -> dict:
        self.assertIn(name, RECORDS['states'], RECORDS['refused'].get(name, 'not made'))
        return RECORDS['states'][name]

    def kernel(self, name: str) -> dict:
        self.assertIn(name, RECORDS['kernels'], RECORDS['refused'].get(name, 'not made'))
        return RECORDS['kernels'][name]


class VertexCubic(Records):
    SIDE, HEADER, SAMPLER = 8, 0x123, 0x56
    # Quarter-fraction rows of Metal's measured 2^-16 weight table (metal/bicubic.rs).
    WEIGHTS: ClassVar = {
        0.0: (0, 1, 0, 0),
        0.25: tuple(x / 128 for x in (-9, 111, 29, -3)),
        0.5: tuple(x / 16 for x in (-1, 9, 9, -1)),
        0.75: tuple(x / 128 for x in (-3, 29, 111, -9)),
    }

    @classmethod
    def texel(cls, x: int, y: int) -> list[float]:
        at = (min(max(x, 0), cls.SIDE - 1), min(max(y, 0), cls.SIDE - 1))
        return [float(at == point) for point in ((2, 3), (3, 4), (0, 0), (7, 7))]

    @staticmethod
    def half_away(value: float) -> float:
        magnitude = abs(value)
        bits = struct.unpack('<H', struct.pack('<e', magnitude))[0]
        rounded = struct.unpack('<e', struct.pack('<H', bits))[0]
        if rounded < magnitude:
            higher = struct.unpack('<e', struct.pack('<H', bits + 1))[0]
            if magnitude - rounded == higher - magnitude:
                rounded = higher
        return math.copysign(rounded, value)

    @classmethod
    def reference(cls, point: tuple[float, float], cubic: bool) -> list[float]:
        x, y = (math.floor(v) for v in point)
        fx, fy = point[0] - x, point[1] - y
        wx, wy = (cls.WEIGHTS[fx], cls.WEIGHTS[fy]) if cubic else ((0, 1 - fx, fx, 0), (0, 1 - fy, fy, 0))
        return [
            sum(wx[i] * wy[j] * cls.texel(x + i - 1, y + j - 1)[channel] for j in range(4) for i in range(4))
            for channel in range(4)
        ]

    def run_vertex(self, name: str, point: tuple[float, float]) -> list[float]:
        record = self.state(name)
        program = record['programs'][0]
        binding = dr.Binding()
        uv = tuple((v + 0.5) / self.SIDE for v in point)
        binding.bind((1, 0), dr.floats(*uv))
        buffers = dict(record, entries=[entry for entry in record['entries'] if entry[1] == dr.ENTRY_UBO])
        set0 = bytearray(dr.render_set(buffers, binding))
        for offset, kind, stage, slot, sampler_kind, _sampler, _pad in record['entries']:
            if kind == dr.ENTRY_UBO:
                continue
            self.assertEqual((stage, slot), (1, 0))
            self.assertIn(kind, (0, 2))
            handle = self.HEADER if kind == 2 else 0
            if kind == 0 or sampler_kind == 1:
                handle |= self.SAMPLER << 20
            struct.pack_into('<I', set0, offset, handle)
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), bytes(set0))
        environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: 0}), {})
        fetched = []

        def texture(key: tuple) -> list[int]:
            self.assertEqual(key[1] & 0xFFFFF, self.HEADER)
            if key[0] == 'txq':
                self.assertEqual(key[2], 1)
                return [self.SIDE, self.SIDE, 1, 1]
            fetched.append(key)
            if key[0] == 'tld':
                self.assertEqual(key[6], 2)
                self.assertIn(key[3], (None, 0))
                coords = [v - (1 << 32) if v & (1 << 31) else v for v in key[2]]
                offset = key[4] or (0, 0)
                return [dr.f32(v) for v in self.texel(coords[0] + offset[0], coords[1] + offset[1])]
            if key[0] == 'tex':
                self.assertEqual(key[7], 2)
                self.assertIn(key[3], (1, 3))
                self.assertIn(key[4], (None, 0))
                at = tuple(dr.value(v) * self.SIDE - 0.5 for v in key[2])
                return [dr.f32(v) for v in self.reference(at, False)]
            raise UnsupportedError(f'a vertex cubic fetch {key[0]}')

        inputs = environment.inputs()
        inputs.texture = texture
        dr.run_lane(dr.code(program), inputs, environment)
        self.assertTrue(fetched)
        self.assertEqual(outputs(environment, ATTR_POSITION, 4), [-1.0, -1.0, 0.0, 1.0])
        return outputs(environment, ATTR_GENERIC, 4)

    def test_native_vertex_cubic_matches_the_measured_format_rules(self):
        linear = self.state('cubic_vertex_linear')
        half = self.state('cubic_vertex_half')
        single = self.state('cubic_vertex_float')
        self.assertNotEqual(half['programs'][0]['image'], linear['programs'][0]['image'])
        self.assertEqual(half['programs'][1]['image'], linear['programs'][1]['image'])
        self.assertEqual(single['programs'][1]['image'], linear['programs'][1]['image'])
        differs = False
        for point in ((-0.75, 0.25), (0.5, 0.5), (2.25, 3.75), (3.5, 4.5), (6.75, 6.25), (7.75, 7.25)):
            with self.subTest(point=point):
                expected_linear = self.reference(point, False)
                expected_cubic = [self.half_away(v) for v in self.reference(point, True)]
                self.assertEqual(self.run_vertex('cubic_vertex_linear', point), expected_linear)
                self.assertEqual(self.run_vertex('cubic_fragment_only', point), expected_linear)
                self.assertEqual(self.run_vertex('cubic_vertex_float', point), expected_linear)
                self.assertEqual(self.run_vertex('cubic_vertex_half', point), expected_cubic)
                differs |= expected_cubic != expected_linear
        self.assertTrue(differs)


class Tessellation(Records):
    """A triangle-patch state, m2v's vertex, control and evaluation stages.

    The vertex stage per patch, the control stage's levels and patch outputs from the factor buffer, and the
    post-tessellation vertex function at tessellation coordinates over the patch's control points.
    """

    NAME, TOP = 'tessellation', 16.0
    POINTS = tuple((float(k), float(2 * k + 1), float(-k), 1.0) for k in range(12))
    # MTLTriangleTessellationFactorsHalf per patch: three edge factors and the inside one
    FACTORS = ((2.0, 3.0, 4.0, 5.0), (40.0, 0.5, 6.0, 0.25), (0.0, 3.0, 3.0, 3.0), (70.0, 2.0, 3.0, 65.0))
    COORDS = ((0.25, 0.5), (0.5, 0.125), (0.0, 0.75))

    @staticmethod
    def levels(factors: tuple, top: float = 16.0) -> tuple[list[float], float | None]:
        """Give Metal's levels of a patch's factors under the integer partition.

        A patch of an edge factor at or below 0 is culled (every outer level 0, its inner one unread); each factor is
        clamped to [1, maxTessellationFactor].
        """
        edges, inside = factors[:3], factors[3]
        if any(not e > 0 for e in edges):
            return [0.0, 0.0, 0.0], None
        return [max(1.0, min(top, e)) for e in edges], max(1.0, min(top, inside))

    def setUp(self) -> None:
        self.record = self.state(self.NAME)
        self.assertEqual(len(self.record['tess_programs']), 2)
        self.binding = dr.Binding()
        self.binding.bind((1, 0), dr.floats(*[c for p in self.POINTS for c in p]))
        half = b''.join(struct.pack('<4e', *f) for f in self.FACTORS)
        self.binding.bind(('tess', 0), half)
        self.binding.bind(('tess', 1), bytes(256))
        self.binding.bind(('tess', 2), bytes(256))
        push = dr.record_push(self.record)
        first = self.record['control'][0] - 1
        self.assertGreaterEqual(first, 0)
        for k, word in enumerate([0, 0, dr.f32(1.0), 0, 0, 0]):
            push[first + k] = word
        self.binding.tables(dr.root_table(push, self.record['set_bytes']), dr.render_set(self.record, self.binding))

    def run_patch(self, patch: int) -> tuple[dr.Stage, dr.Stage]:
        vertex_program, _fragment = self.record['programs']
        control_program, _evaluation = self.record['tess_programs']
        system = {ATTR_VERTEX_ID: patch, ATTR_INSTANCE_ID: 0}
        vertex = dr.execute(vertex_program, dr.stage(vertex_program, self.binding, attributes(system), {}))
        written = {address: word for (_, address), word in vertex.outputs.items()}
        control = dr.stage(control_program, self.binding, attributes(written), CONTROL_SYSTEM)
        return vertex, dr.execute(control_program, control)

    def test_the_control_stage_writes_each_patchs_levels_from_its_factors(self):
        for patch, factors in enumerate(self.FACTORS):
            _, control = self.run_patch(patch)
            outer, inner = self.levels(factors, self.TOP)
            got = [dr.value(control.patch_outputs[a]) for a in TESS_LEVELS['outer']]
            self.assertEqual(got, outer, f'patch {patch}')
            if inner is not None:
                self.assertEqual(dr.value(control.patch_outputs[TESS_LEVELS['inner']]), inner, f'patch {patch}')

    def test_the_evaluation_stage_interpolates_the_patchs_control_points(self):
        evaluation_program = self.record['tess_programs'][1]
        for patch in range(len(self.FACTORS)):
            _, control = self.run_patch(patch)
            patch_words = dict(control.patch_outputs)
            for u, v in self.COORDS:
                w = 1.0 - u - v
                evaluation = dr.stage(evaluation_program, self.binding, attributes(patch_words), CONTROL_SYSTEM)
                # the tessellator's coordinate: a per-vertex output of the lane (NAK from_nir.rs load_tess_coord)
                evaluation.outputs[(0, ATTR_TESS_COORD)] = dr.f32(u)
                evaluation.outputs[(0, ATTR_TESS_COORD + 4)] = dr.f32(v)
                dr.execute(evaluation_program, evaluation)
                points = self.POINTS[3 * patch : 3 * patch + 3]
                expected = [points[0][c] * u + points[1][c] * v + points[2][c] * w for c in range(4)]
                self.assertEqual(outputs(evaluation, ATTR_POSITION, 4), expected, f'patch {patch} at {u}, {v}')
                self.assertEqual(outputs(evaluation, ATTR_GENERIC, 4), [u, v, w, float(patch)])


class Tessellation64(Tessellation):
    """The same patches under maxTessellationFactor 64, the device's (GM20B's) maximum: 40 is kept, 70 is 64."""

    NAME, TOP = 'tessellation64', 64.0


class Amplification(Records):
    """A state of maxVertexAmplificationCount 2.

    Each amplification's draw writes its id into the record's AmplificationId word, and the vertex function reads it as
    [[amplification_id]]; it writes its mapping's offsets and the pass's layer and viewport counts into the record's
    words, and the vertex program stores Layer and ViewportIndex from them: the offset below the count, else 0.
    """

    POINTS = ((1.0, 2.0, 3.0, 1.0), (-4.0, 0.5, 0.25, 1.0), (8.0, -16.0, 0.0, 2.0))

    NAME, COUNT = 'amplification', 2
    # (layer offset, layers, viewport offset, viewports) of each amplification tested: amplification 1's layer past the
    # pass's length renders layer 0
    MAPPINGS: ClassVar[dict[int, tuple[int, int, int, int]]] = {0: (1, 2, 3, 4), 1: (5, 2, 1, 4)}

    def test_each_amplifications_vertices(self):
        record = self.state(self.NAME)
        amplification_max, id_word, count_word = record['amplification']
        layer_word, viewport_word = record['amplification_offsets']
        length_word, count_word_of_viewports = record['pass_words']
        self.assertEqual(amplification_max, self.COUNT)
        self.assertGreater(id_word, 0)
        self.assertEqual(count_word, 0)
        for word in (layer_word, viewport_word, length_word, count_word_of_viewports):
            self.assertGreater(word, 0)
        program = record['programs'][0]
        mappings = self.MAPPINGS
        for amplification in mappings:
            binding = dr.Binding()
            binding.bind((1, 0), dr.floats(*[c for p in self.POINTS for c in p]))
            push = dr.record_push(record)
            push[id_word - 1] = amplification
            layer, layers, viewport, viewports = mappings[amplification]
            push[layer_word - 1], push[length_word - 1] = layer, layers
            push[viewport_word - 1], push[count_word_of_viewports - 1] = viewport, viewports
            binding.tables(dr.root_table(push, record['set_bytes']), dr.render_set(record, binding))
            for vid, point in enumerate(self.POINTS):
                words = {ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}
                environment = dr.stage(program, binding, attributes(words), {})
                dr.execute(program, environment)
                expected = [point[0] + 0.5 * amplification, *point[1:]]
                self.assertEqual(outputs(environment, ATTR_POSITION, 4), expected, f'{amplification} {vid}')
                self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [float(amplification), float(vid), 0.0, 1.0])
                self.assertEqual(environment.outputs[(0, ATTR_LAYER)], layer if layer < layers else 0)
                self.assertEqual(environment.outputs[(0, ATTR_VIEWPORT)], viewport if viewport < viewports else 0)


class Amplification8(Amplification):
    """maxVertexAmplificationCount 8, every count the amplify macro keeps a mapping word for (SWITCH_GM20B_AMP_MAX)."""

    NAME, COUNT = 'amplification8', 8
    MAPPINGS: ClassVar[dict[int, tuple[int, int, int, int]]] = {
        0: (1, 8, 3, 16),
        5: (6, 8, 15, 16),
        7: (7, 8, 16, 16),
    }


class LayeredFetch(Records):
    """A slice-choosing state of m2v's layered storage form, whose fragment function fetches [[color(0)]].

    The vertex program stores instance i as Layer below the pass's length word (an index at or past it as 0). The
    fragment program, run on quads of each layer of passes of 2 and 6 layers, reads its Layer input and loads and stores
    the attachment's storage image, a 2D array (SULD and SUST of three coordinates), at (x, y, Layer): each layer's own
    texel with its channels rotated, as the Metal source computes it on the CPU, compared as the RGBA8 codes stored.
    """

    HEADER = 0x123  # the attachment view's header index set 0 holds (raw_gpu.cpp: tic | tsc << 20, no sampler)
    QUADS = ((2, 4), (13, 7))

    @staticmethod
    def codes(x: int, y: int, layer: int) -> list[int]:
        """Give the RGBA8 codes of the storage image's texel (x, y) of `layer`, each layer's its own."""
        return [(7 * x + 31 * layer) & 255, (5 * y + 47 * layer) & 255, (x + y + 90 * layer) & 255, 200 + layer]

    @classmethod
    def texel(cls, x: int, y: int, layer: int) -> list[int]:
        """Give the texel as SULD.P loads an RGBA8Unorm one: each code's value."""
        return [dr.f32(c / 255) for c in cls.codes(x, y, layer)]

    def setUp(self) -> None:
        self.record = self.state('layered_fetch')
        storage_mask, _writes, _view = self.record['storage']
        self.assertEqual(storage_mask, 1, 'the storage form')
        self.length_word = self.record['pass_words'][0]
        self.assertTrue(0 < self.length_word <= 32, 'the layered form names its length word')

    def binding(self, layers: int) -> dr.Binding:
        binding = dr.Binding()
        push = dr.record_push(self.record)
        push[self.length_word - 1] = layers
        set0 = dr.render_set(self.record, binding, {0: self.HEADER})
        binding.tables(dr.root_table(push, self.record['set_bytes']), set0)
        return binding

    def layer_of(self, binding: dr.Binding, instance: int) -> int:
        program = self.record['programs'][0]
        environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: 0, ATTR_INSTANCE_ID: instance}), {})
        dr.execute(program, environment)
        return environment.outputs[(0, ATTR_LAYER)]

    def lane(self, banks: dr.Stage, pixel: tuple[int, int], layer: int, loads: list) -> Inputs:
        x, y = pixel
        words = {ATTR_LAYER: layer, ATTR_POSITION: dr.f32(x + 0.5), ATTR_POSITION + 4: dr.f32(y + 0.5)}
        words.update({ATTR_POSITION + 8: dr.f32(0.0), ATTR_POSITION + 12: dr.f32(1.0)})

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        def surface(op: tuple) -> list[int]:
            if op[0] != 'suld':
                raise UnsupportedError(f'{op[0]}, a read the test gave no memory for')
            loads.append((op[1], op[2]))
            return self.texel(*op[2]) if len(op[2]) == 3 else [0, 0, 0, 0]

        return Inputs(attribute, banks.cbuf, surface)

    def test_the_vertex_program_writes_each_instances_layer_below_the_length(self):
        for layers in (2, 6):
            binding = self.binding(layers)
            for instance in range(layers + 1):
                want = instance if instance < layers else 0
                self.assertEqual(self.layer_of(binding, instance), want, f'{layers} layers, instance {instance}')

    def test_each_layers_fragments_fetch_and_store_their_own_texel(self):
        program = self.record['programs'][1]
        for layers in (2, 6):
            binding = self.binding(layers)
            banks = dr.stage(program, binding, attributes({}), {})
            for instance in range(layers):
                layer = self.layer_of(binding, instance)
                for x0, y0 in self.QUADS:
                    pixels = [(x0 + k % 2, y0 + k // 2) for k in range(4)]
                    loads: list[list] = [[] for _ in pixels]
                    lanes = [self.lane(banks, p, layer, loads[k]) for k, p in enumerate(pixels)]
                    for (x, y), loaded, (_, killed, stores) in zip(
                        pixels, loads, run_quad(dr.code(program), lanes, 0), strict=True
                    ):
                        at = (x, y, layer)
                        self.assertFalse(killed, f'{layers} layers: {at}')
                        self.assertEqual(loaded, [(self.HEADER, at)], f'{layers} layers: the loads at {at}')
                        self.assertEqual([s[:2] for s in stores], [(self.HEADER, at)], f'{layers} layers: {at}')
                        stored = [round(dr.value(w) * 255) for w in stores[0][2]]
                        r, g, b, a = self.codes(*at)
                        self.assertEqual(stored, [g, b, r, a], f'{layers} layers: the codes stored at {at}')


class WideStride(Records):
    """Layout strides past SET_VERTEX_STREAM_A_FORMAT's 12 bits.

    A static layout of 2052 and one of 4100 bytes are pulled in the shader, the tint's 16-byte layout is the record's
    one fixed-function stream at location 0, and a dynamic layout's stride twin pulls it at its bind's stride from the
    twin's VertexBufferStride word.
    """

    VERTICES = 4

    @staticmethod
    def laid(values: list[tuple], stride: int) -> bytes:
        out = bytearray(stride * len(values))
        for k, v in enumerate(values):
            struct.pack_into(f'<{len(v)}f', out, k * stride, *v)
        return bytes(out)

    @staticmethod
    def blob(record: dict) -> dict[int, int]:
        words = record['blob']
        return dict(zip(words[0::2], words[1::2], strict=True))

    def test_a_static_stride_past_12_bits_is_pulled_and_the_fixed_function_attribute_keeps_location_0(self):
        record = self.state('wide_static')
        self.assertEqual(record['vertex_slot'], 3)
        self.assertEqual(record['vertex_stride'], 16)
        self.assertEqual(record['vertex_streams'], [0] * 7)
        registers = self.blob(record)
        self.assertEqual(registers.get(0x1C00, 0) & 0xFFF, 16)
        self.assertEqual(registers.get(0x1160, 0) & 0x1F, 0)
        self.assertEqual(registers.get(0x1164, 0) & (1 << 6), 1 << 6)
        positions = [(float(k), float(-2 * k)) for k in range(self.VERTICES)]
        colours = [(0.5 * k, 1.0, 2.0 * k, 4.0) for k in range(self.VERTICES)]
        tints = [(2.0, 0.5, 1.0, float(k + 1)) for k in range(self.VERTICES)]
        binding = dr.Binding()
        binding.bind((1, 1), self.laid(positions, 2052))
        binding.bind((1, 2), self.laid(colours, 4100))
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        program = record['programs'][0]
        for vid in range(self.VERTICES):
            words = {ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}
            words.update({ATTR_GENERIC + 4 * c: dr.f32(tints[vid][c]) for c in range(4)})
            environment = dr.stage(program, binding, attributes(words), {})
            dr.execute(program, environment)
            self.assertEqual(outputs(environment, ATTR_POSITION, 4), [*positions[vid], 0.0, 1.0])
            self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [colours[vid][c] * tints[vid][c] for c in range(4)])

    def test_a_dynamic_strides_twin_pulls_the_layout_at_its_bind_stride(self):
        record = self.state('wide_dynamic')
        self.assertEqual(record['vertex_stride_dynamic'], 1)
        self.assertEqual(record['vertex_slot'], 1)
        self.assertIn('stride_twin', record)
        twin = record['stride_twin']
        self.assertEqual(twin['vertex_stride_dynamic'], 0)
        self.assertEqual(twin['vertex_slot'], 0xFF)
        pulled = [p for p in twin['pulled_stride'] if p[1]]
        self.assertEqual([p[0] for p in pulled], [1])
        program = twin['programs'][0]
        for stride in (4112, 8192 + 4):
            positions = [(float(3 * k + 1), float(k) * 0.25) for k in range(self.VERTICES)]
            binding = dr.Binding()
            binding.bind((1, 1), self.laid(positions, stride))
            binding.bind((1, 2), dr.floats(0.25, 0.5, 0.75, 1.0))
            push = dr.record_push(twin)
            push[pulled[0][1] - 1] = stride
            binding.tables(dr.root_table(push, twin['set_bytes']), dr.render_set(twin, binding))
            for vid in range(self.VERTICES):
                environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}), {})
                dr.execute(program, environment)
                self.assertEqual(outputs(environment, ATTR_POSITION, 4), [*positions[vid], 0.0, 1.0], f'{stride}')
                self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [0.25, 0.5, 0.75, 1.0])


class Interpolants(Records):
    """Run 32 float4 varyings, GM20B's 128 interpolated components.

    The vertex program stores each at its generic attribute, and the fragment program interpolates all of them.
    """

    COUNT = 32

    @classmethod
    def varying(cls, i: int, vid: int) -> list[float]:
        return [float(i), float(vid), 0.5 * i, 1.0 + i]

    def test_the_vertex_program_writes_every_generic_attribute(self):
        record = self.state('interpolants')
        program = record['programs'][0]
        binding = dr.Binding()
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        for vid in (0, 3):
            environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}), {})
            dr.execute(program, environment)
            for i in range(self.COUNT):
                self.assertEqual(outputs(environment, ATTR_GENERIC + 16 * i, 4), self.varying(i, vid), f'{vid} v{i}')

    def test_the_fragment_program_reads_every_generic_attribute(self):
        record = self.state('interpolants')
        program = record['programs'][1]
        binding = dr.Binding()
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        banks = dr.stage(program, binding, attributes({}), {})
        vid = 3
        words = {ATTR_POSITION + 4 * c: dr.f32(v) for c, v in enumerate((0.5, 0.5, 0.0, 1.0))}
        for i in range(self.COUNT):
            for c, v in enumerate(self.varying(i, vid)):
                words[ATTR_GENERIC + 16 * i + 4 * c] = dr.f32(v)

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        lanes = [Inputs(attribute, banks.cbuf, lambda _: [0, 0, 0, 0]) for _ in range(4)]
        want = [sum(self.varying(i, vid)[c] * (i + 1) for i in range(self.COUNT)) for c in range(4)]
        for colour, killed, _ in run_quad(dr.code(program), lanes, 4):
            self.assertFalse(killed)
            self.assertEqual([dr.value(w) for w in colour], want)


class ArgumentBuffers(Records):
    """Read a fragment function's textures and sampler by ID in a tier-2 argument buffer.

    They are read through the device's handle tables (raw/runtime/raw_heap_tables.h, as raw/test/argcheck.py models
    them for the pack's records). The draw's root table holds the handle tables' root and ExecutionStatus words, set 0
    the argument buffer and the texture and sampler tables, set 1 header i at element i of each array; an ID is
    1 << 32 | TIC or 1 << 32 | TSC << 20.
    A fetch answers its handle, so each check is the texture and sampler headers the fetches name: the sample through
    the ID's sampler, and every fetch of the ID's texture.
    """

    TICS, TSCS, TAG = 8192, 1024, 1 << 32
    ROOTS, STATUS, TABLES, HEAP, SET0, ARG = 0x2000000, 0x2000100, 0x3000000, 0x4000000, 0x5000000, 0x6000000
    ENTRY_UBO, ENTRY_TEXTURE_TABLE, ENTRY_SAMPLER_TABLE = 1, 8, 9
    CBUF_UBO, CBUF_HEAP_SET = 5, 3 | 1 << 3

    def memory(self, record: dict, textures: dict[int, int], samplers: dict[int, int]) -> dict[int, int]:
        """Give the device's memory once the IDs of `textures` and `samplers` (header: entry flags) were asked for."""
        words: dict[int, int] = {}

        def put(va: int, data: bytes) -> None:
            for i in range(0, len(data), 4):
                words[va + i] = struct.unpack_from('<I', data, i)[0]

        sink, identity = self.ROOTS + 96, struct.pack('<4Q', 0x1000, 0x1000, (1 << 39) - 0x1000, 0)
        put(self.ROOTS, bytes(32) + struct.pack('<4Q', sink, 2, 1, 0) + identity)
        for i in range(1, self.TICS):
            put(self.TABLES + 16 * i, struct.pack('<4I', i, 1, i if i in textures else 0, textures.get(i, 0)))
        for i in range(1, self.TSCS):
            entry = struct.pack('<4I', i << 20, 1, i if i in samplers else 0, samplers.get(i, 0))
            put(self.TABLES + 16 * self.TICS + 16 * i, entry)
        for heap_class, offset in record['arguments']['heap']:
            for header in samplers if heap_class & 3 == 2 else textures:
                put(self.HEAP + offset + 4 * header, struct.pack('<I', header << 20 if heap_class & 3 == 2 else header))
        return words

    def fetches(self, record: dict, words: dict[int, int], pixel: tuple[int, int]) -> list[tuple[int, int]]:
        """Run the fragment program at `pixel`; give the (texture, sampler) header of each fetch."""
        program = record['programs'][1]
        fetched: list[tuple[int, int]] = []
        cbuf = self.constant_buffers(record, program, words)
        texture = self.fetch_answers(words, fetched)

        position = {ATTR_POSITION: pixel[0] + 0.5, ATTR_POSITION + 4: pixel[1] + 0.5}
        position.update({ATTR_POSITION + 8: 0.5, ATTR_POSITION + 12: 1.0})

        def attribute(address: int) -> int:
            if address not in position:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return dr.f32(position[address])

        lanes = [Inputs(attribute, cbuf, texture, -1) for _ in range(4)]
        run_quad(dr.code(program), lanes, 4)
        return fetched

    def root_push(self, record: dict) -> dict[int, int]:
        """Give the draw's push words with the handle tables' root and ExecutionStatus addresses set."""
        arguments = record['arguments']
        push = dr.record_push(record)
        for stage in range(2):
            for word, va in (
                (arguments['root_table_word'][stage], self.ROOTS),
                (arguments['status_word'][stage], self.STATUS),
            ):
                if word:
                    push[word - 1], push[word] = va & 0xFFFFFFFF, va >> 32
        return push

    def set_zero(self, record: dict) -> dict[int, list[int]]:
        """Give set 0: the argument buffer and the texture and sampler tables, by the offset the entries name."""
        set0 = {}
        for offset, kind, _stage, slot, *_ in record['entries']:
            if kind == self.ENTRY_UBO and slot == 0:
                set0[offset] = [self.ARG, 0, 256, 0]
            elif kind == self.ENTRY_TEXTURE_TABLE:
                set0[offset] = [self.TABLES, 0, 16 * self.TICS, 0]
            elif kind == self.ENTRY_SAMPLER_TABLE:
                set0[offset] = [self.TABLES + 16 * self.TICS, 0, 16 * self.TSCS, 0]
        return set0

    def constant_buffers(self, record: dict, program: dict, words: dict[int, int]) -> Callable[[int, int], int]:
        """Give the fragment program's constant buffer reads over the device's memory `words`."""
        arguments = record['arguments']
        push = self.root_push(record)
        set0 = self.set_zero(record)

        def cbuf(bank: int, offset: int) -> int:
            kind = program['cbufs'][bank] if bank < len(program['cbufs']) else 0
            if kind == dr.CBUF_ROOT:
                if dr.ROOT_PUSH <= offset < dr.ROOT_PUSH + 128:
                    return push.get((offset - dr.ROOT_PUSH) // 4, 0)
                sets = {dr.ROOT_SETS: [self.SET0, 0, 256, 0], 0x48: [self.HEAP, 0, arguments['heap_bytes'], 0]}
                return sets.get(offset & ~15, [0] * 4)[(offset & 15) // 4]
            if kind == dr.CBUF_SET:
                return set0.get(offset & ~15, [0] * 4)[(offset & 15) // 4]
            if kind == self.CBUF_HEAP_SET:
                return words.get(self.HEAP + offset, 0)
            if kind & 7 == self.CBUF_UBO:
                base = set0.get(kind >> 3 << 4, [self.ARG, 0])
                return words.get((base[0] | base[1] << 32) + offset, 0)
            raise UnsupportedError(f'a constant buffer of type {kind} at bank {bank}')

        return cbuf

    @staticmethod
    def fetch_answers(words: dict[int, int], fetched: list[tuple[int, int]]) -> Callable[[tuple], list[int]]:
        """Give the fetch answers: a global load reads `words`; a texture fetch is appended to `fetched`."""

        def texture(key: tuple) -> list[int]:
            if key[0] == 'ldg':
                (lo, hi), off = key[1], key[2]
                va = (lo | hi << 32) + (off - (1 << 24) if off & 1 << 23 else off)
                return [words.get(va + 4 * k, 0) for k in range(4)]
            if key[0] in ('tex', 'tld'):
                fetched.append((key[1] & 0xFFFFF, key[1] >> 20))
                return [dr.f32(0.5)] * 4
            if key[0] == 'txq':
                return [4, 4, 1, 1]
            raise UnsupportedError(f'a fetch {key[0]}')

        return texture

    def test_the_record_reads_the_handle_tables_and_set_1(self):
        record = self.state('argument_textures')
        kinds = {kind for _, kind, *_ in record['entries']}
        self.assertIn(self.ENTRY_TEXTURE_TABLE, kinds)
        self.assertIn(self.ENTRY_SAMPLER_TABLE, kinds)
        self.assertTrue(record['arguments']['heap'], 'set 1 holds the heap arrays')
        self.assertIn(self.CBUF_HEAP_SET, record['programs'][1]['cbufs'])

    def test_each_ids_texture_and_sampler_are_its_headers(self):
        record = self.state('argument_textures')
        for first, second, sampler in ((27, 8191, 1023), (4096, 5, 7)):
            words = self.memory(record, {first: 0, second: 1 << 16}, {sampler: 0})
            for va, word in zip(
                range(self.ARG, self.ARG + 24, 4),
                struct.unpack('<6I', struct.pack('<3Q', self.TAG | first, self.TAG | second, self.TAG | sampler << 20)),
                strict=True,
            ):
                words[va] = word
            for x, tic in ((1, first), (6, second)):
                fetched = self.fetches(record, words, (x, 4))
                self.assertIn((tic, sampler), fetched, f'x {x}')
                self.assertEqual({t for t, _ in fetched}, {tic}, f'x {x}')

    def test_a_texture_no_id_names_reads_header_0(self):
        record = self.state('argument_textures')
        words = self.memory(record, {}, {1023: 0})
        for va, word in zip(
            range(self.ARG, self.ARG + 24, 4),
            struct.unpack('<6I', struct.pack('<3Q', self.TAG | 27, self.TAG | 8191, self.TAG | 1023 << 20)),
            strict=True,
        ):
            words[va] = word
        fetched = self.fetches(record, words, (1, 4))
        self.assertTrue(fetched)
        self.assertTrue(all(t == 0 for t, _ in fetched), fetched)


# QMDV01_07's SHARED_MEMORY_SIZE MW(561:544) and L1_CONFIGURATION MW(671:669), whose 3 is 48 KiB directly addressable
# (clb1c0qmd.h)
QMD_SHARED_MEMORY_SIZE, QMD_L1_CONFIGURATION, QMD_L1_48KB = (544, 18), (669, 3), 3


def qmd_field(qmd: list[int], field: tuple[int, int]) -> int:
    first, width = field
    return sum((qmd[(first + b) // 32] >> ((first + b) % 32) & 1) << b for b in range(width))


class ThreadgroupMemory(Records):
    """A kernel of 48 KiB of threadgroup memory, every word of it used, under the QMD's own shared memory size."""

    THREADS = 256

    def run_kernel(self, record: dict, rows: int) -> list[int]:
        """Run one threadgroup of THREADS threads under exactly the shared memory its QMD sizes; give out[]."""
        size = qmd_field(record['qmd'], QMD_SHARED_MEMORY_SIZE)
        binding = dr.Binding()
        out = binding.bind((3, 0), bytes(4 * self.THREADS))
        binding.bind((3, 1), struct.pack('<I', rows))
        push = dr.record_push(record)
        if record['region_word'] >= 0:
            for k, v in enumerate([0, 0, 0, self.THREADS - 1, self.THREADS, 1, 1, 0]):
                push[record['region_word'] + k] = v
        binding.tables(dr.root_table(push, record['set_bytes'], {12: 1, 16: 1, 20: 1}), dr.kernel_set(record, binding))
        shared = bytearray(size)
        for tid in range(self.THREADS):
            system = {SV_TID_X: tid, 0x22: 0, 0x23: 0, SV_COMBINED_TID: tid, SV_LANE_ID: tid % 32}
            system.update({SV_CTAID_X: 0, 0x26: 0, 0x27: 0})
            environment = dr.stage(record, binding, attributes({}), system)
            environment.shared = shared
            dr.execute(record, environment, header=False)
        return list(struct.unpack(f'<{self.THREADS}I', binding.memory.read(out, 4 * self.THREADS)))

    def test_a_dispatchs_48_kib_takes_the_whole_cta(self):
        """The length a dispatch sets (setThreadgroupMemoryLength:atIndex:), as MPS's kernels take theirs."""
        record = self.kernel('dynamic_threadgroup')
        self.assertEqual(qmd_field(record['qmd'], QMD_SHARED_MEMORY_SIZE), 48 << 10)
        self.assertEqual(qmd_field(record['qmd'], QMD_L1_CONFIGURATION), QMD_L1_48KB)
        want = []
        for tid in range(self.THREADS):
            x = 0
            for k in range(48):
                x ^= ((tid + 256 * k) * 2654435761 + k) & 0xFFFF_FFFF
            want.append(x)
        self.assertEqual(self.run_kernel(record, 48), want)

    def test_a_dispatch_past_48_kib_is_refused_naming_the_limit(self):
        self.assertNotIn('past_dynamic_threadgroup', RECORDS['kernels'])
        self.assertIn('MaxComputeSharedMemorySize', RECORDS['refused'].get('past_dynamic_threadgroup', ''))

    def test_a_kernel_of_48_kib_takes_the_whole_cta(self):
        record = self.kernel('wide_threadgroup')
        size = qmd_field(record['qmd'], QMD_SHARED_MEMORY_SIZE)
        self.assertEqual(size, 48 << 10)
        self.assertEqual(qmd_field(record['qmd'], QMD_L1_CONFIGURATION), QMD_L1_48KB)
        binding = dr.Binding()
        out = binding.bind((3, 0), bytes(4 * self.THREADS))
        push = dr.record_push(record)
        if record['region_word'] >= 0:
            for k, v in enumerate([0, 0, 0, self.THREADS - 1, self.THREADS, 1, 1, 0]):
                push[record['region_word'] + k] = v
        binding.tables(dr.root_table(push, record['set_bytes'], {12: 1, 16: 1, 20: 1}), dr.kernel_set(record, binding))
        shared = bytearray(size)
        for tid in range(self.THREADS):
            system = {SV_TID_X: tid, 0x22: 0, 0x23: 0, SV_COMBINED_TID: tid, SV_LANE_ID: tid % 32}
            system.update({SV_CTAID_X: 0, 0x26: 0, 0x27: 0})
            environment = dr.stage(record, binding, attributes({}), system)
            environment.shared = shared
            dr.execute(record, environment, header=False)
        got = struct.unpack(f'<{self.THREADS}I', binding.memory.read(out, 4 * self.THREADS))
        want = []
        for tid in range(self.THREADS):
            x = 0
            for k in range(48):
                x ^= ((tid + 256 * k) * 2654435761 + k) & 0xFFFF_FFFF
            want.append(x)
        self.assertEqual(list(got), want)

    def test_a_kernel_past_48_kib_is_refused_naming_the_limit(self):
        self.assertNotIn('past_threadgroup', RECORDS['kernels'])
        why = RECORDS['refused'].get('past_threadgroup', '')
        self.assertIn('MaxComputeSharedMemorySize', why)
        self.assertIn('available: 49152', why)


class ShadowStencil(Records):
    """A fragment function's [[stencil]] output: m2v's shader-side stencil (raw_shader_test_push.h shadowStencil).

    GM20B's fragment program header has no stencil output. The record binds the stencil stand-in alone and carries the
    ShadowStencilState word in PackShaderTests::reserved. On quads of the front and back face, the program reads the
    stand-in's texel at its pixel (SULD), tests the output's low 8 bits against it under the face's compare function and
    read mask, stores the stencil-failure or pass operation's value under the write mask (SUST), and kills a fragment
    that fails, as Metal's stencil test does (the operations' values as m2v measured them, computed here on the CPU). In
    a pass without a stencil the record binds none and the program reads and writes nothing.
    """

    HEADER = 0x2A7
    QUADS = ((2, 4), (31, 9), (40, 1))
    FACES = (
        (3, 1, 2, 0xFF, 0xFF),  # LessEqual, Zero, Replace
        (5, 6, 7, 0x3C, 0xF0),  # NotEqual, IncrementWrap, DecrementWrap
        (6, 4, 3, 0x0F, 0x33),  # GreaterEqual, DecrementClamp, IncrementClamp
        (2, 5, 1, 0xF0, 0x0F),  # Equal, Invert, Zero
        (0, 2, 2, 0xFF, 0xFF),  # Never
    )

    @staticmethod
    def word(face: tuple, attached: bool) -> int:
        compare, fail, passed, read, write = face
        return compare | fail << 3 | passed << 6 | read << 9 | write << 17 | (1 << 31 if attached else 0)

    @staticmethod
    def stored(x: int, y: int) -> int:
        return (13 * x + 5 * y + 40) & 255

    @staticmethod
    def compare(code: int, a: int, b: int) -> bool:
        """Give Metal's compare function `code` of `a` against `b`."""
        return (False, a < b, a == b, a <= b, a > b, a != b, a >= b, True)[code]

    @staticmethod
    def operation(op: int, stored: int, reference: int, write: int) -> int:
        """Give the value Metal's stencil operation `op` leaves under write mask `write`."""
        value = (
            stored,
            0,
            reference,
            min(stored + 1, 255),
            max(stored - 1, 0),
            stored ^ 255,
            (stored + 1) & 255,
            (stored - 1) & 255,
        )[op]
        return (stored & ~write & 255) | (value & write)

    @classmethod
    def metal(cls, face: tuple, reference: int, stored: int) -> tuple[bool, int]:
        """Give whether Metal's stencil test passes and the value it leaves."""
        compare, fail, passed, read, write = face
        ok = cls.compare(compare, reference & read, stored & read)
        return ok, cls.operation(passed if ok else fail, stored, reference, write)

    def setUp(self) -> None:
        self.record = self.state('shadow_stencil')
        _depth, _stencil, self.flags, self.word_at, _depth_tic, self.stencil_tic = self.record['shader_tests'][:6]

    def test_the_record_binds_the_stencil_stand_in_alone_and_names_its_word(self):
        self.assertEqual(self.flags, 2, 'the stencil stand-in, no depth stand-in')
        self.assertTrue(0 < self.word_at < 32, 'ShadowStencilState in the push window')
        self.assertNotEqual(self.stencil_tic, 0, 'the stand-in R8Uint storage view')
        self.assertEqual([e[1] for e in self.record['entries'] if e[1] in (6, 7)], [7])
        unattached = self.state('shadow_unattached')
        self.assertEqual(unattached['shader_tests'][2], 0, 'a pass without a stencil: no stand-in')
        self.assertNotEqual(unattached['shader_tests'][3], 0, 'its words still say nothing is attached')

    def run_faces(self, record: dict, attached: bool) -> None:
        program = record['programs'][1]
        word_at = record['shader_tests'][3]
        for face in self.FACES:
            for front in (True, False):
                push = dr.record_push(record)
                other = (7, 0, 0, 0xFF, 0xFF)
                push[word_at - 1] = self.word(face if front else other, attached)
                push[word_at] = self.word(other if front else face, attached)
                binding = dr.Binding()
                stand_ins = {dr.ENTRY_STENCIL_TEST: self.HEADER}
                binding.tables(
                    dr.root_table(push, record['set_bytes']), dr.render_set(record, binding, stand_ins=stand_ins)
                )
                banks = dr.stage(program, binding, attributes({}), {})
                for x0, y0 in self.QUADS:
                    pixels = [(x0 + k % 2, y0 + k // 2) for k in range(4)]
                    loads: list[list] = [[] for _ in pixels]
                    lanes = [self.lane(banks, p, front, loads[k]) for k, p in enumerate(pixels)]
                    for (x, y), loaded, (_, killed, stores) in zip(
                        pixels, loads, run_quad(dr.code(program), lanes, 0), strict=True
                    ):
                        at = f'face {face} front={front} at {(x, y)}'
                        if not attached:
                            self.assertEqual((loaded, stores, killed), ([], [], False), at)
                            continue
                        ok, value = self.metal(face, (7 * x + 61 * y + 0x100) & 255, self.stored(x, y))
                        self.assertEqual(loaded, [(self.HEADER, (x, y))], at)
                        self.assertEqual(killed, not ok, at)
                        self.assertEqual([(h, c, v[0] & 255) for h, c, v in stores], [(self.HEADER, (x, y), value)], at)

    def lane(self, banks: dr.Stage, pixel: tuple[int, int], front: bool, loads: list) -> Inputs:
        x, y = pixel
        words = {
            ATTR_POSITION: dr.f32(x + 0.5),
            ATTR_POSITION + 4: dr.f32(y + 0.5),
            ATTR_FRONT_FACE: MASK if front else 0,
        }
        words.update({ATTR_POSITION + 8: dr.f32(0.0), ATTR_POSITION + 12: dr.f32(1.0)})

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        def surface(op: tuple) -> list[int]:
            if op[0] != 'suld':
                raise UnsupportedError(f'{op[0]}, a read the test gave no memory for')
            loads.append((op[1], op[2]))
            return [self.stored(*op[2][:2]), 0, 0, 0]

        return Inputs(attribute, banks.cbuf, surface)

    def test_each_fragment_runs_metals_stencil_test_on_the_stand_in(self):
        self.run_faces(self.record, True)

    def test_a_pass_without_a_stencil_tests_nothing(self):
        self.run_faces(self.state('shadow_unattached'), False)


class ShadowStencilTested(Records):
    """The same function's " | tests=4" variant, for a draw whose stencil write follows its depth test.

    The shader-side stencil writes before the hardware's depth test, which picks between a face's depth-failure and pass
    operations: this variant runs both tests on the depth and stencil stand-ins, the output's low 8 bits the stencil
    test's reference. On quads at depths 0.25 and 0.75 over stand-in depth 0.5 (Less, depth writes), each fragment's
    stencil stand-in texel is the face's stencil-failure, depth-failure or pass operation's value, its depth written
    where both tests pass, and a fragment that fails either is killed, as Metal's tests do.
    """

    DEPTH, STENCIL = 0x2B1, 0x2B2
    QUADS = ((2, 4), (31, 9))
    FRONT = (3, 1, 3, 2, 0xFF, 0xFF)  # LessEqual; Zero, IncrementClamp on depth failure, Replace
    BACK = (6, 6, 0, 7, 0x3C, 0xF0)  # GreaterEqual; IncrementWrap, Keep on depth failure, DecrementWrap

    @staticmethod
    def word(face: tuple) -> int:
        compare, fail, depth_fail, passed, read, write = face
        return compare | fail << 3 | passed << 6 | read << 9 | write << 17 | depth_fail << 25

    def setUp(self) -> None:
        self.record = self.state('shadow_tested')
        self.depth_word, self.stencil_word, self.flags, shadow_word = self.record['shader_tests'][:4]
        self.storage_tests = self.record['shader_tests'][6]
        self.assertEqual(shadow_word, 0, 'no shader-side stencil in the variant')

    def test_the_record_binds_both_stand_ins_for_stencil_writing_draws(self):
        self.assertEqual(self.flags, 3)
        self.assertEqual(self.storage_tests, 4, 'its draws are StencilWrite (raw_fragment_tests.h)')
        self.assertEqual(sorted(e[1] for e in self.record['entries'] if e[1] in (6, 7)), [6, 7])

    def test_each_fragment_runs_metals_stencil_then_depth_test(self):
        program = self.record['programs'][1]
        for front in (True, False):
            push = dr.record_push(self.record)
            push.update({self.depth_word - 1 + k: w for k, w in enumerate((1 | 1 << 3, dr.f32(0.0), dr.f32(1.0), 0))})
            words = (self.word(self.FRONT), self.word(self.BACK), 0, 0)
            push.update({self.stencil_word - 1 + k: w for k, w in enumerate(words)})
            binding = dr.Binding()
            stand_ins = {dr.ENTRY_DEPTH_TEST: self.DEPTH, dr.ENTRY_STENCIL_TEST: self.STENCIL}
            binding.tables(
                dr.root_table(push, self.record['set_bytes']), dr.render_set(self.record, binding, stand_ins=stand_ins)
            )
            banks = dr.stage(program, binding, attributes({}), {})
            for x0, y0 in self.QUADS:
                pixels = [(x0 + k % 2, y0 + k // 2) for k in range(4)]
                depths = [0.25 if (x + y) % 2 else 0.75 for x, y in pixels]
                lanes = [self.lane(banks, p, z, front) for p, z in zip(pixels, depths, strict=True)]
                for (x, y), z, (_, killed, stores) in zip(
                    pixels, depths, run_quad(dr.code(program), lanes, 0), strict=True
                ):
                    compare, fail, depth_fail, passed, read, write = self.FRONT if front else self.BACK
                    reference, stored = (7 * x + 61 * y + 0x100) & 255, ShadowStencil.stored(x, y)
                    stencil_ok = ShadowStencil.compare(compare, reference & read, stored & read)
                    depth_ok = stencil_ok and z < 0.5
                    op = passed if depth_ok else depth_fail if stencil_ok else fail
                    value = ShadowStencil.operation(op, stored, reference, write)
                    at = f'front={front} at {(x, y)} z={z}'
                    self.assertEqual(killed, not depth_ok, at)
                    self.assertEqual(
                        [(c, v[0] & 255) for h, c, v in stores if h == self.STENCIL], [((x, y), value)], at
                    )
                    depth_stores = [(c, v[0]) for h, c, v in stores if h == self.DEPTH]
                    self.assertEqual(depth_stores, [((x, y), dr.f32(z))] if depth_ok else [], at)

    def lane(self, banks: dr.Stage, pixel: tuple[int, int], z: float, front: bool) -> Inputs:
        x, y = pixel
        words = {
            ATTR_POSITION: dr.f32(x + 0.5),
            ATTR_POSITION + 4: dr.f32(y + 0.5),
            ATTR_POSITION + 8: dr.f32(z),
            ATTR_POSITION + 12: dr.f32(1.0),
            ATTR_FRONT_FACE: MASK if front else 0,
        }

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        def surface(op: tuple) -> list[int]:
            if op[0] != 'suld':
                raise UnsupportedError(f'{op[0]}, a read the test gave no memory for')
            header, at = op[1], op[2]
            if header == self.DEPTH:
                return [dr.f32(0.5), 0, 0, 0]
            return [ShadowStencil.stored(*at[:2]), 0, 0, 0]

        return Inputs(attribute, banks.cbuf, surface)


class Kernels(Records):
    MASK = 0xFFFF_FFFF_FFFF_FFFF
    ERROR = MASK  # raw_counters.h kErrorValue
    # nanoseconds a PTIMER unit: 32 units a 19.2 MHz tick
    NUM, DEN = 625, 384

    def test_a_binary_function_links_under_its_specialized_name(self):
        record = self.kernel('binary_affine')
        binding = dr.Binding()
        out = binding.bind((3, 0), bytes(128))
        self.dispatch(record, binding, 1, 32)
        self.assertEqual(struct.unpack('<32I', binding.memory.read(out, 128)), tuple(2 * i + 1 for i in range(32)))
        self.assertNotIn('binary_wrong_alias', RECORDS['kernels'])
        self.assertIn('binary_affine_alias', RECORDS['refused'].get('binary_wrong_alias', ''))
        self.assertIn('binary_affine', RECORDS['answers'], RECORDS['refused'].get('binary_affine', ''))
        answers = RECORDS['answers']['binary_affine']
        self.assertGreaterEqual(answers['facts'][0], 32)
        self.assertEqual(answers['facts'][1:], [32, 0])
        reflection = bytes.fromhex(answers['reflection'])
        self.assertEqual(struct.unpack_from('<4sII', reflection), (b'SWRF', 1, 1))
        nodes = struct.unpack_from('<I', reflection, 12)[0]
        first, count = struct.unpack_from('<II', reflection, 24)
        self.assertEqual(count, 1)
        binding = struct.unpack_from('<I', reflection, 32 + nodes * 104 + first * 4)[0]
        self.assertLess(binding, nodes)
        self.assertEqual(struct.unpack_from('<I', reflection, 32 + binding * 104)[0], 1)
        self.assertEqual(struct.unpack_from('<Q', reflection, 32 + binding * 104 + 56)[0], 0)

    @staticmethod
    def dispatch(record: dict, binding: dr.Binding, groups: int, local: int, stage_in: list[int] | None = None) -> None:
        """Run every thread of a dispatchThreadgroups: of `groups` threadgroups of `local` threads on x.

        The root table's group count (nvk_root_descriptor_table cs.group_count) and the record's DispatchRegion words as
        raw_compute.cpp writes them (raw_dispatch_regions.h computeDispatch, computeRegionWords: base 0, the packed
        threadgroup size less 1, the grid's threads); each thread's TID, CTAID and lane in its warp of 32.
        """
        push = dr.record_push(record)
        if stage_in is not None:
            word = record['stage_in_region_word']
            for k, v in enumerate(stage_in):
                push[word - 1 + k] = v
        if record['region_word'] >= 0:
            for k, v in enumerate([0, 0, 0, local - 1, groups * local, 1, 1, 0]):
                push[record['region_word'] + k] = v
        root = dr.root_table(push, record['set_bytes'], {12: groups, 16: 1, 20: 1})
        binding.tables(root, dr.kernel_set(record, binding))
        for group in range(groups):
            for tid in range(local):
                system = {SV_TID_X: tid, 0x22: 0, 0x23: 0, SV_COMBINED_TID: tid, SV_LANE_ID: tid % 32}
                system.update({SV_CTAID_X: group, 0x26: 0, 0x27: 0})
                dr.execute(record, dr.stage(record, binding, attributes({}), system), header=False)

    def test_a_stage_in_kernel_reads_its_region_from_its_origin(self):
        record = self.kernel('stage_in')
        self.assertGreater(record['stage_in_region_word'], 0)
        origin, size, stride = 3, 8, 12
        values = [(float(k), 0.5 * k - 1.0) for k in range(origin + size)]
        source = bytearray(stride * len(values))
        for k, v in enumerate(values):
            struct.pack_into('<2f', source, k * stride, *v)
        binding = dr.Binding()
        binding.bind((3, 0), bytes(source))
        out = binding.bind((3, 1), bytes(4 * size))
        self.dispatch(record, binding, 1, size, [origin, 0, 0, 0, size, 1, 1, 0])
        written = struct.unpack(f'<{size}f', binding.memory.read(out, 4 * size))
        expected = [values[origin + g][0] * 2.0 + values[origin + g][1] for g in range(size)]
        self.assertEqual(list(written), expected)

    def resolve(self, records: list[tuple[int, int]], published: int, slots: dict[int, tuple[int, int]]) -> list[int]:
        """Give the counter resolve kernel's samples of `records`, each (payload, units).

        The kernel runs beside a clock anchor table of `published` anchors, `slots` its (units, ns) by slot, bound as
        RawMetalCounterEncode.mm binds them.
        """
        layout = RECORDS['layouts']['counter']
        self.assertEqual(layout['sample_bytes'], 8, 'a sample is a 64-bit time')
        record = self.kernel('counter_resolve')
        count = len(records)
        binding = dr.Binding()
        binding.bind((3, 0), dr.counter_records(layout, records))
        out = binding.bind((3, 1), bytes(8 * count))
        binding.bind((3, 2), dr.resolve_params(layout, count))
        binding.bind((3, 3), dr.clock_anchors(layout, published, slots))
        self.dispatch(record, binding, (count + 63) // 64, 64)
        return list(struct.unpack(f'<{count}Q', binding.memory.read(out, 8 * count)))

    def test_the_counter_resolve_kernel_gives_nanoseconds(self):
        """A sample at or past the table's one anchor resolves with it, and one before it to the error value."""
        anchor_units, anchor_ns = 0x0000_0312_FFFF_FF00, 0x0000_0003_FFFF_FFF0
        edges = [0, 5, 383, 384, 0x1_0000_0005, 0xFFFF_FFFF, -1, -384, -0x2_0000_0001, 0x7FFF_0000_0000]
        spans = edges + [(k * 0x1_3579_BDF1 * (-1) ** k) % (1 << 40) - (1 << 39) for k in range(70)]
        self.assertTrue(all(anchor_units + s >= 0 for s in spans), 'no span wraps below 0')
        records = [(1, anchor_units + s) for s in spans] + [(0, 0x1234)]
        self.assertGreater(len(records), 64)
        got = self.resolve(records, 1, {0: (anchor_units, anchor_ns)})
        expected = [(anchor_ns + s * self.NUM // self.DEN) & self.MASK if s >= 0 else self.ERROR for s in spans]
        self.assertEqual(got, [*expected, 0])

    def test_the_counter_resolve_kernel_of_no_anchor_gives_the_error_value(self):
        self.assertEqual(self.resolve([(1, 77), (0, 77)], 0, {}), [self.ERROR, 0])

    def test_the_counter_resolve_kernel_takes_each_samples_newest_kept_anchor(self):
        """A table of 65 anchors past its ring's slots: anchor n at slot n % slots, the newest slots - 1 kept (3 to 64).

        A sample past anchor n and below anchor n + 1 resolves with anchor n; one of anchor 2's, whose slot the next
        power-up writes, or before it, to the error value.
        """
        slots = RECORDS['layouts']['counter']['anchors']['slots']
        published, step = 65, 0x3_0000_0007
        units = {n: 0xFFFF_FF00 + n * step for n in range(published)}
        ns = {n: 0x3_FFFF_FFF0 + n * 0x2_8000_0011 for n in range(published)}
        table = {n % slots: (units[n], ns[n]) for n in range(published - slots, published)}
        kept = range(published - (slots - 1), published)
        samples = [(n, n * 0x9E37_79B9 % step) for n in kept] + [(kept[0], 0), (kept[-1], 0), (40, step - 1), (63, 384)]
        records = [(1, units[n] + offset) for n, offset in samples]
        records += [(1, units[2] + 5), (1, units[3] - 1), (1, units[2] - 1), (0, units[10])]
        self.assertGreater(len(records), 64)
        expected = [(ns[n] + offset * self.NUM // self.DEN) & self.MASK for n, offset in samples]
        self.assertEqual(self.resolve(records, published, table), [*expected, self.ERROR, self.ERROR, self.ERROR, 0])


if __name__ == '__main__':
    RECORDS.update(dr.load(Path(sys.argv[1])))
    unittest.main(argv=sys.argv[:1])
