"""The corpus's lines (precompile/harvest.py's form of the frontend's pipelines.log) as values: S, R and C lines.

A stage is `<function> [lib=<library>] [spec] <constant>...`, each constant `<name or #index>:<MTLDataType>:<hex>`.
An R line is `R ok=1 label=<label> <descriptor> | v <stage> | f <stage or -> | `, the descriptor's samples=, topo=, ds=
and c<i>= (the target) and a<i>= (the vertex layout) as the frontend wrote them. A C line is `C ok=1 <fields> | k
<stage> | ` with linked stages after the kernel's.
"""

import re
from dataclasses import dataclass, replace

TARGET = re.compile(r'(samples|topo|ds|opt|c\d+)=')
LAYOUT = re.compile(r'a\d+=')


@dataclass(frozen=True)
class Stage:
    """A function as a pipeline stage names it."""

    name: str
    lib: str | None = None
    spec: bool = False
    constants: tuple[str, ...] = ()
    specialized_name: str | None = None

    def text(self) -> str:
        """Return the stage as the journal writes it."""
        parts = [self.name]
        if self.lib:
            parts.append('lib=' + self.lib)
        if self.specialized_name:
            parts.append('as=' + self.specialized_name)
        if self.spec and not self.constants:
            parts.append('spec')
        return ' '.join(parts + list(self.constants))

    def specialized(self, constants: tuple[str, ...]) -> 'Stage':
        """Return this stage with `constants`, in the journal's order."""
        return replace(self, spec=True, constants=ordered(constants))


@dataclass(frozen=True)
class Render:
    """An R line: the target, the vertex layout and the two stages."""

    target: tuple[str, ...]
    layout: tuple[str, ...]
    vertex: Stage
    fragment: Stage | None
    label: str = '-'
    linked: tuple[tuple[str, Stage], ...] = ()

    def text(self) -> str:
        """Return the line as the corpus holds it."""
        head = ' '.join((*self.target, *self.layout))
        fragment = self.fragment.text() if self.fragment else '-'
        linked = ''.join(f'{tag} {stage.text()} | ' for tag, stage in self.linked)
        return f'R ok=1 label={self.label} {head} | v {self.vertex.text()} | f {fragment} | {linked}'

    def key(self) -> tuple:
        """Return what the line makes: everything but the label."""
        return (self.target, self.layout, self.vertex, self.fragment, self.linked)

    def colors(self) -> int:
        """Count its color attachments."""
        return sum(1 for t in self.target if t[0] == 'c')


def constant_key(token: str) -> str:
    """Return the name or #index a constant token sets."""
    return token.rsplit(':', 2)[0]


def ordered(constants: tuple[str, ...] | list[str]) -> tuple[str, ...]:
    """Order constants as the frontend does: by index for #index ones, then by name."""

    def order(token: str) -> tuple:
        key = constant_key(token)
        return (0, int(key[1:]), '') if key.startswith('#') and key[1:].isdigit() else (1, 0, key)

    return tuple(sorted(constants, key=order))


def parse_stage(text: str) -> Stage | None:
    """Parse a stage's text; None for `-` (no fragment function)."""
    tokens = text.split()
    if not tokens or tokens[0] == '-':
        return None
    lib, spec, constants, specialized_name = None, False, [], None
    for token in tokens[1:]:
        if token.startswith('lib='):
            lib = token[4:]
        elif token.startswith('as='):
            specialized_name = token[3:]
        elif token == 'spec':
            spec = True
        elif token.count(':') >= 2:
            constants.append(token)
    return Stage(tokens[0], lib, spec or bool(constants), tuple(constants), specialized_name)


def parse_render(line: str) -> Render | None:
    """Parse an R line, or None when it is not one."""
    parts = line.split(' | ')
    if not line.startswith('R ') or len(parts) < 3 or not parts[1].startswith('v ') or not parts[2].startswith('f '):
        return None
    head = parts[0].split()
    vertex = parse_stage(parts[1][2:])
    if vertex is None:
        return None
    label = next((t[6:] for t in head if t.startswith('label=')), '-')
    linked = tuple(
        (part[:2], stage)
        for part in parts[3:]
        if part[:3] in ('vl ', 'vp ', 'fl ', 'fp ')
        if (stage := parse_stage(part[3:])) is not None
    )
    return Render(
        tuple(t for t in head if TARGET.match(t)),
        tuple(t for t in head if LAYOUT.match(t)),
        vertex,
        parse_stage(parts[2][2:]),
        label,
        linked,
    )


def parse_kernel(line: str) -> tuple[str, Stage] | None:
    """Parse a C line, or a D line's state, into its fields (without the label) and its kernel stage."""
    if line.startswith('D ') and ' | ' in line:
        line = 'C ok=1 ' + line.split(' | ', 1)[1]
    parts = line.split(' | ')
    if not line.startswith('C ') or len(parts) < 2 or not parts[1].startswith('k '):
        return None
    kernel = parse_stage(parts[1][2:])
    fields = ' '.join(t for t in parts[0].split()[2:] if not t.startswith('label='))
    return (fields, kernel) if kernel else None


def kernel_links(line: str) -> tuple[tuple[str, Stage], ...]:
    return tuple(
        (part[0], stage)
        for part in line.split(' | ')[1:]
        if part[:2] in ('l ', 'p ')
        if (stage := parse_stage(part[2:])) is not None
    )


def compute_line(fields: str, kernel: Stage) -> str:
    """Write a C line making `kernel` with `fields` (form=f opt=0: newComputePipelineStateWithFunction)."""
    return f'C ok=1 {fields} | k {kernel.text()} | '


def parse_specialization(line: str) -> Stage | None:
    """Parse an S line into its function and constants."""
    m = re.match(r'S ok=1 (?:lib=(\S+) )?name=(\S+) k=(.*?)( \| |$)', line)
    if not m:
        return None
    stage = parse_stage(' '.join([m[2], *(['lib=' + m[1]] if m[1] else []), m[3]]))
    return replace(stage, spec=True)
