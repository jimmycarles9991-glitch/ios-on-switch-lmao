"""The pipeline states of a pipeline library's pipelines.desc (<name>.pipelinelib/, or a framework's g<gpu>/ directory).

A pipeline library is what newPipelineLibraryWithFilePath: opens: pipelines.desc names its states, `functions` is the
metallib they are made from and pipelines.<gpu> holds one Apple GPU's compiled code. The raw device refuses the
library (RawMetalDeviceAnswers.mm), and its caller makes the same states from its own metallib.

The file: a u32 section count, each section's offset, and per section a u32 count and each entry's offset from the
section. Sections 0 libraries, 1 the specialized functions' names, 2 compute and 3 render state names, 5 the
specialized functions, 6 compute and 7 render states. An entry is either a "struct" (u32 count, each member's offset
from the entry) or a "table" (u8 count, then per field a u8 number, a u8 size and that many bytes: a scalar, or the
offset from the table of what the field holds). The field numbers were matched against the pipeline scripts beside some
libraries (PassKitUIFoundation's *-pipeline.json):

  render        0 label, 1 vertex and 2 fragment function, 3 vertex descriptor, 4 raster sample count, 8 colour
                attachments, 9 depth and 10 stencil format, 11-17 tessellation, 19 the GPU families it is for, 20
                those it is not for
  attachment    0 index, 1 pixel format, 2 blending, 3 RGB source and 4 destination factor, 5 RGB operation, 6 alpha
                source and 7 destination factor, 8 alpha operation, 9 write mask
  descriptor    0 attributes {0 index, 1 format, 2 offset, 3 buffer index}, 1 layouts {0 index, 1 step function,
                2 step rate, 3 stride}
  compute       0 the kernel, 1 threadgroup size is a multiple of the width, 2 label
  function      0 the function (a struct: its name, and 1 for a library function or 0 for a specialized one by its
                section 1 name), 1 constants {0 index, 1 name, 2 MTLDataType, 3 value}
"""

import struct

from drivers.TegraGM20BMetal.corpus.tools.archives.model import (
    COMPUTE,
    RENDER,
    TESSELLATION,
    Attribute,
    Color,
    Constant,
    Function,
    Layout,
    Pipeline,
)

LIBRARIES, FUNCTION_NAMES, SPECIALIZED, COMPUTES, RENDERS = 0, 1, 5, 6, 7
# A render state's tessellation fields (LocalAuthenticationPrivateUI's states set 12 and 16): the line has none of them
TESSELLATION_FIELDS = range(11, 18)
COLOR_FIELDS = ('index', 'format', 'blending', 'src_rgb', 'dst_rgb', 'rgb_op', 'src_alpha', 'dst_alpha', 'alpha_op')
COLOR_FIELDS += ('write_mask',)


