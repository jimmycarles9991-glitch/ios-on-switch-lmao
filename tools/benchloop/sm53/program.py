"""Bench programs around a measured body: registers loaded from the shader data, results packed into one RGBA32F texel.

The shader data is bound at c[1]; the prologue moves word i into the i-th initial register. The epilogue writes the
two 32-bit results x and y as 0x3f800000 | (x & 0xffff), 0x3f800000 | (x >> 16) and the same for y: normal floats in
[1, 2), which the ROP stores bit for bit whatever x and y are (denormals, NaNs, integers). Delays outside the body are
15 and the body is followed by a wait on every barrier, so only the body's own scheduling is measured.
"""

import struct
from dataclasses import dataclass

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, operands, sph
from tools.benchloop.sm53.encode import ALL_BARRIERS, RZ, Cbuf, Instr
from tools.gpu.maxwell import execute
from tools.gpu.maxwell.interp import Inputs, UnsupportedError

DATA_BANK = 1
SAFE_DELAY = 15
ONE = 0x3F800000
OUTPUT_REGISTERS = 4
LOOP_COUNTER = 3
LOOP_PREDICATE = 6
FIXED_LATENCY = 6
# NAK's sm50 exec_latency for a branch
BRANCH_DELAY = 13
POSITION = (0x70, 0x74)
SEPARATOR = encode.nop().sched(delay=SAFE_DELAY, wait=ALL_BARRIERS)
# A kernel's output: its buffer's address is the shader data's first 8 bytes (the app writes it), the registers it
# loads follow from OUTPUT_DATA; each thread stores its two results at its thread index x OUTPUT_STRIDE
OUTPUT_DATA = 16
OUTPUT_STRIDE = 8
SR_TID_X = 0x21
STG_32 = 4


@dataclass(frozen=True)
class Program:
    code: bytes
    data: bytes
    gprs: int
    sph: bytes | None = None

    def wire(self) -> dict:
        """Give the program as a protocol experiment carries it."""
        out = {'code': protocol.b64(self.code), 'data': protocol.b64(self.data), 'gprs': self.gprs}
        if self.sph is not None:
            out['sph'] = protocol.b64(self.sph)
        return out


def _scheduled(instrs: list[Instr]) -> list[Instr]:
    """Give independent setup instructions issued a cycle apart, the last one SAFE_DELAY before what follows."""
    return [i.sched(delay=SAFE_DELAY if k == len(instrs) - 1 else 1) for k, i in enumerate(instrs)]


def prologue(init: dict[int, int], preds: dict[int, bool] | None = None, offset: int = 0) -> list[Instr]:
    moves = [encode.mov(r, Cbuf(DATA_BANK, offset + 4 * k)) for k, r in enumerate(sorted(init))]
    setps = [encode.isetp(p, RZ, RZ, 'eq' if v else 'ne') for p, v in sorted((preds or {}).items())]
    return _scheduled(moves + setps)


def data_of(init: dict[int, int]) -> bytes:
    words = [init[r] & encode.MASK32 for r in sorted(init)]
    words += [0] * (-len(words) % 4)
    return struct.pack(f'<{len(words)}I', *words)


def epilogue(results: tuple[int, int]) -> list[Instr]:
    if set(results) & set(range(OUTPUT_REGISTERS)):
        raise ValueError(f'results {results} would be overwritten by the outputs R0..R3')
    x, y = results
    split = [
        encode.lop32i(0, x, 0xFFFF, 'and'),
        encode.shr(1, x, encode.Imm(16)),
        encode.lop32i(2, y, 0xFFFF, 'and'),
        encode.shr(3, y, encode.Imm(16)),
    ]
    mark = [encode.lop32i(r, r, ONE, 'or') for r in range(OUTPUT_REGISTERS)]
    return _scheduled(split) + _scheduled(mark)


def settled(body: list[Instr]) -> list[Instr]:
    """Give `body` with its last instruction held until a barrier it sets can be waited on by what follows."""
    if not body:
        return body
    last = body[-1].control
    if (last.wr_bar, last.rd_bar) != (encode.NO_BAR, encode.NO_BAR) and last.delay < encode.BARRIER_SET_CYCLES:
        return [*body[:-1], body[-1].sched(delay=encode.BARRIER_SET_CYCLES)]
    return body


def looped(body: list[Instr], iterations: int, before: list[Instr]) -> list[Instr]:
    """Give `body` run `iterations` times: a counter in LOOP_COUNTER, LOOP_PREDICATE set at the top, BRA at the end.

    The predicate is written at the top so its 13 cycles pass inside the body; `before` is what precedes the loop, which
    the branch offset needs.
    """
    start = len(before) + 1
    top = [
        encode.mov32i(LOOP_COUNTER, iterations).sched(delay=SAFE_DELAY),
        encode.iadd32i(LOOP_COUNTER, LOOP_COUNTER, -1).sched(delay=FIXED_LATENCY),
        encode.isetp(LOOP_PREDICATE, LOOP_COUNTER, RZ, 'ne').sched(delay=1),
    ]
    at = start + len(top) - 1 + len(body)
    branch = encode.bra(encode.address(start) - encode.address(at) - 8, LOOP_PREDICATE).sched(delay=BRANCH_DELAY)
    return [*top, *body, branch]


