"""Controlled Metal buffer contents for the five 24B119 comparison draws."""

import random
import re
import struct
from collections.abc import Iterator

ABSENT = 0xFFFFFFFF
SEEDS = (0x312B, 0xABA1, 0xFF03, 0x4011)
HALF = (0, 0x8000, 1, 0x83FF, 0x0400, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7E01)
FLOAT = (0, 0x80000000, 1, 0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC01234)


Node = tuple[int, ...]


class Reflection:
    def __init__(self, data: bytes) -> None:
        magic, version, stages, nodes, edges, names = struct.unpack_from('<4s5I', data)
        if (magic, version, stages) != (b'SWRF', 1, 5) or len(data) != 64 + 104 * nodes + 4 * edges + names:
            raise ValueError('unexpected Metal reflection')
        self.nodes = list(struct.iter_unpack('<10I8Q', data[64 : 64 + nodes * 104]))
        self.edges = struct.unpack_from(f'<{edges}I', data, 64 + nodes * 104)
        self.names = data[len(data) - names :]
        self.buffers = {}
        for stage in (1, 2):
            first, count = struct.unpack_from('<II', data, 24 + 8 * (stage - 1))
            for index in self.edges[first : first + count]:
                node = self.nodes[index]
                if node[10] == 0:
                    self.buffers[stage, node[12]] = node

    def name(self, node: Node) -> str:
        return self.names[node[2] - 1 :].split(b'\0')[0].decode() if node[2] else ''

    def members(self, node: Node) -> dict[str, Node]:
        if node[5] == ABSENT:
            return {}
        block = self.nodes[node[5]]
        return {self.name(m): m for m in (self.nodes[i] for i in self.edges[block[3] : block[3] + block[4]])}

    def float_fields(self, at: int, kind: int, node: Node) -> Iterator[tuple[int, bool]]:
        if node[7] != ABSENT or (node[8] != ABSENT and (node[0] != 1 or self.nodes[node[8]][1] & 1)):
            raise ValueError('argument-buffer handles require device relocation')
        if kind == 1:
            for m in self.members(node).values():
                yield from self.float_fields(at + m[11], m[10], m)
        elif kind == 2:
            a = self.nodes[node[6]]
            for i in range(a[11]):
                yield from self.float_fields(at + i * a[12], a[10], a)
        elif 3 <= kind <= 6 or 16 <= kind <= 19:
            half = kind >= 16
            for lane in range(kind - (16 if half else 3) + 1):
                offset = at + lane * (2 if half else 4)
                yield offset, half


def put_float(data: bytearray, offset: int, half: bool, rng: random.Random, ordinal: int) -> None:
    fmt, edges = ('H', HALF) if half else ('I', FLOAT)
    if ordinal == 0:
        struct.pack_into('<' + ('e' if half else 'f'), data, offset, rng.uniform(-1, 1))
    elif rng.randrange(3) == 0:
        struct.pack_into('<' + fmt, data, offset, rng.choice(edges))
    else:
        struct.pack_into('<' + fmt, data, offset, rng.getrandbits(16 if half else 32))


