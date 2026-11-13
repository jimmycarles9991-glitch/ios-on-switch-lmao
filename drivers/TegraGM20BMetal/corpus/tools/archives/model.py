"""A pipeline state an Apple archive declares, and its corpus line: the journal's R or C line for its descriptor.

The line is what the device would journal for the same descriptor (NativeBridgeMetalFrontend.mm journalRenderPipeline),
so the pack's key of it (raw/pack/rawpack.py state_key) is the key the raw runtime asks for (RawMetalLibrary.mm
raw_metal_pipeline_key): every descriptor field at Metal's default where the archive leaves it out.
"""

import re
from dataclasses import dataclass, field

from drivers.TegraGM20BMetal.corpus.tools.archives.gpubin import type_width

RENDER, COMPUTE, TILE, TESSELLATION = 'render', 'compute', 'tile', 'tessellation'
# A render state whose function lacks a constant its library requires: the archive does not record it, so the
# state's own values are its caller's at run time
UNSPECIALIZED = 'unspecialized'
# MTLGPUFamilyApple1 .. Apple3: what the raw device answers supportsFamily: true for (raw_device_answers.h)
DEVICE_FAMILIES = frozenset({1001, 1002, 1003})
FRAMEWORK_LIBRARY = re.compile(r'^/System/Library/(?:Private)?Frameworks/\w+\.framework/default\.metallib$')
# A bundle's library or a framework resource library (tools/gpu/rawpack.py OS_LIBRARY), keyed by its functions' AIR
BUNDLE_LIBRARY = re.compile(r'^/System/Library/\S+?\.(?:bundle|framework/Resources)/\S+\.metallib$')
STAGE_LIBRARY = re.compile(r' \| [vf] \S+ lib=(\S+)')
LINKED = re.compile(r' \| [vf][lp] \S+(?: lib=(\S+))?')


def packable(line: str) -> bool:
    """Whether the raw runtime looks a render state up in the pack (RawMetalStates.mm pipelineFor).

    It has both functions, from one framework's default.metallib, and every function it links is a framework
    library's; or both from one bundle's or framework resource library, linking none (looked up by their AIR). The
    direct compile makes any other state when it is created (m2v one with no fragment function).
    """
    stages = STAGE_LIBRARY.findall(line)
    libraries = set(stages)
    linked = LINKED.findall(line)
    if len(stages) != 2 or len(libraries) != 1:
        return False
    library = next(iter(libraries))
    if BUNDLE_LIBRARY.match(library) and '/QuartzCore.framework/' not in library:
        return not linked
    return FRAMEWORK_LIBRARY.match(library) is not None and all(FRAMEWORK_LIBRARY.match(lib) for lib in linked)


@dataclass
class Constant:
    """A function constant's value: by index or by name (as the caller set it), its MTLDataType and its bytes."""

    index: int | None
    name: str | None
    kind: int
    value: bytes

    def text(self) -> str:
        head = self.name if self.name is not None else f'#{self.index}'
        width = type_width(self.kind)
        return f'{head}:{self.kind}:{self.value[:width].ljust(width, bytes(1)).hex()}'


@dataclass
class Function:
    name: str
    library: str | None = None
    constants: list[Constant] = field(default_factory=list)
    specialized: bool = False
    lacks_required: bool = False

    def constants_text(self) -> str:
        """Give the constants as MTLFunctionConstantValues lists them: by index in index order, then by name."""
        indexed = sorted((c for c in self.constants if c.name is None), key=lambda c: c.index or 0)
        named = sorted((c for c in self.constants if c.name is not None), key=lambda c: c.name or '')
        return ' '.join(c.text() for c in [*indexed, *named])

    def stage(self) -> str:
        """Give the journal's stage text: "<name> lib=<library> <constants>"."""
        text = self.name + (f' lib={self.library}' if self.library else '')
        constants = self.constants_text()
        if self.specialized and not constants:
            text += ' spec'
        return f'{text} {constants}'


@dataclass
class Color:
    index: int
    format: int
    write_mask: int = 15
    blending: int = 0
    src_rgb: int = 1
    dst_rgb: int = 0
    rgb_op: int = 0
    src_alpha: int = 1
    dst_alpha: int = 0
    alpha_op: int = 0

    def text(self) -> str:
        fields = (self.format, self.write_mask, self.blending, self.src_rgb, self.dst_rgb, self.rgb_op)
        fields += (self.src_alpha, self.dst_alpha, self.alpha_op)
        return f'c{self.index}=' + ':'.join(map(str, fields))


@dataclass
class Attribute:
    index: int
    format: int
    offset: int = 0
    buffer: int = 0


@dataclass
class Layout:
    stride: int = 0
    step_function: int = 1
    step_rate: int = 1


@dataclass
class Pipeline:
    """One declared state, `source` the archive file it came from and `owner` the framework, bundle or app it is in."""

    kind: str
    source: str
    owner: str
    label: str = '-'
    samples: int = 1
    topology: int = 0
    depth: int = 0
    stencil: int = 0
    colors: list[Color] = field(default_factory=list)
    attributes: list[Attribute] = field(default_factory=list)
    layouts: dict[int, Layout] = field(default_factory=dict)
    vertex: Function | None = None
    fragment: Function | None = None
    kernel: Function | None = None
    vertex_linked: list[Function] = field(default_factory=list)
    families: frozenset[int] = frozenset()
    excluded_families: frozenset[int] = frozenset()
    max_threads: int = 0
    multiple_of_width: int = 0

    def for_device(self) -> bool:
        """Whether a device of the raw device's families is one the archive declares the state for."""
        if self.families and not self.families & DEVICE_FAMILIES:
            return False
        return not self.excluded_families >= DEVICE_FAMILIES

    def line(self) -> str:
        label = self.label.replace(' ', '%20') if self.label else '-'
        if self.kind == COMPUTE:
            head = f'C ok=1 form=d opt=0 max={self.max_threads} tgm={self.multiple_of_width} label={label}'
            return f'{head} | k {self.kernel.stage() if self.kernel else "- "} | '
        head = f'R ok=1 label={label} samples={self.samples} topo={self.topology} ds={self.depth}/{self.stencil}'
        for color in sorted(self.colors, key=lambda c: c.index):
            if color.format:
                head += ' ' + color.text()
        for a in sorted(self.attributes, key=lambda a: a.index):
            if a.format:
                lay = self.layouts.get(a.buffer, Layout())
                head += f' a{a.index}={a.format}:{a.offset}:{a.buffer}:{lay.stride}:{lay.step_function}:{lay.step_rate}'
        if self.kind == TILE:
            return f'T{head[1:]} | t {self.kernel.stage() if self.kernel else "- "} | '
        vertex = self.vertex.stage() if self.vertex else '- '
        fragment = self.fragment.stage() if self.fragment else '- '
        linked = ''.join(f' | vl {f.stage()}' for f in self.vertex_linked)
        return f'{head} | v {vertex} | f {fragment}{linked} | '
