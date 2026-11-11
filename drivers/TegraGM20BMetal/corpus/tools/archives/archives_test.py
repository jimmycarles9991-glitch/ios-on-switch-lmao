"""The archive readers on buffers laid out as Apple's are: one AIRP render state and one pipelines.desc render state.

The expected lines are the corpus lines the device journals for the same descriptors (fields Metal defaults written at
their defaults, constants at their type's size).
"""

import struct
import unittest

from drivers.TegraGM20BMetal.corpus.tools.archives import airp, declared, desc, gpubin
from drivers.TegraGM20BMetal.corpus.tools.archives.model import Constant, Function, Pipeline
from tools.gpu.shaders.metallib import Constant as Declared


class FlatBuffer:
    """A FlatBuffers writer enough for the tests: each object after the one that points at it."""

    def __init__(self, identifier: bytes) -> None:
        self.data = bytearray(struct.pack('<I', 8) + identifier)

    def _place(self, value: object) -> int:
        while len(self.data) % 4:
            self.data += b'\0'
        at = len(self.data)
        if isinstance(value, str):
            self.data += struct.pack('<I', len(value)) + value.encode() + b'\0'
        elif isinstance(value, bytes):
            self.data += value
        elif isinstance(value, list):
            self.data += struct.pack('<I', len(value)) + bytes(4 * len(value))
            for k, item in enumerate(value):
                slot = at + 4 + 4 * k
                struct.pack_into('<I', self.data, slot, self._place(item) - slot)
        else:
            at = self.table(value)
        return at

    def table(self, fields: dict[int, tuple[int, object]]) -> int:
        """Write a table: field number -> (width, scalar) or (0, object an offset points at)."""
        layout, offset = {}, 4
        for number, (width, _) in sorted(fields.items()):
            size = width or 4
            offset += -offset % size
            layout[number] = offset
            offset += size
        count = max(fields, default=-1) + 1
        vtable = struct.pack(f'<{2 + count}H', 4 + 2 * count, offset, *(layout.get(i, 0) for i in range(count)))
        pad = -len(vtable) % 4
        self.data += vtable + bytes(pad)
        at = len(self.data)
        self.data += struct.pack('<i', len(vtable) + pad) + bytes(offset - 4)
        for number, (width, value) in fields.items():
            if width:
                self.data[at + layout[number] : at + layout[number] + width] = int(value).to_bytes(width, 'little')
        for number, (width, value) in fields.items():
            if not width:
                field = at + layout[number]
                struct.pack_into('<I', self.data, field, self._place(value) - field)
        return at

    def finish(self, root: dict[int, tuple[int, object]]) -> bytes:
        struct.pack_into('<I', self.data, 0, self.table(root))
        return bytes(self.data)


class AirpTest(unittest.TestCase):
    def test_render_state(self):
        color0 = {0: (2, 80), 2: (1, 1), 6: (1, 5), 5: (1, 5)}
        color1 = {0: (2, 55), 1: (1, 8), 2: (1, 1), 6: (1, 1)}
        vertex = {0: (0, [{0: (1, 29), 2: (4, 1)}, {0: (1, 27), 1: (4, 8), 2: (4, 1)}]), 1: (0, [{}, {2: (8, 16)}])}
        targets = {9: (0, [color0, color1]), 10: (2, 252), 4: (4, 1)}
        state = {
            0: (0, 'quad_vertex'),
            1: (0, '__air_resolved__::tinted_0'),
            2: (0, {1: (0, vertex), 20: (0, {0: (0, ['__air_resolved__::fetch_0'])})}),
            3: (0, targets),
        }
        buffer = FlatBuffer(b'AIRP').finish({1: (1, 1), 2: (1, 1), 3: (0, state)})
        archive = airp.Archive.__new__(airp.Archive)
        archive.source, archive.owner, archive.hashes = '/x/archive.metallib', 'X.framework', {}
        archive.descriptors = buffer
        archive.specialized = {'tinted': gpubin.Specialization('tint_fragment', ((3, 53, b'\1'), (0, 50, b'\2\0')))}
        (p,) = archive.pipelines()
        self.assertEqual(
            p.line(),
            'R ok=1 label=- samples=1 topo=0 ds=252/0 c0=80:15:1:1:5:0:1:5:0 c1=55:8:1:1:1:0:1:0:0'
            ' a0=29:0:1:16:1:1 a1=27:8:1:16:1:1 | v quad_vertex  | f tint_fragment #0:50:0200 #3:53:01 | vl fetch  | ',
        )


