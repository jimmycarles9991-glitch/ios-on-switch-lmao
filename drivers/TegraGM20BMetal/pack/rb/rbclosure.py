"""The RenderBox states the pack holds beyond the corpus: each corpus state in every context RenderBox can draw it in.

RenderBox builds a pipeline from its 16-byte constant block (rbspec.py) alone; rb-<build>.json's "closure" section
holds what that build's RenderBox (its pipeline builder and the tables it reads) does with three parts of it:
  target  word 2's c0 MTLPixelFormat, which the builder gives c0, and from which RenderBox's format table gives word 3's
          extended and float colour bits and c2's format ("formats"); a colour state moves to every colour target,
          an alpha-only one (word 2's alpha-only bit, the target's) to every alpha-only target
  shape   word 0's shape class and sub-case, which the primitive functions ("shaped") decode; the sub-case comes from
          the geometry's edge alone, and sub-case 3 only with the edge test bit set
  blend   word 1's mode where the ROP blends: RenderBox puts a blend on the ROP or in the shader by the state's kind,
          shape and mode ("placement"), and gives the written colour the ROP blend of its mode from its blend table
          ("blend"), alpha's factors on an alpha-only target
The closure is their product over every corpus state, each block written into both stages: a vertex function that takes
the block carries its fragment's.
"""

import json
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path
from typing import NamedTuple

import rbspec

BLOCK = '#0:36:'  # the constant block's spelling on a line (rbspec.STATE)
BLEND_OFF = '0:1:0:0:1:0:0'  # a Metal colour attachment's blend fields with blending disabled
Colours = dict[int, tuple[int, int, str]]  # slot: (MTLPixelFormat, write mask, "<blend on>:<six blend fields>")
Field = tuple[int, int, int]


class Placement(NamedTuple):
    """Which states RenderBox blends in the shader (word 3's "blends" bit) rather than on the ROP."""

    rop_kinds: frozenset[int]  # always on the ROP, whatever the mode
    shader_kinds: frozenset[int]  # always in the shader
    shader_modes: frozenset[int]  # in the shader unless the kind, shape or coverage puts them on the ROP
    highest_rop_mode: int  # past it every mode is the shader's
    custom_mode: int  # the mode a custom blend counts as
    forced_rop_kinds: frozenset[int]  # on the ROP when the coverage target is not read
    primitive_kinds: frozenset[int]  # on the ROP for shape class 1, sub-case 1 or the edge test
    accumulator_kinds: frozenset[int]  # on the ROP when word 0 bits 6-7 are 2
    filter_kind: int
    filter_rop_modes: frozenset[int]  # the filter kind's modes that may reach the ROP
    reads_dest_kinds: frozenset[int]  # whose destination read the kind decides, not the placement
    coverage_kinds: frozenset[int]  # that always read the coverage target
    blur_kind: int  # whose coverage read its blur kind decides


class Closure(NamedTuple):
    """One build's rb-<build>.json "closure" section."""

    fields: dict[str, Field]
    formats: dict[int, tuple[int, int, int]]  # MTLPixelFormat: (extended, float, c2's format, 0 for c0's own)
    colour_targets: tuple[int, ...]
    alpha_targets: tuple[int, ...]
    exempt: frozenset[str]
    shaped: frozenset[str]
    shapes: tuple[int, ...]
    sub_cases: tuple[int, ...]
    colour_modes: tuple[int, ...]
    coverage_modes: tuple[int, ...]
    blend: dict[int, tuple[int, ...]]  # mode: rgb op, alpha op, source rgb, source alpha, destination rgb, alpha
    alpha_factor: dict[int, int]  # an alpha factor's rgb factor on an alpha-only target
    placement: Placement


def load(path: Path) -> Closure | None:
    """Read rb-<build>.json's closure, None for a build that has none."""
    with open(path) as f:
        c = json.load(f).get('closure')
    if c is None:
        return None
    p = c['placement']
    return Closure(
        {k: tuple(v) for k, v in c['fields'].items()},
        {int(k): tuple(v) for k, v in c['formats'].items()},
        tuple(c['colour_targets']),
        tuple(c['alpha_targets']),
        frozenset(c['exempt']),
        frozenset(c['shaped']),
        tuple(c['shapes']),
        tuple(c['sub_cases']),
        tuple(c['colour_modes']),
        tuple(c['coverage_modes']),
        {int(k): tuple(v) for k, v in c['blend'].items()},
        {int(k): v for k, v in c['alpha_factor'].items()},
        Placement(
            frozenset(p['rop_kinds']),
            frozenset(p['shader_kinds']),
            frozenset(p['shader_modes']),
            p['highest_rop_mode'],
            p['custom_mode'],
            frozenset(p['forced_rop_kinds']),
            frozenset(p['primitive_kinds']),
            frozenset(p['accumulator_kinds']),
            p['filter_kind'],
            frozenset(p['filter_rop_modes']),
            frozenset(p['reads_dest_kinds']),
            frozenset(p['coverage_kinds']),
            p['blur_kind'],
        ),
    )


