# ruff: noqa: N803, N806  S, D, K, C, B, L and A are the names QuartzCore's and the W3C's blend formulas use
"""QuartzCore 24B119's blend formulas in exact arithmetic.

From fixed_frag_lph_cph (out/agent-scratch/x1-glass/blend/frag_cph.ll), including the tail (IR:3690-3729): alpha
saturate and rgb <= a for codes 32, 33, 37, 38, 40. S and D are premultiplied (r, g, b, a). Returns the colour before
the render-target store.
"""

from collections.abc import Mapping, Sequence

from ucode import half_value

E = half_value(0x068E)
W601 = tuple(half_value(b) for b in (0x34CD, 0x38B8, 0x2F0A))
W709 = tuple(half_value(b) for b in (0x32CD, 0x39B9, 0x2C9D))


def sat(x: float) -> float:
    return min(1.0, max(0.0, x))


def dot(c: Sequence[float], w: Sequence[float]) -> float:
    return c[0] * w[0] + c[1] * w[1] + c[2] * w[2]


def separable_f(  # noqa: C901, PLR0911, PLR0912  one return per separable mode
    mode: str, s: float, sa: float, d: float, da: float
) -> float:
    if mode == 'ovl':
        return 2 * d * s if d < 0.5 * da else 2 * ((sa - s) * d + s * da) - sa * da
    if mode == 'drk':
        return min(sa * d, s * da)
    if mode == 'ltn':
        return max(sa * d, s * da)
    if mode == 'cdg':
        return min(sa, sa * sa * d / max(sa - s, E))
    if mode == 'cbn':
        return (sa * da - sa * sa * (da - d) / max(s, E)) if s >= E else 0.0
    if mode == 'slt':
        return 2 * s * d - (2 * s - sa) * d * d / max(da, E)
    if mode == 'hlt':
        return 2 * d * s if s < 0.5 * sa else sa * da - 2 * (da - d) * (sa - s)
    if mode == 'dif':
        return abs(sa * d - s * da)
    if mode == 'sbt':
        return s * da - sa * d
    if mode == 'div':
        return s * da / max(sa * d, E)
    if mode == 'lbn':
        return sa * d - (sa - s) * da
    if mode == 'llt':
        return sa * d - (sa - 2 * s) * da
    if mode == 'pin':
        if s >= 0.5 * sa:
            return max(sa * d, 2 * da * s - sa * da)
        return min(sa * d, 2 * da * s)
    raise KeyError(mode)


SEPARABLE = ('ovl', 'drk', 'ltn', 'cdg', 'cbn', 'slt', 'hlt', 'dif', 'sbt', 'div', 'lbn', 'llt', 'pin')
CLAMPED = ('cdg', 'cbn', 'sbt', 'div', 'llt')


def w3c(mode: str, S: Sequence[float], D: Sequence[float], flags: str) -> tuple[float, ...]:
    sa, da = S[3], D[3]
    a = sat((1 - sa) * da + sa * (1 - da) + sa * da)
    rgb = [separable_f(mode, S[i], sa, D[i], da) + (1 - sa) * D[i] + S[i] * (1 - da) for i in range(3)]
    if mode in CLAMPED and 'u' not in flags:
        limit = a * 1.25 if 'x' in flags else a
        rgb = [min(c, limit) for c in rgb]
    return (*rgb, a)


def set_lum(c: Sequence[float], lum: float) -> list[float]:
    d = lum - dot(c, W601)
    C = [x + d for x in c]
    l2 = dot(C, W601)
    mx, mn = max(C), min(C)
    if mx > 1 or mn < 0:
        s = l2 / max(l2 - mn, E) if d < 0 else (1 - l2) / max(mx - l2, E)
        C = [l2 + (x - l2) * s for x in C]
    return C


def set_sat(c: Sequence[float], s: float) -> list[float]:
    mx, mn = max(c), min(c)
    if not mx > mn:
        return [0.0, 0.0, 0.0]
    imax = c.index(mx)
    rest = [0.0 if i == imax else c[i] for i in range(3)]
    mid = sorted(c)[1]
    imid = rest.index(mid)
    out = [0.0, 0.0, 0.0]
    out[imid] = (mid - mn) * s / (mx - mn)
    out[imax] = s
    return out


def hsl(mode: str, S: Sequence[float], D: Sequence[float]) -> tuple[float, ...]:
    sa, da = S[3], D[3]
    s = [x / max(sa, E) for x in S[:3]]
    d = [x / max(da, E) for x in D[:3]]
    if mode == 'hue':
        B = set_lum(set_sat(s, max(d) - min(d)), dot(d, W601))
    elif mode == 'sat':
        B = set_lum(set_sat(d, max(s) - min(s)), dot(d, W601))
    elif mode == 'col':
        B = set_lum(s, dot(d, W601))
    else:
        B = set_lum(d, dot(s, W601))
    rgb = [B[i] * sa * da + (1 - sa) * D[i] + S[i] * (1 - da) for i in range(3)]
    return (*rgb, sat((1 - sa) * da + sa * (1 - da) + sa * da))


