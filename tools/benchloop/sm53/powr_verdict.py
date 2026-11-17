"""Whether the native composition already gives fast::powr's answers, from powr_native.py's results.

Metal's rule is math.rs fast_powr_special as metal2vulkan's powr_specials_test reads it from the bits (tests/powr.rs
Width::fast_powr): a NaN where x is below zero, where y is a zero and x a zero or an infinity, and where y is infinite
and x is one; +0 or +inf where log2(x) or y is infinite and neither is a NaN; the composition everywhere else. The
chain programs hold every special pair of the state's domain to it, through the state's own narrowing to a half. The
per-op checks say which result a miss comes from, and decide the float form, which no chain here runs.
"""

from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field

from tools.benchloop.sm53 import powr_native


@dataclass(frozen=True)
class Width:
    sign: int
    inf: int
    one: int
    nan: int
    # Metal reads a magnitude below this as a zero: a half's zero alone, an f32's whole exponent field 0
    zero_below: int

    def magnitude(self, b: int) -> int:
        return b & (self.sign - 1)

    def is_nan(self, b: int) -> bool:
        return self.magnitude(b) > self.inf

    def is_inf(self, b: int) -> bool:
        return self.magnitude(b) == self.inf

    def is_zero(self, b: int) -> bool:
        return self.magnitude(b) < self.zero_below

    def negative(self, b: int) -> bool:
        return bool(b & self.sign)

    def fast_powr(self, x: int, y: int) -> int | None:
        """Give Metal's fast::powr(x, y) where its special rules fix it, None where the composition stands."""
        below = self.negative(x) and not self.is_zero(x) and not self.is_nan(x)
        zero_times_inf = self.is_zero(y) and (self.is_zero(x) or self.is_inf(x))
        if below or zero_times_inf or (self.is_inf(y) and x == self.one):
            return self.nan
        numbers = not self.is_nan(x) and not self.is_nan(y)
        if numbers and (self.is_zero(x) or self.is_inf(x) or self.is_inf(y)):
            return 0 if (self.magnitude(x) < self.one) != self.negative(y) else self.inf
        return None


HALF = Width(0x8000, 0x7C00, 0x3C00, 0x7E00, 1)
F32 = Width(0x8000_0000, 0x7F80_0000, 0x3F80_0000, 0x7FC0_0000, 0x0080_0000)
NEG_INF = F32.sign | F32.inf
FIRST = 8


@dataclass
class Check:
    name: str
    scope: str
    deciding: bool = True
    checked: int = 0
    off: int = 0
    first: list = field(default_factory=list)

    def see(self, ok: bool, **what: int) -> None:
        self.checked += 1
        if not ok:
            self.off += 1
            if len(self.first) < FIRST:
                self.first.append({k: f'{v:#x}' for k, v in what.items()})

    @property
    def holds(self) -> bool:
        return self.checked > 0 and self.off == 0

    def row(self) -> dict:
        return {k: getattr(self, k) for k in ('scope', 'deciding', 'checked', 'off', 'first')}


def _texels(planned: list[dict], results: dict[str, dict], name: str, missing: list[str]) -> Iterator[tuple]:
    """Give a program's texels as ((data word, table word), (first result, second result)); note the ids missing."""
    for x in (x for x in planned if x['meta'].get('program') == name):
        outs = powr_native.outputs(x, results.get(x['id']))
        if outs is None:
            missing.append(x['id'])
            continue
        yield from zip(powr_native.inputs(x), outs, strict=True)


def _positive_number(b: int) -> bool:
    return not F32.negative(b) and not F32.is_nan(b) and not F32.is_inf(b) and not F32.is_zero(b)