def for_quartzcore(directory: Path, digest: str) -> tuple[rbspec.Tables, Closure] | None:
    """Give the tables and closure of the IPSW whose QuartzCore library has SHA-256 `digest`, None where it has none."""
    for path in sorted(directory.glob('rb-*.json')):
        with open(path) as f:
            if json.load(f).get('quartzcore_metallib_sha256') == digest:
                closure = load(path)
                return (rbspec.load(path), closure) if closure else None
    return None


def made(directory: Path, digest: str, corpus: Iterable[str], key: Callable[[str], str]) -> list[str]:
    """Give the closure's states the corpus lacks, for the IPSW whose QuartzCore library has SHA-256 `digest`."""
    found = for_quartzcore(directory, digest)
    return lines(*found, corpus, key) if found else []


def lines(tables: rbspec.Tables, closure: Closure, corpus: Iterable[str], key: Callable[[str], str]) -> list[str]:
    """Give the closure's states of the corpus's RenderBox lines that the corpus lacks, each once by `key`.

    `key` is the pack's state key of a line (make_pack.state_key): the journal spells one state several ways.
    """
    seeds = [line.rstrip('\n') for line in corpus if rbspec.renderbox_line(line)]
    have = {key(line) for line in seeds}
    out: dict[str, str] = {}
    for seed in seeds:
        for line in states_of(tables, closure, seed):
            k = key(line)
            if k not in have:
                out.setdefault(k, line)
    return list(out.values())


def states_of(tables: rbspec.Tables, closure: Closure, line: str) -> list[str]:
    """Give a RenderBox line's states on every target, shape and ROP blend of the closure, its own among them."""
    spec = rbspec.decode(line)
    if spec is None or spec.function in closure.exempt:
        return [line]
    if placed(tables, closure, list(spec.words), spec.colours) != (list(spec.words), spec.colours):
        return [line]
    out = []
    for words, colours in _targets(tables, closure, spec):
        for shaped in _shapes(closure, spec.function, words):
            for w, c in _blends(tables, closure, *placed(tables, closure, shaped, colours)):
                out.append(_remade(line, spec, w, c))
    return out


def placed(tables: rbspec.Tables, closure: Closure, words: list[int], colours: Colours) -> tuple[list[int], Colours]:
    """Give a state with what RenderBox derives from its blend placement as its pipeline builder writes it.

    Word 3's blend, inverted coverage, destination read, coverage read and depth feedback bits, and the written
    colour's blend: off where the shader blends, the mode's ROP blend where the ROP does.
    """
    p, f = closure.placement, closure.fields
    w = list(words)
    kind = w[0] & 63
    mode = p.custom_mode if w[1] >> 30 & 1 else w[1] >> 16 & 0x3FFF
    shader = shader_blends(closure, w)
    coverage_op = 19 <= mode <= 22
    _put(w, tables.fields['blends'], int(shader))
    _put(w, f['inverted'], int(mode in (21, 22)))
    if kind not in p.reads_dest_kinds:
        _put(w, tables.fields['reads_dest'], int(shader and not coverage_op))
    if w[1] >> 14 & 1 or kind in p.coverage_kinds:
        _put(w, tables.fields['reads_coverage'], 1)
    elif kind != p.blur_kind:
        _put(w, tables.fields['reads_coverage'], int(shader and mode | 2 == 22))
    _put(w, f['depth_feedback'], int(coverage_op and _get(w, f['depth']) & ~1 == 2))
    c = dict(colours)
    slot = _get(w, tables.fields['colour'])
    if slot in c and c[slot][1]:
        blend = BLEND_OFF if shader else rop_blend(closure, mode, _get(w, tables.fields['alpha_only']))
        c[slot] = (c[slot][0], c[slot][1], blend)
    return w, c


def retargeted(tables: rbspec.Tables, closure: Closure, line: str, target: int) -> str | None:
    """Give a RenderBox line's state on c0 format `target`, None where the closure moves it to none."""
    spec = rbspec.decode(line)
    if spec is None or spec.function in closure.exempt:
        return None
    for words, colours in _targets(tables, closure, spec, (target,)):
        if colours[0][0] == target:
            return _remade(line, spec, words, colours)
    return None


def shader_blends(closure: Closure, words: list[int]) -> bool:
    """Whether RenderBox blends a state in its shader: word 3's "blends" bit, as its pipeline builder sets it."""
    p = closure.placement
    kind = words[0] & 63
    mode = p.custom_mode if words[1] >> 30 & 1 else words[1] >> 16 & 0x3FFF
    if kind in p.rop_kinds:
        return False
    if (
        mode > p.highest_rop_mode
        or kind in p.shader_kinds
        or (kind == p.filter_kind and mode not in p.filter_rop_modes)
    ):
        return True
    return mode in p.shader_modes and not _forced_onto_the_rop(closure, words, kind)


