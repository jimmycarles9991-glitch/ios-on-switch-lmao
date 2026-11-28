"""Core Animation's states closed over their context: every known content core under every context value it can take.

QuartzCore keys a state by a Pipeline::Spec. Part of it is the draw's content: the texture function, the images'
functions, coordinates and colour programs, the blends, the vertex layout. The rest is the context the draw lands in,
which any content can meet: the render target's attachment set, the attachment written and whether it is a float target
(u), the flags the bound images and the layer set (v perspective on iOS 27, n dither, s headroom, p premultiply, e EDR
factor), and each textured image's filter. context-<build>.json gives each build's context dimensions and their values;
this module crosses every seed's content with them, as QuartzCore's get_pipeline_spec would.
"""

import copy
import itertools
import json
from collections.abc import Iterable
from pathlib import Path

import caname
import caspec
import caspec27

HERE = Path(__file__).resolve().parent
FLOAT_TARGETS = (115, 125)  # get_pipeline_spec: u is the written surface's format, RGBA16Float or RGBA32Float
FLAGS = ('v', 'n', 's', 'p', 'e')

TablesLike = caspec.Tables | caspec27.Tables27
SpecLike = caspec.Spec | caspec27.Spec


def _unwrap(tables: TablesLike) -> TablesLike:
    return getattr(tables, 'v27', None) or tables


def _is_v27(tables: TablesLike) -> bool:
    return isinstance(_unwrap(tables), caspec27.Tables27)


def _decode(tables: TablesLike, name: str) -> SpecLike:
    inner = _unwrap(tables)
    if isinstance(inner, caspec27.Tables27):
        return caspec27.decode(inner, name)
    return caspec.decode(inner, name)


def _images(tables: TablesLike, s: SpecLike) -> list:
    count = getattr(s, 'decoded', min(2, tables.images(s.T)))
    return s.images[:count]


def _textured(tables: TablesLike, img) -> bool:
    return img.M < 0 and tables.t['image_plane_count'][img.I] > 0


def _table_flags(table: dict) -> tuple[str, ...]:
    dims = table['dimensions']
    return tuple(f for f in FLAGS if f in dims)


def context(build: str) -> dict | None:
    """Give the build's context table, None where the build has none."""
    path = HERE / f'context-{build}.json'
    return json.loads(path.read_text()) if path.exists() else None


def demand(build: str) -> list[str]:
    """Give the names the build's devices made (demand-<build>.json), none where it has no table."""
    path = HERE / f'demand-{build}.json'
    return json.loads(path.read_text())['names'] if path.exists() else []


def fixed(tables: TablesLike, s: SpecLike) -> bool:
    return tables.shader_type(s.T) == 0


def _type_byte8(tables: TablesLike, s: SpecLike) -> int:
    return tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][1]


def values(table: dict, dimension: str) -> list:
    return table['dimensions'][dimension]['values']


def written_index(table: dict, tables: TablesLike, s: SpecLike) -> list[int]:
    """Give the attachments a state can write.

    get_pipeline_spec asserts index 0 for a named function of neither its own targets nor fragment_shader_type_info
    byte 8 bit 6 (24B119 0x184388108-0x18438813c, same table bit on 21H461).
    """
    if fixed(tables, s) or _type_byte8(tables, s) >> 6 & 1:
        return values(table, 'S')
    return [0]


def least_attachments(table: dict, tables: TablesLike, s: SpecLike) -> int:
    """Give the fewest attachments the content's fragment function writes (the table's `needs`, by texture function)."""
    needs = table['dimensions']['attachments'].get('needs', {})
    return next((n for code, n in needs.items() if tables.dec['TextureFunction'].get(code) == s.T), 0)


def applies(tables: TablesLike, s: SpecLike, letter: str) -> bool:
    """Give whether flag `letter` can be set for the content of `s` (the table's notes say where each comes from)."""
    images = _images(tables, s)
    if letter == 'v':
        return _is_v27(tables)
    if letter == 'n':
        return fixed(tables, s) or bool(_type_byte8(tables, s) >> 5 & 1)
    if not fixed(tables, s):
        return False
    if letter == 'p':
        return any(_textured(tables, i) for i in images)
    if letter == 'e':
        return bool(images)
    return True


def closes(table: dict, dimension: str, demanded: bool) -> bool:
    """Give whether the table closes `dimension` for a seed.

    A dimension the table closes for "demand" alone keeps the other seeds' own value.
    """
    return table['dimensions'][dimension].get('seeds', 'all') == 'all' or demanded


def flag_domain(table: dict, tables: TablesLike, s: SpecLike, letter: str, demanded: bool) -> tuple[bool, ...]:
    """Give the values flag `letter` takes for the content of `s`."""
    if not closes(table, letter, demanded):
        return (s.flag(letter),)
    return tuple(dict.fromkeys(bool(v) for v in values(table, letter))) if applies(tables, s, letter) else (False,)


def float_target(tables: TablesLike, seed: SpecLike, s: SpecLike, index: int) -> tuple[bool, ...]:
    """Give u of a fixed-family state writing attachment `index`.

    On 24B119 attachment 1 is always RGBA16Float; on 21H461 attachment 1 derives from s.P (caspec._attachment_format).
    On attachment 0 the target's format gives it, and a seed drawn otherwise keeps its own too.
    """
    fmt = (115 if index else s.P) if _is_v27(tables) else caspec._attachment_format(s, index)
    derived = fmt in FLOAT_TARGETS
    if index:
        return (derived,)
    own = seed.flag('u')
    return (derived,) if seed.S or own == derived else (derived, own)