def dlc(S: Sequence[float], D: Sequence[float], flags: str) -> tuple[float, ...]:
    ls, ld = dot(S, W601), dot(D, W601)
    if not ls < ld:
        return tuple(D)
    d = ls - ld
    C = [x + d for x in D[:3]]
    if 'u' not in flags:
        l2 = dot(C, W601)
        mx, mn = max(C), min(C)
        if mx > 1 or mn < 0:
            s = l2 / max(l2 - mn, E) if d < 0 else (1 - l2) / max(mx - l2, E)
            C = [l2 + (x - l2) * s for x in C]
    return (*C, D[3])


def vcm(
    S: Sequence[float], D: Sequence[float], m: Sequence[Sequence[float]], p3: float, p4: float
) -> tuple[float, ...]:
    da = D[3]
    u = [x / max(da, E) for x in D[:3]]
    v = [u[0] * m[0][i] + u[1] * m[1][i] + u[2] * m[2][i] + m[3][i] * da + m[4][i] for i in range(4)]
    a = sat(v[3])
    c = [v[0] * a * S[3], v[1] * a * S[3], v[2] * a * S[3], a * S[3]]
    if p3 > 0:
        un = [x / max(c[3], E) for x in c[:3]]
        if p4 > 0:
            mx = max(un)
            if mx > p3:
                un = [p3 / mx * x for x in un]
        else:
            un = [min(max(x, -0.5), p3) for x in un]
        c[:3] = [x * c[3] for x in un]
    return tuple(c[i] + (1 - c[3]) * D[i] for i in range(4))


def sover_by_luma(S: Sequence[float], D: Sequence[float], lighten: bool) -> tuple[float, ...]:
    L = dot(D, W709)
    k = sat(1 - L if lighten else L) ** 4
    s = [x * k for x in S]
    return tuple(s[i] + (1 - s[3]) * D[i] for i in range(4))


def cli(S: Sequence[float], D: Sequence[float], K: Mapping[int, Sequence[float]]) -> tuple[float, ...]:
    v = [D[1] * K[1][i] + K[6][0] + D[0] * K[0][i] + D[2] * K[2][i] for i in range(3)] + [D[3]]
    v = [x * S[0] for x in v]
    rgb = [min(max(x, -0.75 * v[3]), v[3] * 1.0864000320434570) for x in v[:3]]
    rgb = [rgb[i] + (1 - v[3]) * D[i] for i in range(3)]
    a1 = v[3] + (1 - v[3]) * D[3]
    t = a1 + S[1]
    A = sat(t)
    rgb = [x + (A - t) for x in rgb]
    w = [rgb[1] * K[4][i] + K[6][1] + rgb[0] * K[3][i] + rgb[2] * K[5][i] for i in range(3)] + [A]
    w = [x * S[2] for x in w]
    out = [min(max(x, -0.75 * w[3]), w[3] * 1.0864000320434570) for x in w[:3]]
    out = [out[i] + (1 - w[3]) * rgb[i] for i in range(3)]
    return (*out, sat(w[3] + (1 - w[3]) * A))


def apple(  # noqa: C901, PLR0911  one return per blend mode
    name: str,
    S: Sequence[float],
    D: Sequence[float],
    params: tuple[Sequence[Sequence[float]], float, float] | Mapping[int, Sequence[float]] | None = None,
) -> tuple[float, ...]:
    mode, _, flags = name.partition('.')
    sa, da = S[3], D[3]
    if mode in SEPARABLE:
        return w3c(mode, S, D, flags)
    if mode == 'mpy':
        return (*(S[i] * (1 - da) + ((1 - sa) + S[i]) * D[i] for i in range(3)), sat(sa * (1 - da) + da))
    if mode == 'pdi':
        return (*(S[i] + D[i] - sa for i in range(3)), da)
    if mode == 'pld':
        a = min(1.0, sa + da)
        return (*(S[i] + D[i] + a - (sa + da) for i in range(3)), a)
    if mode == 'sds':
        return (*(S[i] * da + (1 - sa * da) * D[i] for i in range(3)), 1.0)
    if mode == 'adc':
        limit = max(max(D[:3]) + half_value(0x3117), half_value(0x3CA3))
        return (*(min(S[i] + D[i], limit) for i in range(3)), sat(sa + da))
    if mode in ('dso', 'lso'):
        return sover_by_luma(S, D, mode == 'lso')
    if mode == 'dlc':
        return dlc(S, D, flags)
    if mode in ('hue', 'sat', 'col', 'lum'):
        return hsl(mode, S, D)
    if mode == 'vcm':
        return vcm(S, D, *params)
    if mode == 'cli':
        return cli(S, D, params)
    raise KeyError(name)