def _gprs(code: bytes) -> int:
    return max(OUTPUT_REGISTERS, operands.highest_register(code) + 1)


def fragment(
    body: list[Instr],
    init: dict[int, int],
    results: tuple[int, int],
    preds: dict[int, bool] | None = None,
    loop: int | None = None,
) -> Program:
    """Give a fragment program: load `init` and `preds`, run `body` (`loop` times), write `results` to target 0."""
    head = prologue(init, preds)
    middle = looped(settled(body), loop, head) if loop else settled(body)
    code = encode.bundle(encode.finish([*head, *middle, SEPARATOR, *epilogue(results)]))
    return Program(code, data_of(init), _gprs(code), sph.pixel(operands.attributes(code)))


def compute(body: list[Instr], init: dict[int, int], preds: dict[int, bool] | None = None) -> Program:
    """Give the compute form of a body: the same prologue, no outputs."""
    code = encode.bundle(encode.finish([*prologue(init, preds), *settled(body), SEPARATOR]))
    return Program(code, data_of(init), _gprs(code))


def s2r(d: int, sr: int) -> Instr:
    return encode._Word('S2R').reg(0, d).put(20, 8, sr).instr()


def stg(addr: int, data: int, offset: int = 0) -> Instr:
    """Give STG.E.32 [addr + offset], data; the address the register pair addr, addr + 1."""
    w = encode._Word('STG').reg(0, data).reg(8, addr).put(20, 24, offset)
    return w.flag(45, True).put(48, 3, STG_32).instr()


def store_epilogue(results: tuple[int, int]) -> list[Instr]:
    """Give the stores of `results` to this thread's 8 bytes of the output buffer (R0..R3 are the address's)."""
    if set(results) & set(range(OUTPUT_REGISTERS)):
        raise ValueError(f'results {results} would be overwritten by the address R0..R3')
    carry_in, carry_out = 1 << 43, 1 << 47  # IADD's .X and .CC
    address = [
        s2r(2, SR_TID_X).sched(wr_bar=0, delay=encode.BARRIER_SET_CYCLES),
        encode.mov(0, Cbuf(DATA_BANK, 0)).sched(delay=1),
        encode.mov(1, Cbuf(DATA_BANK, 4)).sched(delay=SAFE_DELAY),
        encode.shl(3, 2, encode.Imm(3)).sched(wait=1, delay=SAFE_DELAY),
        Instr(encode.iadd(0, 0, 3).word | carry_out).sched(delay=SAFE_DELAY),
        Instr(encode.iadd(1, 1, RZ).word | carry_in).sched(delay=SAFE_DELAY),
    ]
    return [*address, stg(0, results[0]).sched(delay=1), stg(0, results[1], 4).sched(delay=1)]


def compute_out(
    body: list[Instr], init: dict[int, int], results: tuple[int, int], preds: dict[int, bool] | None = None
) -> Program:
    """Give a kernel that runs `body` as fragment() does and stores `results` to the output buffer, not a texel."""
    head = prologue(init, preds, OUTPUT_DATA)
    code = encode.bundle(encode.finish([*head, *settled(body), SEPARATOR, *store_epilogue(results)]))
    return Program(code, bytes(OUTPUT_DATA) + data_of(init), _gprs(code))


def inputs(data: bytes, position: tuple[int, int] | None = None) -> Inputs:
    """Give the interpreter's environment: `data` as c[1], `position` the pixel whose centre IPA reads at 0x70/0x74."""

    def cbuf(bank: int, offset: int) -> int:
        if bank != DATA_BANK or offset + 4 > len(data):
            return 0
        return struct.unpack_from('<I', data, offset)[0]

    def attribute(addr: int) -> int:
        if position is None or addr not in POSITION:
            return 0
        return encode.f32(position[POSITION.index(addr)] + 0.5)

    def texture(_: tuple) -> list[int]:
        raise UnsupportedError('bench programs sample no textures')

    return Inputs(attribute, cbuf, texture)


def expected_texel(code: bytes, data: bytes, position: tuple[int, int] | None = None) -> list[int]:
    """Give the four words a fragment program writes, as the host interpreter runs it."""
    regs, _ = execute.run(code, inputs(data, position), OUTPUT_REGISTERS)
    return regs


def pack(x: int, y: int) -> list[int]:
    return [ONE | (x & 0xFFFF), ONE | (x >> 16 & 0xFFFF), ONE | (y & 0xFFFF), ONE | (y >> 16 & 0xFFFF)]


def unpack(texel: list[int]) -> tuple[int, int]:
    w0, w1, w2, w3 = (w & 0xFFFF for w in texel)
    return w0 | w1 << 16, w2 | w3 << 16


def texel_hex(texel: list[int]) -> str:
    """Give a texel as the device reports it: the 16 bytes of the RGBA32F texel, little endian, in hex."""
    return struct.pack('<4I', *texel).hex()


def texel_words(text: str) -> list[int]:
    return list(struct.unpack('<4I', bytes.fromhex(text)))