def _filter_tuples(table: dict, tables: TablesLike, s: SpecLike) -> tuple[list[tuple[int, ...]], list[tuple[int, ...]]]:
    """Give (base, extra) filter tuples for `s`.

    Only fixed-family images with image_plane_count > 0 and M < 0 carry a texture filter; procedural images (planes ==
    0) and memoryless attachments (M >= 0) stay at their own filter. `base` are the tuples crossed with the full flag
    cube (the table's `cross_flags`, else all `values`); `extra` are single-slot variations over the remaining filter
    values so closing all 9 filters stays bounded instead of exploding as 9^2 * 32.
    """
    codes = tables.dec['TextureFilter']
    dim = table['dimensions']['F']
    all_vals = tuple(codes[c] for c in dim['values'])
    cross_names = dim.get('cross_flags', dim['values'])
    base_vals = tuple(codes[c] for c in cross_names)
    images = _images(tables, s)
    can_filter = [fixed(tables, s) and _textured(tables, img) for img in images]
    base = list(itertools.product(*(base_vals if ok else (img.F,) for ok, img in zip(can_filter, images, strict=True))))
    extra_vals = tuple(v for v in all_vals if v not in base_vals)
    if not extra_vals or not any(can_filter):
        return base, []
    seen = set(base)
    extra: list[tuple[int, ...]] = []
    for k, ok in enumerate(can_filter):
        if not ok:
            continue
        for v in extra_vals:
            for row in base:
                cand = (*row[:k], v, *row[k + 1 :])
                if cand not in seen:
                    seen.add(cand)
                    extra.append(cand)
    return base, extra


def filters(table: dict, tables: TablesLike, s: SpecLike) -> list[tuple[int, ...]]:
    """Give each image's filters: a textured image's (image_plane_count > 0, M < 0), procedural/memoryless none."""
    codes = tables.dec['TextureFilter']
    domain = tuple(codes[c] for c in values(table, 'F'))
    return [domain if fixed(tables, s) and _textured(tables, img) else (img.F,) for img in _images(tables, s)]


def _apply_flags(s: SpecLike, active_flags: tuple[str, ...], bits: tuple[bool, ...], u: bool) -> None:
    if isinstance(s.flags, set):
        s.flags = (set(s.flags) - set(active_flags) - {'u'}) | {
            f for f, on in zip(active_flags, bits, strict=True) if on
        }
        if u:
            s.flags.add('u')
        return
    mask = s.flags
    for f in (*active_flags, 'u'):
        mask &= ~(1 << caspec.FLAG_BITS[f])
    for f, on in zip(active_flags, bits, strict=True):
        if on:
            mask |= 1 << caspec.FLAG_BITS[f]
    if u:
        mask |= 1 << caspec.FLAG_BITS['u']
    s.flags = mask


def states(tables: TablesLike, table: dict, name: str, demanded: bool = True) -> list[str]:
    """Give the names of every context of the content of `name`, a strict name of this build and not a generic one.

    `demanded`: a device made it (the demand table).
    """
    inner = _unwrap(tables)
    seed = _decode(inner, name)
    least = least_attachments(table, inner, seed)
    active_flags = _table_flags(table)
    flag_combos = list(itertools.product(*(flag_domain(table, inner, seed, f, demanded) for f in active_flags)))
    seed_flags = tuple(seed.flag(f) for f in active_flags)
    base_fs, extra_fs = _filter_tuples(table, inner, seed)
    # Extra filters vary over clean (0-flag), every single applicable flag, and the seed's own flag combination.
    extra_flag_combos = list(
        dict.fromkeys(
            [fl for fl in flag_combos if sum(fl) <= 1 or fl == seed_flags]
            + ([seed_flags] if seed_flags not in flag_combos else [])
        )
    )
    out = []
    for pflags, count in table['attachments']:
        if count < least:
            continue
        for index in written_index(table, inner, seed):
            if index >= count:
                continue
            base = copy.deepcopy(seed)
            base.A, base.S = count, index
            base.pflags = (set(seed.pflags) - {'m', 'w'}) | set(pflags)
            us = float_target(inner, seed, base, index) if fixed(inner, seed) else (False,)
            for u in us:
                for fl, fs in itertools.chain(
                    itertools.product(flag_combos, base_fs),
                    itertools.product(extra_flag_combos, extra_fs),
                ):
                    s = copy.deepcopy(base)
                    _apply_flags(s, active_flags, fl, u)
                    for k, f in enumerate(fs):
                        s.images[k].F = f
                    out.append(caname.encode(inner, s))
    return out


def strict(tables: TablesLike, name: str) -> bool:
    """Give whether `name` is as this build's QuartzCore writes it (another build's journal names decode loosely)."""
    inner = _unwrap(tables)
    try:
        return caname.encode(inner, _decode(inner, name)) == name
    except (KeyError, IndexError, ValueError):
        return False


def closure(tables: TablesLike, table: dict, seeds: Iterable[str], demanded: Iterable[str] = ()) -> list[str]:
    """Give every context of every seed's content, each name once, the seeds' order kept.

    `demanded`: the seeds a device made, for the dimensions the table closes for those alone.
    """
    inner = _unwrap(tables)
    made = set(demanded)
    out: dict[str, None] = {}
    for name in seeds:
        if name.startswith('P') and strict(inner, name) and not _decode(inner, name).flag('g'):
            out.update(dict.fromkeys(states(inner, table, name, name in made)))
    return list(out)
