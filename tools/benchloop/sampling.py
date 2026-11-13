"""Pixel programs that read the bench's bound texture: the 3D side of a 2D scale's or resolve's compare.

Their TEX instructions are bound fetches whose texture index is the app's placeholder (bench_records.h
kTextureIndexPlaceholder): the app puts in the index of its template's texture handle and gives the program set 0 as a
constant buffer of its own. Coordinates are normalised, from the pixel's centre (IPA of the position), as the pack's mip
state maps them; the level is zero (TEX.LZ).
"""

import struct

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, operands, sph
from tools.benchloop.sm53.encode import RZ, Cbuf, Instr
from tools.benchloop.sm53.program import DATA_BANK, POSITION, SAFE_DELAY

PLACEHOLDER = 0x1FFF
TEX_BOUND = 0xC038  # TEX's bound form with its sparse predicate PT (NAK sm50.rs OpTex)
DIM_2D = 2
LOD_ZERO = 1
VARIABLE = {'wr_bar': 0, 'delay': encode.BARRIER_SET_CYCLES}


def variable(bar: int) -> dict:
    """Give a variable-latency instruction's scheduling on barrier `bar`: one barrier per result outstanding."""
    return {'wr_bar': bar, 'delay': encode.BARRIER_SET_CYCLES}


SETTLE = encode.nop().sched(delay=SAFE_DELAY, wait=encode.ALL_BARRIERS)


def tex2d(d: int, a: int, index: int = PLACEHOLDER, mask: int = 0xF) -> Instr:
    """Give TEX.LZ d, (a, a + 1), the bound texture `index`, 2D, writing the channels in `mask`."""
    word = TEX_BOUND << 48 | d | a << 8 | encode.PT << 16 | RZ << 20 | DIM_2D << 28 | mask << 31 | index << 36
    word |= LOD_ZERO << 55
    return Instr(word)


def _fixed(i: Instr) -> Instr:
    return i.sched(delay=SAFE_DELAY)


def _program(body: list[Instr], data: list[float]) -> dict:
    code = encode.bundle(encode.finish(body))
    words = [*data, *[0.0] * (-len(data) % 4)]
    return {
        'sph': protocol.b64(sph.pixel(operands.attributes(code))),
        'code': protocol.b64(code),
        'data': protocol.b64(struct.pack(f'<{len(words)}f', *words)),
        'gprs': max(8, operands.highest_register(code) + 1),
    }


def _position() -> list[Instr]:
    return [encode.ipa(4, POSITION[0]).sched(**variable(0)), encode.ipa(5, POSITION[1]).sched(**variable(1)), SETTLE]


def scale(width: int, height: int) -> dict:
    """Give the program that samples the texture at the pixel's centre over a `width` x `height` target."""
    body = [
        *_position(),
        _fixed(encode.fmul(4, 4, Cbuf(DATA_BANK, 0))),
        _fixed(encode.fmul(5, 5, Cbuf(DATA_BANK, 4))),
        tex2d(0, 4).sched(wr_bar=1, delay=encode.BARRIER_SET_CYCLES),
    ]
    return _program(body, [1 / width, 1 / height])


# average4's constants, in its shader data
AVERAGE_DATA = ('2/w', '2/h', '-0.5/w', '-0.5/h', '1/w', '1/h', '255', '1/255')


def average4(source_width: int, source_height: int) -> dict:
    """Give the 2x2-box average of an 8-bit unorm source twice the target's size, as (sum + 2) >> 2 of its codes.

    Pixel (x, y) reads texels (2x + i, 2y + j) at their centres with the nearest sampler; each channel's four values
    times 255 are rounded to their code sum, which is averaged in integers and written as code / 255, a value the ROP
    stores as that code.
    """
    c = [Cbuf(DATA_BANK, 4 * k) for k in range(len(AVERAGE_DATA))]
    coords = [
        _fixed(encode.mov(20, c[2])),
        _fixed(encode.mov(21, c[3])),
        _fixed(encode.ffma(8, 4, c[0], 20)),
        _fixed(encode.ffma(9, 5, c[1], 21)),
        _fixed(encode.fadd(10, 8, c[4])),
        _fixed(encode.mov(11, 9)),
        _fixed(encode.mov(12, 8)),
        _fixed(encode.fadd(13, 9, c[5])),
        _fixed(encode.mov(14, 10)),
        _fixed(encode.mov(15, 13)),
    ]
    fetches = [tex2d(24 + 4 * k, 8 + 2 * k).sched(**variable(2 + k)) for k in range(4)]
    channels = []
    for ch in range(4):
        channels += [
            _fixed(encode.fadd(40, 24 + ch, 28 + ch)),
            _fixed(encode.fadd(41, 32 + ch, 36 + ch)),
            _fixed(encode.fadd(40, 40, 41)),
            _fixed(encode.fmul(40, 40, c[6])),
            encode.f2i(40, 40).sched(**VARIABLE),
            SETTLE,
            _fixed(encode.iadd32i(40, 40, 2)),
            _fixed(encode.shr(40, 40, encode.Imm(2))),
            encode.i2f(ch, 40).sched(**VARIABLE),
            SETTLE,
            _fixed(encode.fmul(ch, ch, c[7])),
        ]
    w, h = source_width, source_height
    data = [2 / w, 2 / h, -0.5 / w, -0.5 / h, 1 / w, 1 / h, 255.0, 1 / 255]
    return _program([*_position(), *coords, *fetches, SETTLE, *channels], data)