def _lg2_checks(lg2: dict, halves: set) -> list[Check]:
    exact = {
        'LG2(+0) = -inf': (0, NEG_INF),
        'LG2(-0) = -inf': (F32.sign, NEG_INF),
        'LG2(+inf) = +inf': (F32.inf, F32.inf),
    }
    out = []
    for name, (a, want) in exact.items():
        c = Check(name, 'half')
        if (a, 0) in lg2:
            c.see(lg2[a, 0][0] == want, a=a, got=lg2[a, 0][0])
        out.append(c)
    one = Check('LG2(1) = 0', 'half')
    if (F32.one, 0) in lg2:
        one.see(F32.magnitude(lg2[F32.one, 0][0]) == 0, got=lg2[F32.one, 0][0])
    out.append(one)
    for scope in ('half', 'float'):
        neg, sign = Check('LG2(x < 0) = NaN', scope), Check('LG2(x) normal with the sign of x - 1', scope)
        denormal = Check('LG2(f32 denormal) = -inf', scope)
        for (a, _), (got, _) in lg2.items():
            if (a in halves) != (scope == 'half'):
                continue
            if F32.negative(a) and not F32.is_zero(a) and not F32.is_nan(a):
                neg.see(F32.is_nan(got), a=a, got=got)
            elif _positive_number(a) and a != F32.one:
                normal = not F32.is_nan(got) and not F32.is_zero(got)
                sign.see(normal and F32.negative(got) == (a < F32.one), a=a, got=got)
            elif scope == 'float' and F32.is_zero(a) and F32.magnitude(a):
                denormal.see(got == NEG_INF, a=a, got=got)
        out += [neg, sign] + ([denormal] if scope == 'float' else [])
    return out


def _ex2_checks(lg2: dict) -> list[Check]:
    low, high = Check('EX2(RRO(-inf)) = +0', 'half'), Check('EX2(RRO(+inf)) = +inf', 'half')
    nan = Check('EX2(RRO(NaN)) = NaN', 'half')
    for (a, _), (_, got) in lg2.items():
        if a == NEG_INF:
            low.see(got == 0, got=got)
        elif a == F32.inf:
            high.see(got == F32.inf, got=got)
        elif F32.is_nan(a):
            nan.see(F32.is_nan(got), a=a, got=got)
    return [low, high, nan]


def _fmul_checks(fmul: dict, halves: set) -> list[Check]:
    inf_zero = Check('FMUL.FTZ(±inf, ±0) = NaN', 'half')
    inf_y = {s: Check('FMUL.FTZ(±inf, y) = ±inf', s) for s in ('half', 'float')}
    nan_y = Check('FMUL.FTZ(NaN, y) = NaN', 'half')
    inf_denormal = Check('FMUL.FTZ(±inf, f32 denormal) = NaN', 'float')
    zero_y = Check('FMUL.FTZ(±0, y) = ±0', 'half', deciding=False)
    for (a, s), products in fmul.items():
        both = {'a': a, 's': s, 'got': products[0], 'swapped': products[1]}
        pair = (a, s)
        if any(F32.is_nan(v) for v in pair):
            nan_y.see(all(F32.is_nan(p) for p in products), **both)
        elif any(F32.is_inf(v) for v in pair) and any(F32.magnitude(v) == 0 for v in pair):
            inf_zero.see(all(F32.is_nan(p) for p in products), **both)
        elif any(F32.is_inf(v) for v in pair) and any(F32.is_zero(v) for v in pair):
            inf_denormal.see(all(F32.is_nan(p) for p in products), **both)
        elif F32.is_inf(s):
            want = F32.inf | (F32.sign if F32.negative(a) != F32.negative(s) else 0)
            inf_y['half' if a in halves else 'float'].see(all(p == want for p in products), **both)
        elif F32.magnitude(s) == 0 and not F32.is_inf(a) and not F32.is_zero(a):
            want = F32.sign if F32.negative(a) != F32.negative(s) else 0
            zero_y.see(all(p == want for p in products), **both)
    return [inf_zero, *inf_y.values(), nan_y, inf_denormal, zero_y]


def _narrow(narrow: dict) -> dict:
    """Give the half 0x7fc00000 narrows to (what the translation's own NaN becomes) and whether every NaN does too."""
    canonical = narrow.get((F32.nan, 0), (0, None))[1]
    c = Check('F2F.F16(NaN) = F2F.F16(0x7fc00000)', 'half')
    if canonical is not None:
        for (a, _), (_, got) in narrow.items():
            if F32.is_nan(a):
                c.see(got & 0xFFFF == canonical & 0xFFFF, a=a, got=got)
    return {'nan_half': None if canonical is None else canonical & 0xFFFF, 'check': c}


