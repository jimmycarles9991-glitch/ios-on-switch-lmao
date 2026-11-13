"""Maxwell's texture fetches on the host: TEX and TLD (bindless and bound), TEXS and TLDS, as interp.py runs them.

A fetch is a pure function of a canonical key, so two programs that sample the same texel the same way get the same
result whatever form each uses: the handle word (a bindless one's register, a bound one's constant buffer word at its
index in the buffer SET_BINDLESS_TEXTURE names), the coordinates (an array's index first), the level mode (0 implicit,
1 zero, 2 bias, 3 level) and its value, the depth reference, the offsets and the dimension (TEX's encoding). Field
layouts are NAK's sm50.rs; TEXS's targets, masks and F16 packing those of the Switch emulators' decoder
(texture_fetch_swizzled.cpp). A TEXS.F16 result is the f32 one rounded to the nearest half, ties to even: the
assumption a pack may hold only once the device's probe has shown it (raw_chip_facts.h). A texel load (TLD, TLDS) is
keyed alike by its handle, integer coordinates, level, offsets, sample and dimension; TLDS's targets are nv50_ir's
getTLDSTarget's (NAK's sm50/texs.rs tlds_target), its operands where texture_load_swizzled.cpp reads them.
"""

from tools.gpu.maxwell.floats import F16, F32, RN, convert
from tools.gpu.maxwell.interp import RZ_REG, Inputs, Lane, UnsupportedError
from tools.gpu.maxwell.isa import field as bits

DIM_COORDS = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 6: 3, 7: 4}
LEVEL_OPERAND = (False, False, True, True)  # by level mode: implicit, zero, bias, level
DIM_1D, DIM_2D, DIM_A2D, DIM_3D, DIM_CUBE = 0, 2, 3, 4, 6
# TEXS's targets (bits 53-56): dimension, level mode, depth reference, then where each operand is (a: register A's
# first, a1: its second, b, b1 the same of register B)
TEXS_TARGETS = {
    0: (DIM_1D, 1, False, ('a',), None, None),
    1: (DIM_2D, 0, False, ('a', 'b'), None, None),
    2: (DIM_2D, 1, False, ('a', 'b'), None, None),
    3: (DIM_2D, 3, False, ('a', 'a1'), 'b', None),
    4: (DIM_2D, 0, True, ('a', 'a1'), None, 'b'),
    5: (DIM_2D, 3, True, ('a', 'a1'), 'b', 'b1'),
    6: (DIM_2D, 1, True, ('a', 'a1'), None, 'b'),
    7: (DIM_A2D, 0, False, ('a', 'a1', 'b'), None, None),
    8: (DIM_A2D, 1, False, ('a', 'a1', 'b'), None, None),
    9: (DIM_A2D, 1, True, ('a', 'a1', 'b'), None, 'b1'),
    10: (DIM_3D, 0, False, ('a', 'a1', 'b'), None, None),
    11: (DIM_3D, 1, False, ('a', 'a1', 'b'), None, None),
    12: (DIM_CUBE, 0, False, ('a', 'a1', 'b'), None, None),
    13: (DIM_CUBE, 3, False, ('a', 'a1', 'b'), 'b1', None),
}
R, G, B, A = 1, 2, 4, 8
RG_LUT = (R, G, B, A, R | G, R | A, G | A, B | A)
RGBA_LUT = (R | G | B, R | G | A, R | B | A, G | B | A, R | G | B | A)


# TLDS's targets (bits 53-56): dimension, level mode (1 zero, 3 level), where each coordinate is (an array's index
# first, alone in register A: nak_nir_lower_tex_sm50.c), then where its level, offsets and sample are
TLDS_TARGETS = {
    0x0: (DIM_1D, 1, ('a',), None, None, None),
    0x1: (DIM_1D, 3, ('a',), 'b', None, None),
    0x2: (DIM_2D, 1, ('a', 'b'), None, None, None),
    0x4: (DIM_2D, 1, ('a', 'a1'), None, 'b', None),
    0x5: (DIM_2D, 3, ('a', 'a1'), 'b', None, None),
    0x6: (DIM_2D, 1, ('a', 'a1'), None, None, 'b'),
    0x7: (DIM_3D, 1, ('a', 'a1', 'b'), None, None, None),
    0x8: (DIM_A2D, 1, ('a', 'b', 'b1'), None, None, None),
    0xC: (DIM_2D, 3, ('a', 'a1'), 'b', 'b1', None),
}


def reg(lane: Lane, r: int) -> int:
    return 0 if r == RZ_REG else lane.r(r)


def fetch(inputs: Inputs, handle: int, coords: tuple, mode: int, level, dref, offset, dim: int) -> list[int]:  # noqa: ANN001, PLR0913, PLR0917  the canonical key's parts
    return inputs.texture(('tex', handle, coords, mode, level, dref, offset, dim))


def load(inputs: Inputs, handle: int, coords: tuple, level, offset, sample, dim: int) -> list[int]:  # noqa: ANN001, PLR0913, PLR0917  the canonical key's parts
    return inputs.texture(('tld', handle, coords, level, offset, sample, dim))


def write_compact(lane: Lane, d: int, mask: int, result: list[int]) -> None:
    out = 0
    for c in range(4):
        if mask >> c & 1:
            lane.w(d + out, result[c])
            out += 1


