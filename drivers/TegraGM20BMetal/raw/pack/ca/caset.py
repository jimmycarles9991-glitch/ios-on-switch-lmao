#!/usr/bin/env python3
"""The Core Animation states the raw pack holds, named as QuartzCore names them, and their journal R lines (caspec.py).

Tiers, each name in the first that has it:
  corpus     every Core Animation state the device's journals hold (precompile/corpus.log), by name
  manifest   QuartzCore's offline compilation manifest (CA::OGL::MetalContext::offline_compilation_manifest: the states
             Apple compiles ahead for its GPUs; warmup_shaders makes the ones of the device's GPU family at boot), but
             its generic states: CA makes those only out of a binary archive (create_pipeline_state refuses a generic
             spec without one), and this device has none
  variants   QuartzCore 27's: every member of the texture function families of the two above that a filter picks among
             at draw time (glass variants, blur passes: caspec27.VARIANT_FAMILIES), on the same pipeline
  blend      every fragment state of the tiers above under every blend CA puts in the pipeline (those
             Pipeline::Spec::lift_blend_function lifts out of the fragment: the hardware's) and none, on BGRA8 and on
             the state's own format where m2v draws it once (not the extended-range formats, which mkpack.py leaves out)
  fetch      every fixed-function fragment state of the tiers above with no blend of its own under every blend CA leaves
             in the fragment (framebuffer fetch: the separable and non-separable modes a compositing filter asks for)
A state is made from its name alone (caspec.state_fields): its descriptor is what QuartzCore makes of the name.

usage: caset.py <qc-<build>.json> <corpus.log> [tier ...]   prints the R lines (every tier when none is named)
"""

import sys

import caspec

TIERS = ('corpus', 'manifest', 'variants', 'blend', 'fetch')
LIFTABLE = range(1, 26)  # lift_blend_function: the blend functions above 25 stay in the fragment
CLOSURE_FORMATS = ('BGRA',)
SINGLE_DRAW_FORMATS = ('BGRA', 'RGBA', 'RGhA', 'RGfA')


def split(name: str) -> tuple[str, str]:
    prefix, _, fragment = name.partition('_')
    return prefix, fragment


def lifted(tables: caspec.Tables, fragment: str, fmt: int, blend: int) -> bool:
    """Pipeline::Spec::lift_blend_function: whether `blend` of `fragment` goes to the pipeline (hardware) blend."""
    if tables.v27:
        import caspec27  # noqa: PLC0415  (only a QuartzCore 27's tables need it)

        return caspec27.lifted(tables.v27, fragment, fmt, blend)
    return _lifted(tables, fragment, fmt, blend)


def _lifted(tables: caspec.Tables, fragment: str, fmt: int, blend: int) -> bool:
    s = caspec.decode(tables, 'P0000Xm_' + fragment)
    if tables.tfi_flags(s.T) >> 3 & 1:
        return False
    if blend == 3:
        return True
    info = tables.t['blend_function_info']
    float_target = fmt in (115, 125)
    if (float_target and info[blend] >> 4 & 1) or (blend & 0x3E) > 25:
        return False
    if s.flag('s'):
        if blend in (0, 12) or info[blend] >> 3 & 1:
            return False
    elif blend == 0 or (float_target and blend == 12):
        return False
    return (s.D & 7) == 0 or not info[blend] & 1


def with_fragment_blend(tables: caspec.Tables, fragment: str, blend: int) -> str:
    """Give the fragment segment with blend function `blend` (the name's segment order: T, B, D, A, S, L, X)."""
    code = {v: k for k, v in tables.dec['BlendFunction'].items()}[blend]
    head, _, images = fragment.partition('_')
    at = 4 if head.startswith('T') else 0
    return head[:at] + 'B' + code + head[at:] + ('_' + images if images else '')


def prefix_of(tables: caspec.Tables, fmt: str, blend: int, flags: str) -> str:
    code = {v: k for k, v in tables.dec['BlendFunction'].items()}
    return 'P' + fmt + ('B' + code[blend] if blend != tables.none_blend else '') + 'X' + flags


