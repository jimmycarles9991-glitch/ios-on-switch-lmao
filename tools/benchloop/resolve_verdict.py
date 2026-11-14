"""What batch resolvetie2's rows decide (resolve_ties.analyse), as lines; each names the code it flips.

A complemented cell's models are read in the complement's frame: the engine's own rounding fits there (down for one
rounding, pair_*_down for two steps), and metal_mismatches is the texels against Metal's rule.

Controls first: a cell's form must be the one it asked for, a NOTSRCCOPY copy and a point read of the complemented
grid exact, the NOTSRCCOPY-only reading the complement of its plain twin, and a point read one sample. A failed
control decides nothing else.
"""

FORMATS = ('bgra8unorm', 'rgba8unorm')


def _failed_control(r: dict) -> bool:
    control = r['reading'] in ('not1to1', 'invpoint', 'notonly') or 'point_sample' in r
    return control and (
        r.get('status') != 'ok'
        or not r.get('form_ok')
        or r.get('exact') is False
        or r.get('complements_prod') is False
        or ('point_sample' in r and r['point_sample'] is None)
    )


def _zero(rows: list[dict], name: str) -> bool:
    return bool(rows) and all(r.get('models', {}).get(name, 1) == 0 for r in rows)


def _exact_compare(r: dict) -> bool:
    histogram = (r.get('compare') or {}).get('histogram') or {}
    return bool(histogram) and {k for k, v in histogram.items() if v} <= {'0'}


def rounding(fmt: str, n: int, prod: list[dict]) -> str:
    if not prod or any(r.get('status') != 'ok' or 'models' not in r for r in prod):
        return f'{fmt} {n}x: the production reading did not run ({[r["id"] for r in prod]})'
    if _zero(prod, 'down'):
        return f'{fmt} {n}x: one ties-down rounding of the mean, the store rule' + (
            ' (4x: one rounding, not two 2:1 steps)' if n == 4 else ''
        )
    if _zero(prod, 'up'):
        return f"{fmt} {n}x: already Metal's rule"
    for pairing in ('x', 'y'):
        if n == 4 and _zero(prod, f'pair_{pairing}_down'):
            return f'{fmt} 4x: two rounded 2:1 steps, {pairing} pairs first'
    if _zero(prod, 'trunc'):
        return f'{fmt} 4x: the mean truncated'
    return f'{fmt} {n}x: no model fits; misses {prod[0]["models"]}, residues {prod[0]["residues"]}'


def complement(fmt: str, n: int, sandwich: list[dict]) -> str:
    if not sandwich or any(r.get('status') != 'ok' or 'metal_mismatches' not in r for r in sandwich):
        return f'{fmt} {n}x: the complement route did not run ({[r["id"] for r in sandwich]})'
    if all(r['metal_mismatches'] == 0 and _exact_compare(r) for r in sandwich):
        flip = ': twodResolveSingleRounding4x() true (raw_resolve_rule.h)' if n == 4 else ''
        return f"{fmt} {n}x: the complement route is exact, every texel the resolve pass's{flip}"
    if n == 4 and any(_zero(sandwich, f'pair_{p}_down') for p in ('x', 'y')):
        return f'{fmt} 4x: two steps, so the complement route is within one code, not exact; 4x keeps resolve.twod'
    misses = [r['metal_mismatches'] for r in sandwich]
    return f'{fmt} {n}x: the complement route misses Metal on {misses} channels; models {sandwich[0]["models"]}'


def sweep(n: int, rows: list[dict]) -> str:
    ran = [r for r in rows if r.get('status') == 'ok' and 'models' in r]
    metal = [r['id'] for r in ran if r['models'].get('up') == 0]
    if metal:
        return f'{n}x: these settings give (sum + n/2)/n directly: {", ".join(metal)}'
    return (
        f"{n}x: no sample mode, filter or origin of {len(ran)} run gives Metal's rule ({len(rows) - len(ran)} not run)"
    )


def verdict(rows: list[dict]) -> list[str]:
    failed = [r['id'] for r in rows if _failed_control(r)]
    if failed:
        return [f'controls failed ({", ".join(failed)}): the grid reading or the ROP is wrong; nothing else is decided']
    lines = []
    for fmt in FORMATS:
        for n in (2, 4):
            mine = [r for r in rows if r['id'].startswith(f'rt-{fmt}-{n}x-')]
            prod = [r for r in mine if '-prod' in r['id']]
            lines.append(rounding(fmt, n, prod))
            lines.append(complement(fmt, n, [r for r in mine if '-sandwich' in r['id']]))
            lines.extend(
                f'{r["id"]}: against the resolve pass {(r.get("compare") or {}).get("histogram")}'
                for r in mine
                if r['id'].endswith('-drv')
            )
    lines.extend(
        sweep(n, [r for r in rows if r['id'].startswith(f'rt-sweep{n}-') and 'point_sample' not in r]) for n in (2, 4)
    )
    return lines