def vertices(data: bytearray, case: str, corpus: str, node: Node, ordinal: int) -> None:
    slot, reflected_stride = node[12], node[16]
    layouts = re.findall(r'a(\d+)=(\d+):(\d+):(\d+):(\d+):(\d+):(\d+)', corpus.partition(' | ')[0])
    for vertex, (x, y) in enumerate(((0, 0), (1, 0), (0, 1))):
        if case == 'compositor':
            values = [
                (31, '4f', (4 * x - 1, 4 * y - 1, 0.5, 1)),
                (29, '2f', (2 * x, 2 * y)),
                (29, '2f', (2 * x, 2 * y)),
                (27, '4e', (0.25, 0.5, 0.75, 1)),
            ]
        elif case.startswith('ae6d'):
            values = [(29, '2f', (x, y)), (29, '2f', (2 * x, 2 * y))]
        elif case.startswith('357f'):
            values = [(19, '2H', (65535 * x, 65535 * y)), (12, '4b', (0, 0, (-64, 32, 95)[vertex], 127))]
        elif case.startswith('3f9f'):
            values = [
                (19, '2H', (65535 * x, 65535 * y)),
                (22, '2h', (0, 0)),
                (22, '2h', (32767 * x, 32767 * y)),
                (28, 'f', (0,)),
                (28, 'f', (2 * y + x + 0.125 * ordinal,)),
            ]
        else:
            raise ValueError(f'unknown vertex layout {case}')
        if len(layouts) != len(values):
            raise ValueError('unexpected vertex attribute count')
        for row in layouts:
            index, fmt, off, binding, stride, step, rate = map(int, row)
            expected, packing, lanes = values[index]
            if (fmt, binding, stride, step, rate) != (expected, slot, reflected_stride, 1, 1) or off + struct.calcsize(
                '<' + packing
            ) > stride:
                raise ValueError(f'vertex layout disagrees with reflection: {row}')
            struct.pack_into('<' + packing, data, vertex * stride + off, *lanes)


def color_parameters(data: bytearray, rng: random.Random, ordinal: int) -> None:
    for i in range(0, len(data), 2):
        put_float(data, i, True, rng, ordinal)
    if ordinal == 0:
        # The captured opcodes 5, 1 and 6 each advance the color-parameter pointer by seven half4s.
        for base, curve in ((0, (1.25, 1, 0, 1, 0.25)), (14, (0.8, 1, 0, 1, 0.25, 0, 0))):
            for i, coefficient in enumerate(curve):
                struct.pack_into('<4e', data, (base + i) * 8, *([coefficient] * 4))
        for i in range(4):
            struct.pack_into('<4e', data, (7 + i) * 8, *(float(j == i and i < 3) for j in range(4)))


class BufferFixture:
    def __init__(self, reflection: Reflection, node: Node, rng: random.Random, ordinal: int) -> None:
        self.reflection = reflection
        self.members = reflection.members(node)
        name = reflection.name(node)
        extent = node[16]
        if not extent:
            if name not in ('globals_buffer', 'vertex_buffer', 'shape_buffer', 'fill_buffer'):
                raise ValueError(f'unknown untyped buffer {name}')
            extent = 512 if name == 'globals_buffer' else 256
        repeats = {'colorP': 21, 'texture_matrix': 2, 'vertexBuffer.0': 3}.get(name, 1)
        self.data = bytearray(rng.randbytes(extent * repeats))
        for offset, half in reflection.float_fields(0, node[13], node):
            put_float(self.data, offset, half, rng, ordinal)

    def field(self, member: str, fmt: str, *values: float) -> None:
        struct.pack_into('<' + fmt, self.data, self.members[member][11], *values)


def compositor(name: str, data: bytearray, rng: random.Random, ordinal: int) -> None:
    if name == 'colorP':
        color_parameters(data, rng, ordinal)
    elif name == 'mvp_matrix':
        struct.pack_into('<16f', data, 0, *(float(i % 5 == 0) for i in range(16)))
    elif name == 'texture_matrix':
        struct.pack_into('<8f', data, 0, 1, 1, 0, 0, 1, 1, 0, 0)
    elif name == 'lod_bias':
        struct.pack_into('<I', data, 0, (0, 1, 0x7F800000, 0x7FC01234)[ordinal])