def tex(name: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """TEX: bindless (TEX_b, the handle the first word of register B's operands) or bound (the handle at its index)."""
    if name not in ('TEX', 'TEX_b'):
        raise UnsupportedError(name)
    bindless = name == 'TEX_b'
    dim, mask = bits(w, 28, 3), bits(w, 31, 4)
    mode = bits(w, 37, 2) if bindless else bits(w, 55, 2)
    aoffi = bits(w, 36, 1) if bindless else bits(w, 54, 1)
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)
    coords = tuple(reg(lane, ra + k if ra != RZ_REG else RZ_REG) for k in range(DIM_COORDS[dim]))
    operands = [reg(lane, rb + k if rb != RZ_REG else RZ_REG) for k in range(5)]
    handle = operands.pop(0) if bindless else inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    level = operands.pop(0) if LEVEL_OPERAND[mode] else None
    offset = operands.pop(0) if aoffi else None
    dref = operands.pop(0) if bits(w, 50, 1) else None
    write_compact(lane, d, mask, fetch(inputs, handle, coords, mode, level, dref, offset, dim))


def tld(name: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """TLD: bindless (TLD_b, the handle the first word of register B's operands) or bound (the handle at its index)."""
    if name not in ('TLD', 'TLD_b'):
        raise UnsupportedError(name)
    dim, mask = bits(w, 28, 3), bits(w, 31, 4)
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)
    coords = tuple(reg(lane, ra + k if ra != RZ_REG else RZ_REG) for k in range(DIM_COORDS[dim]))
    operands = [reg(lane, rb + k if rb != RZ_REG else RZ_REG) for k in range(5)]
    handle = operands.pop(0) if name == 'TLD_b' else inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    level = operands.pop(0) if bits(w, 55, 1) else None
    offset = operands.pop(0) if bits(w, 35, 1) else None
    sample = operands.pop(0) if bits(w, 50, 1) else None
    write_compact(lane, d, mask, load(inputs, handle, coords, level, offset, sample, dim))


def txd(name: str, w: int, lane: Lane, inputs: Inputs, d: int) -> None:
    """TXD: a fetch at explicit derivatives, keyed as TEX's implicit one at the same coordinates.

    Register A's operands are the handle (TXD_b) then the coordinates, B's each axis's x and y derivative in turn (NAK's
    nak_nir_lower_tex.c); array and offset forms are not run. The model picks no level from derivatives (an implicit
    TEX's are its quad's), so two programs whose derivatives round apart by an ulp fetch alike, as the hardware would.
    """
    if name not in ('TXD', 'TXD_b'):
        raise UnsupportedError(name)
    dim, mask = bits(w, 28, 3), bits(w, 31, 4)
    axes = {DIM_1D: 1, DIM_2D: 2, DIM_3D: 3, DIM_CUBE: 3}.get(dim)
    if axes is None or bits(w, 35, 1):
        raise UnsupportedError(f'TXD dimension {dim} or offsets')
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)
    bindless = name == 'TXD_b'
    a = [reg(lane, ra + k if ra != RZ_REG else RZ_REG) for k in range(axes + bindless)]
    handle = a.pop(0) if bindless else inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    if rb == RZ_REG:
        raise UnsupportedError('TXD without derivatives')
    write_compact(lane, d, mask, fetch(inputs, handle, tuple(a), 0, None, None, None, dim))


def write_scalar(w: int, lane: Lane, result: list[int]) -> None:
    """Write a scalar form's components: its mask's in register A's pair, then B's; F16 (bit 59 clear) packs halves."""
    da, db = bits(w, 0, 8), bits(w, 28, 8)
    swizzle = bits(w, 50, 3)
    lut = RG_LUT if db == RZ_REG else RGBA_LUT
    if swizzle >= len(lut):
        raise UnsupportedError(f'scalar texture mask {swizzle}')
    picked = [result[c] for c in range(4) if lut[swizzle] >> c & 1]
    targets = [da, da + 1, db, db + 1]
    if bits(w, 59, 1):
        for k, v in enumerate(picked):
            lane.w(targets[k], v)
        return
    halves = [convert(v, F32, F16, RN) for v in picked] + [0] * (4 - len(picked))
    lane.w(da, halves[0] | halves[1] << 16)
    if len(picked) > 2:
        lane.w(db, halves[2] | halves[3] << 16)


def tlds(w: int, lane: Lane, inputs: Inputs) -> None:
    """TLDS: its operands in registers A and B as its target places them, its components as TEXS writes them."""
    target = bits(w, 53, 4)
    if target not in TLDS_TARGETS:
        raise UnsupportedError(f'TLDS target {target}')
    dim, _, where_coords, where_level, where_offset, where_sample = TLDS_TARGETS[target]
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)

    def at(place: str | None):  # noqa: ANN202  an operand's word, or None where the target has none
        if place is None:
            return None
        base = ra if place[0] == 'a' else rb
        return reg(lane, base + len(place) - 1 if base != RZ_REG else RZ_REG)

    handle = inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    coords = tuple(at(p) for p in where_coords)
    write_scalar(w, lane, load(inputs, handle, coords, at(where_level), at(where_offset), at(where_sample), dim))


def texs(w: int, lane: Lane, inputs: Inputs) -> None:
    """TEXS: its operands in registers A and B as its target places them, its components in A's pair then B's."""
    target = bits(w, 53, 4)
    if target not in TEXS_TARGETS:
        raise UnsupportedError(f'TEXS target {target}')
    dim, mode, _, where_coords, where_level, where_dref = TEXS_TARGETS[target]
    ra, rb = bits(w, 8, 8), bits(w, 20, 8)

    def at(place: str | None):  # noqa: ANN202  an operand's word, or None where the target has none
        if place is None:
            return None
        base = ra if place[0] == 'a' else rb
        return reg(lane, base + len(place) - 1 if base != RZ_REG else RZ_REG)

    handle = inputs.cbuf(inputs.bound_cbuf, bits(w, 36, 13) * 4)
    coords = tuple(at(p) for p in where_coords)
    write_scalar(w, lane, fetch(inputs, handle, coords, mode, at(where_level), at(where_dref), None, dim))
