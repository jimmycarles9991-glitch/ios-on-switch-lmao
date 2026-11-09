"""The pipeline states of a binary archive's AIRP buffers (gpubin.py): one buffer per state Apple compiled ahead.

The fields, numbered as the buffers' vtables number them, were matched against the states QuartzCore's tables make of
its offline compilation manifest (raw/pack/ca): every state both name has the same corpus line.

  root          0 a 12-byte header, 1 a byte, 2 the kind (1 render, 2 compute, 3 tile), 3 the state
  render        0 the vertex function, 1 the fragment function, 2 {1 vertex descriptor, 3 input primitive topology,
                20 {0 the vertex linked functions}}, 3 {4 raster sample count, 9 colour attachments, 10 depth format
                (u16), 11 stencil format (u16)}
  attachment    0 pixel format (u16), 1 write mask (u8, 15 when left out), 2 blending, 3 alpha and 4 RGB operation,
                5 alpha and 6 RGB destination factor, 7 alpha and 8 RGB source factor (1 when left out)
  descriptor    0 attributes {0 format (u8), 1 offset, 2 buffer index}, 1 layouts by buffer index {2 stride}
  compute       0 the kernel, 1 {1 max total threads per threadgroup, 6 threadgroup size is a multiple of the width}
  tile          0 the tile function, 1 {5 colour attachments}

A function is a library function by name, or one the archive specialized by its own name (a render state's as
"__air_resolved__::<name>_<n>"): the __metallib section's AIRC buffers name the function it was made from and the
constants it was made with.
"""

from drivers.TegraGM20BMetal.corpus.tools.archives import gpubin
from drivers.TegraGM20BMetal.corpus.tools.archives.flatbuf import Table
from drivers.TegraGM20BMetal.corpus.tools.archives.model import (
    COMPUTE,
    RENDER,
    TILE,
    Attribute,
    Color,
    Constant,
    Function,
    Layout,
    Pipeline,
)

KINDS = {1: RENDER, 2: COMPUTE, 3: TILE}
RESOLVED = '__air_resolved__::'


class Archive:
    """One Apple GPU binary's states and what resolves their functions."""

    def __init__(self, binary: bytes, source: str, owner: str) -> None:
        found = gpubin.sections(binary)
        self.source, self.owner = source, owner
        self.descriptors = found.get('__descriptor', b'')
        self.specialized = gpubin.specializations(found.get('__metallib', b''))
        self.hashes = gpubin.function_hashes(found.get('__metallib', b''))
        self.visible = len(gpubin.buffers(self.descriptors, b'AIRF'))

    def pipelines(self) -> list[Pipeline]:
        out = []
        for buffer in gpubin.buffers(self.descriptors, b'AIRP'):
            root = Table.root(buffer)
            kind = KINDS.get(root.scalar(2, 1))
            body = root.table(3)
            if kind and body is not None:
                out.append({RENDER: self._render, COMPUTE: self._compute, TILE: self._tile}[kind](body))
        return out

    def function(self, name: str | None) -> Function | None:
        """Give the function a state names; the caller finds its library (by base_hash)."""
        if not name:
            return None
        own = name.removeprefix(RESOLVED).rsplit('_', 1)[0] if name.startswith(RESOLVED) else name
        spec = self.specialized.get(own)
        if spec:
            constants = [Constant(i, None, kind, value) for i, kind, value in spec.constants]
            return Function(spec.base or own, constants=constants, specialized=True)
        return Function(own)

    def base_hash(self, function: Function) -> bytes | None:
        """Give the hash of the library function `function` is or was made from: its library's function of that hash."""
        return self.hashes.get(function.name)

    def _pipeline(self, kind: str) -> Pipeline:
        return Pipeline(kind, self.source, self.owner)

    def _render(self, body: Table) -> Pipeline:
        p = self._pipeline(RENDER)
        p.vertex, p.fragment = self.function(body.string(0)), self.function(body.string(1))
        stage = body.table(2)
        if stage is not None:
            p.topology = stage.scalar(3, 1)
            p.vertex_linked = [f for f in map(self.function, stage.table(20).strings(0) if stage.has(20) else ()) if f]
            vertex = stage.table(1)
            if vertex is not None:
                attributes, layouts = enumerate(vertex.tables(0)), enumerate(vertex.tables(1))
                p.attributes = [Attribute(i, a.scalar(0, 1), a.scalar(1, 4), a.scalar(2, 4)) for i, a in attributes]
                p.layouts = {i: Layout(b.scalar(2, 8), b.scalar(0, 1, 1), b.scalar(1, 4, 1)) for i, b in layouts}
        targets = body.table(3)
        if targets is not None:
            p.samples = targets.scalar(4, 4, 1)
            p.colors = [_color(i, c) for i, c in enumerate(targets.tables(9))]
            p.depth, p.stencil = targets.scalar(10, 2), targets.scalar(11, 2)
        return p

    def _compute(self, body: Table) -> Pipeline:
        p = self._pipeline(COMPUTE)
        p.kernel = self.function(body.string(0))
        state = body.table(1)
        if state is not None:
            p.max_threads, p.multiple_of_width = state.scalar(1, 4), state.scalar(6, 1)
        return p

    def _tile(self, body: Table) -> Pipeline:
        p = self._pipeline(TILE)
        p.kernel = self.function(body.string(0))
        state = body.table(1)
        if state is not None:
            p.colors = [_color(i, c) for i, c in enumerate(state.tables(5))]
        return p


def _color(index: int, c: Table) -> Color:
    return Color(
        index,
        c.scalar(0, 2),
        write_mask=c.scalar(1, 1, 15),
        blending=c.scalar(2, 1),
        alpha_op=c.scalar(3, 1),
        rgb_op=c.scalar(4, 1),
        dst_alpha=c.scalar(5, 1),
        dst_rgb=c.scalar(6, 1),
        src_alpha=c.scalar(7, 1, 1),
        src_rgb=c.scalar(8, 1, 1),
    )