def _forced_onto_the_rop(closure: Closure, words: list[int], kind: int) -> bool:
    """Whether the kind or the shape puts a state on the ROP whatever its mode, the coverage target unread."""
    p = closure.placement
    if words[1] >> 14 & 1:
        return False
    shape, sub, edge = (_get(words, closure.fields[n]) for n in ('shape', 'sub_case', 'edge_test'))
    return (
        kind in p.forced_rop_kinds
        or (kind in p.primitive_kinds and (shape == 1 or sub == 1 or bool(edge)))
        or (kind in p.accumulator_kinds and words[0] >> 6 & 3 == 2)
    )


def rop_blend(closure: Closure, mode: int, alpha_only: int) -> str:
    """Give the written colour's blend fields for a mode the ROP blends, on an alpha-only target or not."""
    if mode not in closure.blend:
        return BLEND_OFF
    rgb_op, alpha_op, source_rgb, source_alpha, dest_rgb, dest_alpha = closure.blend[mode]
    if alpha_only:
        f = closure.alpha_factor
        return f'1:{f[source_alpha]}:{f[dest_alpha]}:{alpha_op}:0:0:{alpha_op}'
    return f'1:{source_rgb}:{dest_rgb}:{rgb_op}:{source_alpha}:{dest_alpha}:{alpha_op}'


def keeps_target_rule(closure: Closure, words: list[int], colours: Colours) -> bool:
    """Whether a state's block and c2 are what RenderBox makes of its c0 format."""
    own = colours.get(0, (0, 0, ''))[0]
    second = colours.get(2)
    return (
        own in closure.formats
        and _on_target(closure, words, own) == words
        and (second is None or second[0] == (closure.formats[own][2] or own))
    )


def _get(words: list[int], field: Field) -> int:
    word, shift, mask = field
    return words[word] >> shift & mask


def _put(words: list[int], field: Field, value: int) -> None:
    word, shift, mask = field
    words[word] = words[word] & ~(mask << shift) | value << shift


def _on_target(closure: Closure, words: list[int], target: int) -> list[int]:
    extended, float_colour, _second = closure.formats[target]
    out = list(words)
    _put(out, closure.fields['format'], target)
    _put(out, closure.fields['extended'], extended)
    _put(out, closure.fields['float'], float_colour)
    return out


def _targets(
    tables: rbspec.Tables, closure: Closure, spec: rbspec.Spec, wanted: tuple[int, ...] | None = None
) -> Iterator[tuple[list[int], Colours]]:
    """Yield the state on its own target and, where it keeps the target rule, on each target of its kind."""
    words, colours = list(spec.words), spec.colours
    yield words, colours
    if not keeps_target_rule(closure, words, colours):
        return
    alpha_only = _get(words, tables.fields['alpha_only'])
    own, second = colours[0][0], colours.get(2)
    for target in wanted or (closure.alpha_targets if alpha_only else closure.colour_targets):
        if target == own or target not in closure.formats:
            continue
        moved = dict(colours)
        moved[0] = (target, *colours[0][1:])
        if second:
            moved[2] = (closure.formats[target][2] or target, *second[1:])
        yield _on_target(closure, words, target), moved


def _shapes(closure: Closure, function: str, words: list[int]) -> list[list[int]]:
    fields = closure.fields
    if function not in closure.shaped or not 1 <= _get(words, fields['shape']) <= 11:
        return [words]
    edge = _get(words, fields['edge_test'])
    out = []
    for shape in closure.shapes:
        for sub in closure.sub_cases or (_get(words, fields['sub_case']),):
            if sub == 3 and not edge:
                continue
            w = list(words)
            _put(w, fields['shape'], shape)
            _put(w, fields['sub_case'], sub)
            out.append(w)
    return out


def _blends(
    tables: rbspec.Tables, closure: Closure, words: list[int], colours: Colours
) -> list[tuple[list[int], Colours]]:
    """Give the state and, where the ROP blends its written colour, the state with every other ROP mode of its group."""
    slot, mode = _get(words, tables.fields['colour']), _get(words, tables.fields['mode'])
    written = colours.get(slot)
    out = [(words, colours)]
    if shader_blends(closure, words) or _get(words, tables.fields['custom_blend']) or not written or not written[1]:
        return out
    for m in closure.coverage_modes if mode in closure.coverage_modes else closure.colour_modes:
        w = list(words)
        _put(w, tables.fields['mode'], m)
        if m != mode and not shader_blends(closure, w):
            out.append(placed(tables, closure, w, colours))
    return out


def _remade(line: str, spec: rbspec.Spec, words: list[int], colours: Colours) -> str:
    """Give the line with its block in every stage that takes it and its colour attachments as given."""
    head, bar, rest = line.partition(' | ')
    tokens = []
    for token in head.split(' '):
        slot = token[1 : token.find('=')] if token.startswith('c') and '=' in token else ''
        if slot.isdigit() and int(slot) in colours:
            fmt, mask, blend = colours[int(slot)]
            tokens.append(f'c{slot}={fmt}:{mask}:{blend}')
        else:
            tokens.append(token)
    old, new = (BLOCK + _text(w) for w in (spec.words, words))
    return ' '.join(tokens) + bar + rest.replace(old, new)


def _text(words: Iterable[int]) -> str:
    return b''.join(w.to_bytes(4, 'little') for w in words).hex()