def _chain(texels: Iterable[tuple], name: str, nan_half: int | None) -> dict:
    """Hold every special pair of a chain program to Metal's rule: the half as a class, its bits, and the f32."""
    row = {'pairs': 0, 'special': 0, 'off': 0, 'off_bits': 0, 'off_metal_bits': 0, 'off_f32': 0, 'first': []}
    for (data, table), (r32, r16) in texels:
        row['pairs'] += 1
        x, y = (data, table) if name == 'base' else (table, data)
        want = HALF.fast_powr(x, y)
        if want is None:
            continue
        row['special'] += 1
        got = r16 & 0xFFFF
        nan = want == HALF.nan
        same_class = HALF.is_nan(got) if nan else got == want
        want32 = F32.nan if nan else (F32.inf if want else 0)
        row['off_f32'] += not (F32.is_nan(r32) if nan else r32 == want32)
        row['off_metal_bits'] += got != want
        row['off_bits'] += nan_half is None or got != (nan_half if nan else want)
        if not same_class:
            row['off'] += 1
            if len(row['first']) < FIRST:
                row['first'].append({'x': f'{x:#06x}', 'y': f'{y:#06x}', 'half': f'{got:#06x}', 'f32': f'{r32:#010x}'})
    return row


def analyse(planned: list[dict], results: dict[str, dict]) -> dict:
    planned = [x for x in planned if x['meta'].get('family') == 'powr-native']
    halves = set(powr_native.every_half())
    missing: list[str] = []
    got = {name: dict(_texels(planned, results, name, missing)) for name in ('lg2', 'narrow', 'fmul')}
    narrowed = _narrow(got['narrow'])
    checks = [
        *_lg2_checks(got['lg2'], halves),
        *_ex2_checks(got['lg2']),
        *_fmul_checks(got['fmul'], halves),
        narrowed['check'],
    ]
    before = len(missing)
    chains = {
        name: _chain(_texels(planned, results, name, missing), name, narrowed['nan_half'])
        for name in ('base', 'exponent')
    }
    whole = len(missing) == before
    deciding = [c for c in checks if c.deciding and c.name != narrowed['check'].name]
    return {
        'missing': missing,
        'nan_half': narrowed['nan_half'],
        'checks': {f'{c.name} [{c.scope}]': c.row() for c in checks},
        'chain': chains,
        'half': whole and all(r['special'] and not r['off'] for r in chains.values()),
        'half_bits': whole and all(r['special'] and not r['off_bits'] for r in chains.values()),
        'float': all(c.holds for c in deciding),
    }


def _yes(v: bool) -> str:
    return 'yes' if v else 'no'


def lines(found: dict) -> list[str]:
    out = []
    for name, c in found['checks'].items():
        state = 'missing' if not c['checked'] else ('ok' if not c['off'] else f'OFF {c["off"]}')
        tail = f' first {c["first"][0]}' if c['off'] else ''
        out.append(f'powr {name} {state} of {c["checked"]}{"" if c["deciding"] else " (recorded)"}{tail}')
    nan = found['nan_half']
    out.append(f'powr F2F.F16(0x7fc00000) = {"-" if nan is None else f"{nan:#06x}"}')
    for name, r in found['chain'].items():
        head = f'powr chain {name} {r["pairs"]} pairs, {r["special"]} special, {r["off"]} off fast::powr'
        bits = f'{r["off_bits"]} off the translation bit for bit, {r["off_metal_bits"]} off 0x7e00/0/0x7c00'
        out.append(f'{head}, {bits}, {r["off_f32"]} f32 off' + (f' first {r["first"][0]}' if r['first'] else ''))
    if found['missing']:
        out.append(f'powr missing {len(found["missing"])}: {" ".join(found["missing"])}')
    out.append(
        f'powr verdict half (state 4c33d27bde9b894a): native chain is fast::powr {_yes(found["half"])}, '
        f'bit for bit after the narrowing {_yes(found["half_bits"])}; '
        f'float form per-op checks hold {_yes(found["float"])}'
    )
    return out
