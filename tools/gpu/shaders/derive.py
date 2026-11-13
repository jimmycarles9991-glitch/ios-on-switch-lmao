"""Pipelines the corpus did not record but a process is likely to make, derived from the corpus and the libraries.

- kernels: every kernel of every library as a compute pipeline. A kernel no process was seen to make is made as its
  library's own kernels were (the same pipeline fields, the constants every one of them was given, and for each
  constant the kernel requires the values that constant took), or from the function alone when its library was
  never seen and it requires no constant.
- targets: each render program (vertex layout and both stages) on every target its fragment function was seen
  drawing into with the same number of color attachments.
- neighbours: each render program with one constant set to another value the corpus saw it take, at most
  NEIGHBOURS per program.

A derived pipeline no process makes costs seed space and build time, never a wrong draw: m2v's keys cover every
field of what is made.
"""

import itertools
from collections import Counter, defaultdict
from collections.abc import Iterable

from tools.gpu.shaders.journal import (
    Render,
    Stage,
    compute_line,
    constant_key,
    ordered,
    parse_kernel,
    parse_specialization,
)
from tools.gpu.shaders.metallib import KERNEL, Constant, Function

NEIGHBOURS = 32
KERNEL_VARIANTS = 4
SHARED = 0.9
BOOL = 53
PLAIN = 'form=f opt=0'


def _spelled(constant: Constant, by_index: bool) -> str:
    return f'#{constant.index}' if by_index else constant.name


class _Library:
    """What the corpus shows of how one library's kernels are made."""

    def __init__(self) -> None:
        self.kernels: set[str] = set()
        self.pipelines: set[str] = set()
        self.fields: Counter[str] = Counter()
        self.made: list[tuple[str, ...]] = []
        self.values: dict[str, set[str]] = defaultdict(set)

    def see(self, fields: str | None, kernel: Stage) -> None:
        self.kernels.add(kernel.name)
        if fields is not None:
            self.pipelines.add(kernel.name)
            self.fields[fields] += 1
        self.made.append(kernel.constants)
        for token in kernel.constants:
            self.values[constant_key(token)].add(token)

    def common_fields(self) -> str:
        return self.fields.most_common(1)[0][0] if self.fields else PLAIN

    def by_index(self) -> bool:
        keys = [constant_key(t) for made in self.made for t in made]
        return sum(k.startswith('#') for k in keys) * 2 > len(keys)

    def shared(self) -> tuple[str, ...]:
        """Return the constants (with their values) nearly every kernel of the library was given."""
        if len(self.made) < 3:
            return ()
        counts = Counter(t for made in self.made for t in set(made))
        return tuple(t for t, n in counts.items() if n >= SHARED * len(self.made))

    def variants(self, function: Function) -> list[tuple[str, ...]]:
        """Return the constants `function` is likely given, by how the library's other kernels were."""
        by_index, shared = self.by_index(), self.shared()
        choices: list[list[str]] = []
        for constant in function.required():
            key = _spelled(constant, by_index)
            seen = sorted(t for t in self.values.get(key, ()) if t.rsplit(':', 2)[1] == str(constant.type))
            if not seen and constant.type == BOOL:
                seen = [f'{key}:{BOOL}:00', f'{key}:{BOOL}:01']
            if not seen:
                return []
            choices.append(seen)
        out = []
        for combo in itertools.islice(itertools.product(*choices), KERNEL_VARIANTS):
            given = {constant_key(t): t for t in shared}
            given.update((constant_key(t), t) for t in combo)
            out.append(ordered(list(given.values())))
        return out


def _observed(index: dict[str, list[Function]], corpus: Iterable[str]) -> tuple[dict[str, _Library], list[Stage]]:
    """Each library's kernels as the corpus made them (C lines, and S lines of kernels), and the S lines' kernels."""
    kinds = {(lib, f.name): f.type for lib, functions in index.items() for f in functions}
    libraries: dict[str, _Library] = defaultdict(_Library)
    specialized = []
    for line in corpus:
        made = parse_kernel(line)
        if made and made[1].lib:
            libraries[made[1].lib].see(*made)
            continue
        stage = parse_specialization(line)
        if stage and stage.lib and kinds.get((stage.lib, stage.name)) == KERNEL:
            specialized.append(stage)
    for stage in specialized:
        libraries[stage.lib].see(None, stage)
    return libraries, specialized


def kernels(index: dict[str, list[Function]], corpus: Iterable[str]) -> list[str]:
    """Write C lines for the kernels of `index` (library spelling -> functions) the corpus made no pipeline of."""
    libraries, specialized = _observed(index, corpus)
    out = [
        compute_line(libraries[s.lib].common_fields(), s)
        for s in specialized
        if s.name not in libraries[s.lib].pipelines
    ]
    for lib, functions in sorted(index.items()):
        seen = libraries.get(lib)
        for function in functions:
            if function.type != KERNEL or (seen and function.name in seen.kernels):
                continue
            stage = Stage(function.name, lib)
            if seen is None:
                if not function.required():
                    out.append(compute_line(PLAIN, stage))
                continue
            variants = seen.variants(function) if function.required() or seen.shared() else [()]
            out.extend(compute_line(seen.common_fields(), stage.specialized(v) if v else stage) for v in variants)
    return out


def targets(renders: list[Render]) -> list[Render]:
    """Put each program on every target its fragment function drew into with as many color attachments."""
    seen = defaultdict(set)
    for r in renders:
        seen[(r.vertex.lib, r.fragment.name if r.fragment else '-', r.colors())].add(r.target)
    return [
        Render(target, r.layout, r.vertex, r.fragment, r.label, r.linked)
        for r in renders
        for target in sorted(seen[(r.vertex.lib, r.fragment.name if r.fragment else '-', r.colors())])
        if target != r.target
    ]


def _with(stage: Stage | None, key: str, token: str) -> Stage | None:
    if stage is None or key not in {constant_key(t) for t in stage.constants}:
        return stage
    return Stage(
        stage.name,
        stage.lib,
        True,
        tuple(token if constant_key(t) == key else t for t in stage.constants),
        stage.specialized_name,
    )


def neighbours(renders: list[Render]) -> list[Render]:
    """Each program with one constant (of either stage, or both where both declare it) set to another value it took."""
    values = defaultdict(set)
    for r in renders:
        for stage in (r.vertex, r.fragment):
            for token in stage.constants if stage else ():
                values[(stage.name, stage.lib, constant_key(token))].add(token)
    out = []
    for r in renders:
        made = 0
        keys = sorted({constant_key(t) for s in (r.vertex, r.fragment) if s for t in s.constants})
        for key in keys:
            pool = set()
            for stage in (r.vertex, r.fragment):
                pool |= values.get((stage.name, stage.lib, key), set()) if stage else set()
            for token in sorted(pool):
                derived = Render(
                    r.target, r.layout, _with(r.vertex, key, token), _with(r.fragment, key, token), r.label, r.linked
                )
                if derived.key() != r.key() and made < NEIGHBOURS:
                    out.append(derived)
                    made += 1
    return out
