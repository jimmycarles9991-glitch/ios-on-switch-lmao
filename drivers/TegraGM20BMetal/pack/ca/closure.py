"""Core Animation's states closed over their context: every known content core under every context value it can take.

QuartzCore 27 keys a state by a 32-byte spec (Pipeline::Spec). Part of it is the draw's content: the texture function,
the images' functions, coordinates and colour programs, the blends, the vertex layout. The rest is the context the
draw lands in, which any content can meet: the render target's attachment set, the attachment written and whether it
is a float target (u), and the flags the bound images and the layer set (v perspective, n dither, s headroom, p
premultiply, e EDR factor), and each texture's filter. context-<build>.json gives each context dimension's values and
how far they are proven; this module crosses every seed's content with them, as QuartzCore's get_pipeline_spec would.
"""

import copy
import itertools
import json
from collections.abc import Iterable
from pathlib import Path

import caname
from caspec27 import Spec, Tables27, decode

HERE = Path(__file__).resolve().parent
FLOAT_TARGETS = (115, 125)  # get_pipeline_spec: u is the written surface's format, RGBA16Float or RGBA32Float
FLAGS = ('v', 'n', 's', 'p', 'e')


def context(build: str) -> dict | None:
    """Give the build's context table, None where the build has none."""
    path = HERE / f'context-{build}.json'
    return json.loads(path.read_text()) if path.exists() else None


def demand(build: str) -> list[str]:
    """Give the names the build's devices made (demand-<build>.json), none where it has no table."""
    path = HERE / f'demand-{build}.json'
    return json.loads(path.read_text())['names'] if path.exists() else []


def fixed(tables: Tables27, s: Spec) -> bool:
    return tables.shader_type(s.T) == 0


def _type_byte8(tables: Tables27, s: Spec) -> int:
    return tables.t['fragment_shader_type_info'][tables.shader_type(s.T)][1]


def values(table: dict, dimension: str) -> list:
    return table['dimensions'][dimension]['values']


def written_index(table: dict, tables: Tables27, s: Spec) -> list[int]:
    """Give the attachments a state can write.

    get_pipeline_spec asserts index 0 for a named function of neither its own targets nor fragment_shader_type_info
    byte 8 bit 6 (0x184388108-0x18438813c).
    """
    if fixed(tables, s) or _type_byte8(tables, s) >> 6 & 1:
        return values(table, 'S')
    return [0]


def least_attachments(table: dict, tables: Tables27, s: Spec) -> int:
    """Give the fewest attachments the content's fragment function writes (the table's `needs`, by texture function)."""
    needs = table['dimensions']['attachments'].get('needs', {})
    return next((n for code, n in needs.items() if tables.dec['TextureFunction'].get(code) == s.T), 0)


def applies(tables: Tables27, s: Spec, letter: str) -> bool:
    """Give whether flag `letter` can be set for the content of `s` (the table's notes say where each comes from)."""
    images = s.images[: s.decoded]
    if letter == 'v':
        return True
    if letter == 'n':
        return fixed(tables, s) or bool(_type_byte8(tables, s) >> 5 & 1)
    if not fixed(tables, s):
        return False
    if letter == 'p':
        return any(i.M < 0 for i in images)
    if letter == 'e':
        return bool(images)
    return True


def closes(table: dict, dimension: str, demanded: bool) -> bool:
    """Give whether the table closes `dimension` for a seed.

    A dimension the table closes for "demand" alone keeps the other seeds' own value.
    """
    return table['dimensions'][dimension].get('seeds', 'all') == 'all' or demanded


def flag_domain(table: dict, tables: Tables27, s: Spec, letter: str, demanded: bool) -> tuple[bool, ...]:
    """Give the values flag `letter` takes for the content of `s`."""
    if not closes(table, letter, demanded):
        return (letter in s.flags,)
    return tuple(dict.fromkeys(bool(v) for v in values(table, letter))) if applies(tables, s, letter) else (False,)


def float_target(seed: Spec, s: Spec, index: int) -> tuple[bool, ...]:
    """Give u of a fixed-family state writing attachment `index`.

    Attachment 1 is RGBA16Float; attachment 0 is the target's format, and a seed drawn otherwise keeps its own too.
    """
    if index:
        return (True,)
    derived = s.P in FLOAT_TARGETS
    own = 'u' in seed.flags
    return (derived,) if seed.S or own == derived else (derived, own)


def filters(table: dict, tables: Tables27, s: Spec) -> list[tuple[int, ...]]:
    """Give each image's filters: a texture's (get_pipeline_spec 0x18438841c), a memoryless image's none."""
    codes = tables.dec['TextureFilter']
    domain = tuple(codes[c] for c in values(table, 'F'))
    return [domain if fixed(tables, s) and img.M < 0 else (img.F,) for img in s.images[: s.decoded]]


def states(tables: Tables27, table: dict, name: str, demanded: bool = True) -> list[str]:
    """Give the names of every context of the content of `name`, a strict name of this build and not a generic one.

    `demanded`: a device made it (the demand table).
    """
    seed = decode(tables, name)
    least = least_attachments(table, tables, seed)
    out = []
    for pflags, count in table['attachments']:
        if count < least:
            continue
        for index in written_index(table, tables, seed):
            if index >= count:
                continue
            base = copy.deepcopy(seed)
            base.A, base.S = count, index
            base.pflags = (set(seed.pflags) - {'m', 'w'}) | set(pflags)
            us = float_target(seed, base, index) if fixed(tables, seed) else (False,)
            for u, flags, fs in itertools.product(
                us,
                itertools.product(*(flag_domain(table, tables, seed, f, demanded) for f in FLAGS)),
                itertools.product(*filters(table, tables, seed)),
            ):
                s = copy.deepcopy(base)
                s.flags = (set(seed.flags) - set(FLAGS) - {'u'}) | {f for f, on in zip(FLAGS, flags, strict=True) if on}
                if u:
                    s.flags.add('u')
                for k, f in enumerate(fs):
                    s.images[k].F = f
                out.append(caname.encode(tables, s))
    return out


def strict(tables: Tables27, name: str) -> bool:
    """Give whether `name` is as this build's QuartzCore writes it (another build's journal names decode loosely)."""
    try:
        return caname.encode(tables, decode(tables, name)) == name
    except (KeyError, IndexError, ValueError):
        return False


def closure(tables: Tables27, table: dict, seeds: Iterable[str], demanded: Iterable[str] = ()) -> list[str]:
    """Give every context of every seed's content, each name once, the seeds' order kept.

    `demanded`: the seeds a device made, for the dimensions the table closes for those alone.
    """
    made = set(demanded)
    out: dict[str, None] = {}
    for name in seeds:
        if name.startswith('P') and strict(tables, name) and 'g' not in decode(tables, name).flags:
            out.update(dict.fromkeys(states(tables, table, name, name in made)))
    return list(out)