def tile_transform(block: BufferFixture) -> None:
    block.field('mercatorToLatCoeffs', '4f', 0, 1, 0, 0)
    block.field('mercatorScaleCoeffs', '3f', 1, 0, 0)
    member = block.members['splinePoints']
    array = block.reflection.nodes[member[6]]
    if array[11:13] != (16, 16):
        raise ValueError('unexpected spline extent or stride')
    for i in range(16):
        struct.pack_into('<4f', block.data, member[11] + i * array[12], (i // 4) / 3, (i % 4) / 3, 0.5, 1)
    block.field('earthCenterLocal', '4f', 0, 0, -1, 1)
    for m in ('tileZScale', 'worldZScale', 'groundZScale', 'zOffset', 'zCompensation'):
        block.field(m, 'f', float(m.endswith('Scale')))


def ribbon_style(block: BufferFixture, stage: int, ordinal: int) -> None:
    if stage == 1 or ordinal == 0:
        block.field('halfWidth', 'f', 1)
        block.field('distanceMultiplier', 'f', 4)
    if not ordinal:
        block.field('color', '4f', 1, 0.75, 0.25, 0.875)
        for m, value in [('dashWidth', 1), ('dashHeight', 2), ('gapHeight', 1), ('radius', 0.25)]:
            block.field(m, 'e', value)


def maps(name: str, block: BufferFixture, stage: int, ordinal: int) -> None:
    if name == 'view':
        block.field('modelViewProjectionMatrix', '16f', 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, -1, -1, 0.5, 1)
        block.field('tilePerPoint', 'f', 1)
    elif name == 'transform':
        tile_transform(block)
    elif name == 'glow':
        block.field('radius', 'f', 1 + ordinal / 4)
        block.field('opacity', 'I', (0x3F400000, 0x00000001, 0x7F800000, 0x7FC01234)[ordinal])
    elif name == 'style' and 'blendMode' in block.members:
        block.field('blendMode', 'i', (1, 7, 13, 16)[ordinal])
        if not ordinal:
            block.field('shadowColor', '4f', 0.125, 0.25, 0.375, 1)
            block.field('highlightColor', '4f', 1, 0.875, 0.75, 1)
            block.field('opacity', 'e', 0.75)
    elif name == 'style':
        ribbon_style(block, stage, ordinal)


def renderbox(name: str, block: BufferFixture, rng: random.Random, ordinal: int) -> None:
    data = block.data
    if name == 'uber':
        block.field('shader_state', '4I', 0, 0, 0xF850, 0x71)
        # Shape 3 covers the triangle. Image color 4 takes its UVs from stretch vertex color 1.
        for m, value in [
            ('vertex_geometry', 7),
            ('vertex_color', 1),
            ('fragment_shape', 3),
            ('fragment_color', (4, 1, 0, 4)[ordinal]),
            ('fragment_output', ordinal // 2),
            ('color_offset', 256),
            ('clears_accumulator', ordinal % 2),
            ('_reserved', 0),
        ]:
            block.field(m, 'H', value)
    elif name == 'globals_buffer':
        struct.pack_into('<7f', data, 0, 4, 0, -4, 4, -1, -1, 2.0**31)
        if ordinal in (0, 3):
            struct.pack_into('<6f4e', data, 256, 0.5, 0, 0, 0.5, 0.5, 0.5, 0, 0, 1, 1)
        else:
            for j in range(6):
                struct.pack_into('<f', data, 256 + 4 * j, rng.uniform(0.1, 0.9))
            for j in range(4):
                put_float(data, 256 + 2 * j, True, rng, ordinal)


def fixture(
    reflection: Reflection, active: set[tuple[int, int]], case: str, corpus: str, ordinal: int
) -> list[tuple[int, int, int, bytes]]:
    result = []
    for stage, slot in sorted(active):
        node = reflection.buffers[stage, slot]
        name = reflection.name(node)
        shared = name in ('globals_buffer', 'uber')
        rng = random.Random((SEEDS[ordinal] << 16) | (0 if shared else stage) << 8 | slot)
        block = BufferFixture(reflection, node, rng, ordinal)
        if name == 'vertexBuffer.0':
            vertices(block.data, case, corpus, node, ordinal)
        elif case == 'compositor':
            compositor(name, block.data, rng, ordinal)
        elif case == '17d6ade58fef2152':
            renderbox(name, block, rng, ordinal)
        else:
            maps(name, block, stage, ordinal)
        result.append((stage, slot, (node[15] or 16) * ordinal, bytes(block.data)))
    return result