class DescTest(unittest.TestCase):
    @staticmethod
    def _table(fields: dict[int, bytes]) -> bytes:
        return bytes([len(fields)]) + b''.join(bytes([n, len(v)]) + v for n, v in fields.items())

    def test_render_state(self):
        # sections: 0 libraries, 1-6 empty, 7 one render state
        function = struct.pack('<4I', 2, 16, 12, 1) + b'card_vertex\0'
        fragment = struct.pack('<4I', 2, 16, 12, 1) + b'card_fragment\0'
        color = self._table({0: struct.pack('<I', 0), 1: struct.pack('<I', 80), 2: struct.pack('<I', 1)})
        colors = struct.pack('<2I', 1, 8) + color
        fixed = 1 + 6 * 5
        pad = -fixed % 4
        vertex_at = fixed + pad
        fragment_at = vertex_at + len(function)
        colors_at = fragment_at + len(fragment)
        state = self._table(
            {
                1: struct.pack('<I', vertex_at),
                2: struct.pack('<I', fragment_at),
                8: struct.pack('<I', colors_at),
                9: struct.pack('<I', 252),
                0: struct.pack('<I', colors_at + len(colors)),
            }
        )
        state += b'\0' * pad + function + fragment + colors + b'Card\0'
        library = struct.pack('<2I', 1, 8) + b'default.metallib\0'
        sections = (
            [struct.pack('<2I', 1, 8) + library] + [struct.pack('<I', 0)] * 6 + [struct.pack('<2I', 1, 8) + state]
        )
        data = bytearray(struct.pack('<I', len(sections)) + bytes(4 * len(sections)))
        for i, section in enumerate(sections):
            data += b'\0' * (-len(data) % 4)
            struct.pack_into('<I', data, 4 + 4 * i, len(data))
            data += section
        (p,) = desc.Desc(bytes(data), '/x/Card.pipelinelib/pipelines.desc', 'X.framework').pipelines()
        self.assertEqual(
            p.line(),
            'R ok=1 label=Card samples=1 topo=0 ds=252/0 c0=80:15:1:1:0:0:1:0:0 | v card_vertex  | f card_fragment  | ',
        )


class RequiredTest(unittest.TestCase):
    def test_a_function_lacking_a_required_constant_is_marked(self):
        libraries = declared.Libraries.__new__(declared.Libraries)
        libraries.by_hash, libraries.constant_names = {}, {}
        libraries.by_name = {'sun_fsh': ['/L/default.metallib']}
        libraries.required = {'/L/default.metallib': {'sun_fsh': (Declared('red', 16, 7, True),)}}
        given = Function('sun_fsh', constants=[Constant(7, None, 16, bytes(2))])
        lacking = Function('sun_fsh', constants=[Constant(8, None, 16, bytes(2))])
        for f in (given, lacking):
            self.assertTrue(libraries.place(f, '/L/archive.metallib'))
        self.assertFalse(given.lacks_required)
        self.assertTrue(lacking.lacks_required)


class ModelTest(unittest.TestCase):
    def test_constants_by_index_then_name_at_their_types_size(self):
        f = Function('f', '/L/default.metallib', [Constant(None, 'b', 53, b'\1'), Constant(2, None, 50, b'\3')])
        self.assertEqual(f.stage(), 'f lib=/L/default.metallib #2:50:0300 b:53:01')

    def test_families(self):
        self.assertFalse(Pipeline('render', '', '', families=frozenset({1004})).for_device())
        self.assertTrue(Pipeline('render', '', '', excluded_families=frozenset({1004})).for_device())


if __name__ == '__main__':
    unittest.main()