def _fragments(tables: caspec.Tables, seeds: list[str]) -> list[tuple[str, str]]:
    """Give the seeds' fragment states with no blend of their own, each with the first prefix that has it."""
    out: dict[str, str] = {}
    for name in seeds:
        prefix, fragment = split(name)
        if caspec.decode(tables, name).fragB == tables.none_blend:
            out.setdefault(fragment, prefix)
    return list(out.items())


def _formats(prefix: str) -> tuple[str, ...]:
    own = prefix[1:5]
    return tuple(dict.fromkeys((own, *CLOSURE_FORMATS) if own in SINGLE_DRAW_FORMATS else CLOSURE_FORMATS))


def _blend_tier(tables: caspec.Tables, fragments: list[tuple[str, str]]) -> list[str]:
    codes, names = tables.dec['MTLPixelFormat'], []
    for fragment, prefix in fragments:
        flags = ''.join(sorted(caspec.decode(tables, prefix + '_' + fragment).pflags))
        for fmt in _formats(prefix):
            blends = [b for b in LIFTABLE if lifted(tables, fragment, codes[fmt], b)]
            names += [prefix_of(tables, fmt, b, flags) + '_' + fragment for b in (tables.none_blend, *blends)]
    return names


def _fetch_tier(tables: caspec.Tables, fragments: list[tuple[str, str]]) -> list[str]:
    codes, names = tables.dec['MTLPixelFormat'], []
    for fragment, prefix in fragments:
        s = caspec.decode(tables, prefix + '_' + fragment)
        if tables.shader_type(s.T) != 0 or s.flag('g'):
            continue
        flags = ''.join(sorted(s.pflags))
        for fmt in _formats(prefix):
            head = prefix_of(tables, fmt, tables.none_blend, flags) + '_'
            blends = [b for b in range(tables.none_blend) if not lifted(tables, fragment, codes[fmt], b)]
            names += [head + with_fragment_blend(tables, fragment, b) for b in blends]
    return names


def labels(tables: caspec.Tables, corpus: list[str], tiers: tuple[str, ...] = TIERS) -> dict[str, list[str]]:
    """Give every tier's names, in order, each name in the first tier that has it."""
    manifest = [m[0] for m in tables.t['manifest'] if m[0][0] == 'P' and not caspec.decode(tables, m[0]).flag('g')]
    variants = _variant_tier(tables, list(corpus) + manifest)
    fragments = _fragments(tables, list(corpus) + manifest + variants)
    made = {
        'corpus': lambda: list(corpus),
        'manifest': lambda: manifest,
        'variants': lambda: variants,
        'blend': lambda: _blend_tier(tables, fragments),
        'fetch': lambda: _fetch_tier(tables, fragments),
    }
    seen: set[str] = set()
    out: dict[str, list[str]] = {}
    for tier in TIERS:
        if tier in tiers:
            out[tier] = [n for n in made[tier]() if not (n in seen or seen.add(n))]
    return out


def _variant_tier(tables: caspec.Tables, seeds: list[str]) -> list[str]:
    if not tables.v27:
        return []
    import caspec27  # noqa: PLC0415  (only a QuartzCore 27's tables have variant families)

    return [v for name in seeds for v in caspec27.variants(tables.v27, name)]


def corpus_names(path: str, tables: caspec.Tables | None = None) -> list[str]:
    """Give the corpus's Core Animation names; with `tables`, only those whose line this QuartzCore makes of the name.

    A corpus journaled under another IPSW's QuartzCore names states this one makes differently, or not at all.
    """
    names: dict[str, None] = {}
    with open(path) as f:
        for line in f:
            name, fields = caspec.corpus_fields(line)
            if name and (tables is None or _made_here(tables, name, fields)):
                names[name] = None
    return list(names)


def _made_here(tables: caspec.Tables, name: str, fields: str | None) -> bool:
    try:
        return caspec.state_fields(tables, name) == fields
    except (KeyError, IndexError, ValueError):
        return False


def main() -> None:
    tables = caspec.Tables(sys.argv[1])
    tiers = tuple(sys.argv[3:]) or TIERS
    for tier, names in labels(tables, corpus_names(sys.argv[2], tables), tiers).items():
        for name in names:
            print(caspec.state_line(tables, name))
        print(f'caset: {tier}: {len(names)} states', file=sys.stderr)


if __name__ == '__main__':
    main()