class Desc:
    def __init__(self, data: bytes, source: str, owner: str) -> None:
        self.data, self.source, self.owner = data, source, owner
        count = self._u32(0)
        self.sections = [self._u32(4 + 4 * i) for i in range(count)]
        names = [self._struct(e) for e in self._entries(FUNCTION_NAMES)]
        specs = self._entries(SPECIALIZED)
        self.specialized = {self._string(members[1]): specs[self._u32(members[0])] for members in names}
        self.libraries = [self._string(self._struct(e)[0]) for e in self._entries(LIBRARIES)]

    def pipelines(self) -> list[Pipeline]:
        return [self._render(e) for e in self._entries(RENDERS)] + [self._compute(e) for e in self._entries(COMPUTES)]

    def _u32(self, at: int) -> int:
        return struct.unpack_from('<I', self.data, at)[0]

    def _string(self, at: int) -> str:
        return self.data[at : self.data.index(b'\0', at)].decode('utf-8', 'replace')

    def _struct(self, at: int) -> list[int]:
        return [at + self._u32(at + 4 + 4 * i) for i in range(self._u32(at))]

    def _entries(self, section: int) -> list[int]:
        return self._struct(self.sections[section]) if section < len(self.sections) else []

    def _table(self, at: int) -> dict[int, bytes]:
        out, field = {}, at + 1
        for _ in range(self.data[at]):
            number, size = self.data[field], self.data[field + 1]
            out[number] = self.data[field + 2 : field + 2 + size]
            field += 2 + size
        return out

    def _offset(self, at: int, value: bytes) -> int:
        return at + int.from_bytes(value, 'little')

    def _function(self, at: int) -> Function:
        name_at, kind_at = self._struct(at)
        name = self._string(name_at)
        if self._u32(kind_at) == 1 or name not in self.specialized:
            return Function(name)
        spec_at = self.specialized[name]
        spec = self._table(spec_at)
        base = self._function(self._offset(spec_at, spec[0])) if 0 in spec else Function(name)
        base.specialized = True
        for c_at in self._struct(self._offset(spec_at, spec[1])) if 1 in spec else ():
            c = self._table(c_at)
            named = self._string(self._offset(c_at, c[1])) if 1 in c else None
            index = int.from_bytes(c.get(0, b''), 'little')
            base.constants.append(Constant(None if named else index, named, int.from_bytes(c[2], 'little'), c[3]))
        return base

    def _render(self, at: int) -> Pipeline:
        t = self._table(at)
        p = Pipeline(TESSELLATION if any(n in t for n in TESSELLATION_FIELDS) else RENDER, self.source, self.owner)
        p.label = self._string(self._offset(at, t[0])) if 0 in t else '-'
        p.vertex = self._function(self._offset(at, t[1])) if 1 in t else None
        p.fragment = self._function(self._offset(at, t[2])) if 2 in t else None
        if 3 in t:
            self._vertex_descriptor(self._offset(at, t[3]), p)
        p.samples = int.from_bytes(t.get(4, b'\1'), 'little')
        p.colors = [self._color(c) for c in self._struct(self._offset(at, t[8]))] if 8 in t else []
        p.depth, p.stencil = (int.from_bytes(t.get(n, b''), 'little') for n in (9, 10))
        p.families = frozenset(self._u32(a) for a in self._words(at, t, 19))
        p.excluded_families = frozenset(self._u32(a) for a in self._words(at, t, 20))
        return p

    def _words(self, at: int, t: dict[int, bytes], number: int) -> list[int]:
        if number not in t:
            return []
        list_at = self._offset(at, t[number])
        return [list_at + 4 + 4 * i for i in range(self._u32(list_at))]

    def _compute(self, at: int) -> Pipeline:
        t = self._table(at)
        p = Pipeline(COMPUTE, self.source, self.owner)
        p.kernel = self._function(self._offset(at, t[0])) if 0 in t else None
        p.multiple_of_width = int.from_bytes(t.get(1, b''), 'little')
        p.label = self._string(self._offset(at, t[2])) if 2 in t else '-'
        return p

    def _color(self, at: int) -> Color:
        t = self._table(at)
        c = Color(0, 0)
        for number, value in t.items():
            if number < len(COLOR_FIELDS):
                setattr(c, COLOR_FIELDS[number], int.from_bytes(value, 'little'))
        return c

    def _vertex_descriptor(self, at: int, p: Pipeline) -> None:
        t = self._table(at)
        for a_at in self._struct(self._offset(at, t[0])) if 0 in t else ():
            a = {n: int.from_bytes(v, 'little') for n, v in self._table(a_at).items()}
            p.attributes.append(Attribute(a.get(0, 0), a.get(1, 0), a.get(2, 0), a.get(3, 0)))
        for l_at in self._struct(self._offset(at, t[1])) if 1 in t else ():
            lay = {n: int.from_bytes(v, 'little') for n, v in self._table(l_at).items()}
            p.layouts[lay.get(0, 0)] = Layout(lay.get(3, 0), lay.get(1, 1), lay.get(2, 1))
