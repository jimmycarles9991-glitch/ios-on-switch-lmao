#!/usr/bin/env python3
"""The Core Animation states the raw pack holds, named as QuartzCore names them, and their journal R lines (caspec.py).

Tiers, each name in the first that has it:
  corpus     every Core Animation state the device's journals hold (precompile/corpus.log), by name
  manifest   QuartzCore's offline compilation manifest (CA::OGL::MetalContext::offline_compilation_manifest: the states
             Apple compiles ahead for its GPUs; warmup_shaders makes the ones of the device's GPU family at boot), each
             as Apple names it and as this device's QuartzCore does (deepcolour.py: Apple names them for its
             extended-colour devices). CA
             makes a generic state (g: the uber shader) only out of its binary archive, failing on a miss; the raw
             runtime answers such a creation from the pack as the device's archive would. QuartzCore 27 asks for every
             generic state of its manifest at boot (17 F "Metal failed to load render pipeline" lines each iOS 27 boot
             while they were left out); 21H461's are left out until a 17 boot shows the same
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
import deepcolour
import fetchfree

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


# QuartzCore sets a state's x (fc_extended_range) from its target alone, an extended-range format (MTLPixelFormat
# 550-555; get_pipeline_spec: 24B119 0x184388160, 21H461 0x18942f218): a fragment carried onto another format takes its.
EXTENDED_RANGE = range(550, 556)


def _on_format(tables: caspec.Tables, fragment: str, fmt: int) -> str:
    return deepcolour.fragment_with_flag(tables, fragment, 'x', fmt in EXTENDED_RANGE)


def _formats(prefix: str) -> tuple[str, ...]:
    own = prefix[1:5]
    return tuple(dict.fromkeys((own, *CLOSURE_FORMATS) if own in SINGLE_DRAW_FORMATS else CLOSURE_FORMATS))


def _blend_tier(tables: caspec.Tables, fragments: list[tuple[str, str]]) -> list[str]:
    codes, names = tables.dec['MTLPixelFormat'], []
    for fragment, prefix in fragments:
        flags = ''.join(sorted(caspec.decode(tables, prefix + '_' + fragment).pflags))
        for fmt in _formats(prefix):
            on = _on_format(tables, fragment, codes[fmt])
            blends = [b for b in LIFTABLE if lifted(tables, on, codes[fmt], b)]
            names += [prefix_of(tables, fmt, b, flags) + '_' + on for b in (tables.none_blend, *blends)]
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
            on = _on_format(tables, fragment, codes[fmt])
            blends = [b for b in range(tables.none_blend) if not lifted(tables, on, codes[fmt], b)]
            names += [head + with_fragment_blend(tables, on, b) for b in blends]
    return names


def labels(
    tables: caspec.Tables, corpus: list[str], tiers: tuple[str, ...] = TIERS, cif10: bool | None = None
) -> dict[str, list[str]]:
    """Give every tier's names, in order, each name in the first tier that has it.

    `cif10`: CADeviceUseCIF10 on the device (deepcolour.cif10), None where it is not known: each manifest state goes
    in as Apple names it and as the device's QuartzCore does (deepcolour.asked), so the tiers made of them hold the
    device's too.
    """
    manifest = [
        form
        for m in tables.t['manifest']
        if m[0][0] == 'P' and (tables.v27 or not caspec.decode(tables, m[0]).flag('g'))
        for form in dict.fromkeys([m[0], *deepcolour.asked(tables, m[0], cif10)])
    ]
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


# The fetch ladder's rung below the interlock a Core Animation state's fragment blend is a candidate for
# (runtime/raw_pack.h kFetchCandidateBlend, kFetchCandidateIterated): 2 where QuartzCore's blend function has a
# fixed-function equation (its table's 0-25), 3 where it has none (the iterated blend unit's), each only where nothing
# else reads the destination (no texture or image function that does)
FIXED_FUNCTION_BLENDS = fetchfree.FIXED_FUNCTION_BLENDS


def fetch_rung(tables: caspec.Tables, name: str) -> int:
    """Give the rung (2 or 3) a fragment blend of `name` is a candidate for, 0 for none."""
    try:
        s = caspec.decode(tables, name)
    except (KeyError, IndexError, ValueError):
        return 0
    if s.fragB == tables.none_blend or tables.tfi_flags(s.T) >> 1 & 1:
        return 0
    images = s.images[: getattr(s, 'decoded', len(s.images))]
    if any(tables.t['image_function_info'][image.I] >> 3 & 1 for image in images):
        return 0
    return 2 if s.fragB in FIXED_FUNCTION_BLENDS else 3


def with_fetch_rung(tables: caspec.Tables, line: str, why: dict[str, int] | None = None) -> str:
    """Give a Core Animation R line with its fragment blend's candidate rung after its label (" rung=<n>").

    A line whose state has a fetch-free form also carries its tag (fetchfree.tag); `why` counts the candidates' forms by
    rung and their reasons for having none.
    """
    name = caspec.corpus_fields(line)[0]
    rung = fetch_rung(tables, name) if name else 0
    if not rung:
        return line
    free, reason = fetchfree.tag(tables, name, line)
    if why is not None:
        key = f'rung {rung}: ' + (reason or f'form on rung {free[6]}')
        why[key] = why.get(key, 0) + 1
    return line.replace(' samples=', f' rung={rung}{free} samples=', 1)


def corpus_names(path: str, tables: caspec.Tables | None = None) -> list[str]:
    """Give the corpus's Core Animation names; with `tables`, only those this QuartzCore makes a state of.

    A name another IPSW's journal holds names a state this QuartzCore makes otherwise (its own constants and flags):
    that state, which its devices ask for under the same name, is the one made of it.
    """
    names: dict[str, None] = {}
    with open(path) as f:
        for line in f:
            name, _fields = caspec.corpus_fields(line)
            if name and (tables is None or makes(tables, name)):
                names[name] = None
    return list(names)


def makes(tables: caspec.Tables, name: str) -> bool:
    """Whether this QuartzCore makes a state of the name."""
    try:
        caspec.state_line(tables, name)
    except (KeyError, IndexError, ValueError):
        return False
    return True


def main() -> None:
    tables = caspec.Tables(sys.argv[1])
    tiers = tuple(sys.argv[3:]) or TIERS
    for tier, names in labels(tables, corpus_names(sys.argv[2], tables), tiers).items():
        for name in names:
            print(caspec.state_line(tables, name))
        print(f'caset: {tier}: {len(names)} states', file=sys.stderr)


if __name__ == '__main__':
    main()
