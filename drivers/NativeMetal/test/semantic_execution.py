#!/usr/bin/env python3
import argparse
import itertools
import math
import random
import re
import struct
from collections.abc import Callable
from pathlib import Path
from typing import ClassVar, NamedTuple

MASK = (1 << 32) - 1
SOURCE, TARGET = 0x100000000, 0x200000000
EXIT = -1
LITERALS = {'rZ': 0, 'pT': 1, 'pF': 0}
NO_EFFECT = ('nop', 'ssy', 'pbk', 'pcnt', 'bar.sync', 'membar.sc.cta')
BRANCHES = ('bra', 'sync', 'brk', 'cont')
CONSTANT_REFERENCE = re.compile(r'c\[(0x[0-9a-f]+)\]\[(0x[0-9a-f]+)\]')
REGISTER_RANGE = re.compile(r'r(\d+)\.\.(\d+)')

Program = tuple[dict[int, tuple[list[str], list[int]]], dict[str, int]]
Texels = dict[tuple[int, int, int], tuple[int, ...]]


class Dispatch(NamedTuple):
    index: int
    source: bytes
    before: bytes
    src_offset: int
    dst_offset: int
    src_length: int
    dst_length: int


class Extras(NamedTuple):
    constant_words: dict[tuple[int, int], int] | None = None
    system_values: dict[str, int] | None = None
    texture_words: Texels | None = None


def f32(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


def bits(number: float) -> int:
    return struct.unpack('<I', struct.pack('<f', number))[0]


def ftz(word: int) -> int:
    return word if word & 0x7F800000 else word & 0x80000000


def half(word: int) -> int:
    return bits(struct.unpack('<e', struct.pack('<H', word & 0xFFFF))[0])


def program(log: str, name: str, route: str) -> Program:
    text = log.split(f'compile semantic_{name} {route}\n', 1)[1]
    text = text.split('NAK IR after calc_instr_deps:\n', 1)[1].split('\n\n', 1)[0]
    blocks, labels = {}, {}
    for line in text.splitlines():
        if match := re.match(r'block(?:\.u)? (\d+) (L\d+) .* -> \{', line):
            block = int(match[1])
            labels[match[2]] = block
            blocks[block] = ([], [])
        elif line.startswith('} -> '):
            blocks[block][1].extend(map(int, re.findall(r'\d+', line)))
        else:
            instruction = line.split('//', 1)[0].strip()
            if instruction:
                blocks[block][0].append(instruction)
    assert blocks
    assert labels
    return blocks, labels


def prmt(a: int, selectors: int, b: int) -> int:
    words = a | b << 32
    result = 0
    for lane in range(4):
        selector = (selectors >> (4 * lane)) & 15
        byte = (words >> (8 * (selector & 7))) & 255
        if selector & 8:
            byte = 255 if byte & 128 else 0
        result |= byte << (8 * lane)
    return result


def narrow_half(word: int) -> int:
    try:
        return int.from_bytes(struct.pack('<e', f32(word)), 'little')
    except OverflowError:
        return 0x7C00 | (word >> 16 & 0x8000)


def rounded_integer(word: int, op: str) -> int:
    word = ftz(word)
    number = f32(word)
    if not math.isfinite(number):
        return word
    result = bits(float(math.floor(number) if op.endswith('.rm') else round(number)))
    return result or word & 0x80000000


def integer_from_float(word: int, op: str) -> int:
    result = math.trunc(f32(ftz(word)))
    if '.u32.' in op:
        return max(0, min(MASK, result))
    return max(-(1 << 31), min((1 << 31) - 1, result))


def float_from_integer(word: int, op: str) -> int:
    if '.i32.' in op and word >> 31:
        word -= 1 << 32
    return bits(float(word))


def compare(op: str, a: int, b: int) -> bool:
    if op.endswith('.i32'):
        a, b = (v - (1 << 32) if v >> 31 else v for v in (a, b))
    return {'lt': a < b, 'gt': a > b, 'le': a <= b, 'ge': a >= b, 'eq': a == b, 'ne': a != b}[op.split('.')[1]]


class Machine:
    def __init__(self, code: Program, dispatch: Dispatch, extras: Extras) -> None:
        self.blocks, self.labels = code
        self.dispatch = dispatch
        self.texture_words = extras.texture_words
        self.registers: dict[str, int] = {}
        self.local = bytearray(4096)
        self.output = bytearray(dispatch.before)
        self.constants = {
            (0, 0): 0,
            (0, 0x720): dispatch.src_offset,
            (0, 0x724): dispatch.dst_offset,
            (1, 0): SOURCE & MASK,
            (1, 4): SOURCE >> 32,
            (1, 8): dispatch.src_length,
            (1, 16): TARGET & MASK,
            (1, 20): TARGET >> 32,
            (1, 24): dispatch.dst_length,
        }
        self.constants.update(extras.constant_words or {})
        self.sysvals = {'sr[0x25]': dispatch.index // 32, 'sr[0x21]': dispatch.index % 32}
        self.sysvals.update(extras.system_values or {})

    def value(self, arg: str) -> int:
        if arg in LITERALS:
            return LITERALS[arg]
        if arg.endswith('.yy'):
            return self.value(arg[:-3]) >> 16
        if arg.startswith('!'):
            return int(not self.value(arg[1:]))
        if arg.startswith('-'):
            return -self.value(arg[1:]) & MASK
        return self.operand(arg)

    def operand(self, arg: str) -> int:
        if arg.startswith('0x'):
            return int(arg, 16)
        if match := CONSTANT_REFERENCE.fullmatch(arg):
            return self.constants[(int(match[1], 16), int(match[2], 16))]
        if match := REGISTER_RANGE.fullmatch(arg):
            return sum(self.value(f'r{i}') << (32 * (i - int(match[1]))) for i in range(int(match[1]), int(match[2])))
        return self.registers[arg]

    def address(self, arg: str) -> int:
        return sum(self.value(a) for a in arg.strip('[],').split('+'))

    def region(self, at: int, writing: bool) -> tuple[bytearray | bytes, int, int]:
        dispatch = self.dispatch
        if at < len(self.local):
            return self.local, at, len(self.local)
        if SOURCE <= at < SOURCE + len(dispatch.source):
            assert not writing, 'source write'
            return dispatch.source, at - SOURCE, dispatch.src_length
        if TARGET <= at < TARGET + len(self.output):
            return self.output, at - TARGET, dispatch.dst_length
        raise AssertionError(f'unmapped access {at:x}')

    def read(self, at: int, size: int) -> int:
        data, offset, limit = self.region(at, writing=False)
        assert offset >= 0, ('out-of-range access', offset, size, limit)
        assert offset + size <= limit, ('out-of-range access', offset, size, limit)
        return int.from_bytes(data[offset : offset + size], 'little')

    def write(self, at: int, size: int, word: int) -> None:
        data, offset, limit = self.region(at, writing=True)
        assert offset >= 0, ('out-of-range access', offset, size, limit)
        assert offset + size <= limit, ('out-of-range access', offset, size, limit)
        data[offset : offset + size] = word.to_bytes(size, 'little')

    def run(self) -> bytearray:
        block = 0
        for _ in range(4096):
            instructions, successors = self.blocks[block]
            next_block = successors[0] if successors else None
            for line in instructions:
                step = self.step(line)
                if step == EXIT:
                    return self.output
                if step is not None:
                    next_block = step
                    break
            assert next_block is not None, 'fell off program'
            block = next_block
        raise AssertionError('instruction budget exceeded')

    def step(self, line: str) -> int | None:
        if line.startswith('@'):
            predicate, line = line.split(None, 1)
            if not self.value(predicate[1:]):
                return None
        outputs, expression = line.split('=', 1) if '=' in line else ('', line)
        outputs = outputs.split()
        op, *args = expression.strip().split()
        if op in NO_EFFECT:
            return None
        if op == 'exit':
            return EXIT
        if op in BRANCHES:
            if op == 'bra' and not self.value(args[0]):
                return None
            return self.labels[args[-1]]
        if op.startswith('st.'):
            self.write(self.address(args[0]), int(op.rsplit('.b', 1)[1]) // 8, self.value(args[1]))
        elif op == 'tld.2d.lz.rgba':
            self.texture_load(outputs[0], args)
        else:
            self.assign(outputs, op, self.compute(op, args, line))
        return None

    def assign(self, outputs: list[str], op: str, result: int) -> None:
        assert len(outputs) in (1, 2), outputs
        self.registers[outputs[0]] = int(result) & MASK
        if len(outputs) == 2:
            assert outputs[1] == 'c0'
            assert op.startswith('iadd2')
            self.registers['c0'] = int(result) >> 32

    def texture_load(self, target: str, args: list[str]) -> None:
        assert args[0] == 'bindless'
        assert self.texture_words is not None
        match = REGISTER_RANGE.fullmatch(target)
        assert match
        assert int(match[2]) - int(match[1]) == 4
        xy = self.value(args[1])
        texel = self.texture_words.get((self.value(args[2]), xy & MASK, xy >> 32), (0, 0, 0, 0))
        for reg, word in zip(range(int(match[1]), int(match[2])), texel, strict=True):
            self.registers[f'r{reg}'] = word

    def compute(self, op: str, args: list[str], line: str) -> int:
        v = self.value
        if op == 'ldc.b32':
            match = re.fullmatch(r'c\[(0x[0-9a-f]+)\]\[(r\d+)\]', args[0])
            assert match
            return self.constants[(int(match[1], 16), v(match[2]))]
        if op.startswith('ld.'):
            assert op.endswith('.b32')
            assert v(args[1])
            return self.read(self.address(args[0]), 4)
        if op.startswith('isetp.'):
            return compare(op, v(args[0]), v(args[1]))
        if op.startswith('iadd2'):
            return sum(v(a) for a in args)
        if op.startswith('f2f.ftz.int.f32.f32.'):
            return rounded_integer(v(args[0]), op)
        if op in self.simple:
            return self.simple[op](self, args)
        raise AssertionError(f'unsupported instruction {line}')

    def minmax(self, args: list[str]) -> int:
        a, b = (f32(ftz(self.value(arg))) for arg in args[:2])
        return bits((min if self.value(args[2]) else max)(a, b))

    def reciprocal(self, args: list[str]) -> int:
        number = f32(self.value(args[0]))
        assert math.isfinite(number)
        assert number > 0
        return bits(1.0 / number)

    simple: ClassVar[dict[str, Callable[['Machine', list[str]], int]]] = {
        's2r': lambda m, args: m.sysvals[args[0]],
        'i2f.f32.u32.re': lambda m, args: float_from_integer(m.value(args[0]), '.u32.'),
        'i2f.f32.i32.re': lambda m, args: float_from_integer(m.value(args[0]), '.i32.'),
        'f2i.u32.f32.rz.ftz': lambda m, args: integer_from_float(m.value(args[0]), '.u32.'),
        'f2i.i32.f32.rz.ftz': lambda m, args: integer_from_float(m.value(args[0]), '.i32.'),
        'mov': lambda m, args: m.value(args[0]),
        'prmt': lambda m, args: prmt(m.value(args[0]), int(args[1].strip('[]'), 16), m.value(args[2])),
        'shl.w': lambda m, args: m.value(args[0]) << (m.value(args[1]) & 31),
        'shr.w.u32': lambda m, args: m.value(args[0]) >> (m.value(args[1]) & 31),
        'imul.u32.u32': lambda m, args: m.value(args[0]) * m.value(args[1]),
        'imul.hi.u32.u32': lambda m, args: (m.value(args[0]) * m.value(args[1])) >> 32,
        'lop2.and': lambda m, args: m.value(args[0]) & m.value(args[1]),
        'lop2.xor': lambda m, args: m.value(args[0]) ^ m.value(args[1]),
        'lop2.or': lambda m, args: m.value(args[0]) | m.value(args[1]),
        'sel': lambda m, args: m.value(args[1] if m.value(args[0]) else args[2]),
        'fmnmx.ftz': lambda m, args: m.minmax(args),
        'mufu.rcp.f32': lambda m, args: m.reciprocal(args),
        'fmul.ftz': lambda m, args: ftz(bits(f32(ftz(m.value(args[0]))) * f32(ftz(m.value(args[1]))))),
        'f2f.f32.f16.re': lambda m, args: half(m.value(args[0])),
        'f2f.f16.f32.re': lambda m, args: narrow_half(m.value(args[0])),
        'fsetp.ge.ftz': lambda m, args: f32(ftz(m.value(args[0]))) >= f32(ftz(m.value(args[1]))),
    }


def execute(code: Program, dispatch: Dispatch, extras: Extras | None = None) -> bytearray:
    return Machine(code, dispatch, extras or Extras()).run()


def oracle_vector(x: list[int], index: int) -> list[int]:
    return [
        ((x[3] << 1) ^ x[1]) & MASK,
        ((x[2] << 2) ^ x[0]) & MASK,
        ((x[1] << 3) ^ index) & MASK,
        ((x[0] << 4) ^ ((x[2] + 5) & MASK)) & MASK,
    ]


def oracle_private(x: list[int], index: int) -> list[int]:
    for j in range(index & 3):
        x[(j + 1) & 3] ^= x[j]
    return [x[index & 3]]


def oracle_extended(x: list[int], _index: int) -> list[int]:
    a, b, c = (f32(ftz(word)) for word in x[:3])
    a = float(math.floor(a)) if math.isfinite(a) else a
    b = float(round(b)) if math.isfinite(b) else b
    return [
        bits(math.copysign(a, f32(x[0]))) if a == 0 else bits(a),
        bits(math.copysign(b, f32(x[1]))) if b == 0 else bits(b),
        bits(min(max(c, -0.25), 0.75)),
        x[3] & 0x7FFFFFFF,
    ]


def oracle_unpack_half(x: list[int], _index: int) -> list[int]:
    return [half(word >> shift) for word in x for shift in (0, 16)]


def oracle_convert(x: list[int], _index: int) -> list[int]:
    a = f32(bits(float(x[0])))
    b = f32(bits(float(x[1] - (1 << 32) if x[1] >> 31 else x[1])))
    try:
        narrowed = struct.unpack('<e', struct.pack('<e', a))[0]
    except OverflowError:
        narrowed = math.inf
    return [
        bits(a),
        bits(b),
        MASK if math.isinf(narrowed) else min(MASK, int(narrowed)),
        min((1 << 31) - 1, max(-(1 << 31), int(b))) & MASK,
    ]


ORACLES = {
    'vector': oracle_vector,
    'private': oracle_private,
    'extended': oracle_extended,
    'unpack_half': oracle_unpack_half,
    'convert': oracle_convert,
}


def oracle(name: str, dispatch: Dispatch) -> bytearray:
    if name not in ORACLES:
        raise AssertionError(name)
    index, source = dispatch.index, dispatch.source
    x = []
    stride = 8 if name == 'unpack_half' else 16
    for lane in range(stride // 4):
        at = dispatch.src_offset + index * stride + lane * 4
        x.append(int.from_bytes(source[at : at + 4], 'little') if at + 4 <= dispatch.src_length else 0)
    y = ORACLES[name](x, index)
    out = bytearray(dispatch.before)
    for lane, word in enumerate(y):
        at = dispatch.dst_offset + index * len(y) * 4 + lane * 4
        if at + 4 <= dispatch.dst_length:
            out[at : at + 4] = struct.pack('<I', word)
    return out


def check_encoded_differences(root: Path, name: str) -> None:
    a = (root / f'semantic_{name}.native.sass').read_bytes()
    b = (root / f'semantic_{name}.portable.sass').read_bytes()
    assert len(a) == len(b)
    assert len(a) % 32 == 0
    compares = masks = 0
    for at in range(0, len(a), 8):
        x, y = (int.from_bytes(data[at : at + 8], 'little') for data in (a, b))
        if x == y:
            continue
        assert at % 32, 'scheduling controls differ'
        if (x ^ y) == (7 << 49):
            assert ((x >> 48) & ~15) == 0x5B60
            assert {(x >> 49) & 7, (y >> 49) & 7} == {2, 5}
            compares += 1
        else:
            assert x ^ y == 0xC00000
            assert x >> 48 == y >> 48 == 0x3947
            immediates = {((v >> 20) & 0x7FFFF) | (-1 << 19) for v in (x, y)}
            assert immediates == {-4, -16}
            masks += 1
    assert compares == (2 if name in ('vector', 'extended', 'convert') else 1)
    assert masks == 2 * compares


def extended_source(rng: random.Random) -> bytes:
    values = [
        0.0,
        -0.0,
        0.5,
        -0.5,
        1.5,
        -1.5,
        2.5,
        -2.5,
        0.75,
        -0.25,
        float('inf'),
        -float('inf'),
        f32(1),
        f32(0x80000001),
    ]
    values += [rng.uniform(-1e5, 1e5) for _ in range(242)]
    return b''.join(struct.pack('<f', value) for value in values)


def unpack_half_source(rng: random.Random) -> bytes:
    values = [0, 0x8000, 1, 0x8001, 0x3C00, 0xBC00, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00]
    values += [rng.randrange(0x7C00) | rng.randrange(2) << 15 for _ in range(502)]
    return b''.join(struct.pack('<H', value) for value in values)


def convert_source(rng: random.Random) -> bytes:
    values = [0, 1, 65504, 65520, 0x7FFFFFFF, 0x80000000, MASK, 0xFFFF0001]
    values += [rng.randrange(1 << 32) for _ in range(248)]
    return b''.join(struct.pack('<I', value) for value in values)


SOURCES = {'extended': extended_source, 'unpack_half': unpack_half_source, 'convert': convert_source}
LENGTHS = (
    (0, 0),
    (3, 256),
    (4, 256),
    (15, 256),
    (16, 256),
    (61, 256),
    (127, 255),
    (256, 61),
    (256, 127),
    (1024, 1024),
)


def broken_predicate(native: Program) -> Program:
    broken = ({b: (list(ops), list(edges)) for b, (ops, edges) in native[0].items()}, native[1])
    entry, at = next((ops, i) for ops, _ in broken[0].values() for i, op in enumerate(ops) if 'isetp.ne.i32' in op)
    entry[at] = entry[at].replace('isetp.ne.i32', 'isetp.eq.i32')
    return broken


def check_function(root: Path, name: str, source: bytes, before: bytes) -> int:
    check_encoded_differences(root, name)
    log = (root / f'{name}-nir.txt').read_text()
    native, portable = (program(log, name, r) for r in ('native', 'portable'))
    control = Dispatch(1, source, before, 4, 4, 1024, 1024)
    assert execute(broken_predicate(native), control) != oracle(name, control), (
        'negative predicate control was ineffective'
    )
    checks = 0
    for src_offset, dst_offset, (src_length, dst_length), index in itertools.product(
        (0, 4, 8, 12, 16), (0, 4, 8, 12, 16), LENGTHS, range(16)
    ):
        dispatch = Dispatch(index, source, before, src_offset, dst_offset, src_length, dst_length)
        expected = oracle(name, dispatch)
        for route, code in (('native', native), ('portable', portable)):
            where = (name, route, src_offset, dst_offset, src_length, dst_length, index)
            assert execute(code, dispatch) == expected, where
            checks += 1
    return checks


def check_texture(root: Path, rng: random.Random, before: bytes) -> int:
    check_encoded_differences(root, 'texture')
    texture = {(9, x, y): tuple(rng.getrandbits(32) & 0x7EFFFFFF for _ in range(4)) for y in range(2) for x in range(8)}
    log = (root / 'texture-nir.txt').read_text()
    checks = 0
    for route in ('native', 'portable'):
        code = program(log, 'texture', route)
        for offset, length, index in itertools.product((0, 4, 8, 12, 16), (0, 3, 4, 15, 16, 61, 127, 1024), range(16)):
            cb = {(0, 0x720): offset, (1, 0): TARGET & MASK, (1, 4): TARGET >> 32, (1, 8): length, (1, 16): 9}
            want = bytearray(before)
            texel = texture.get((9, index, 0), (0, 0, 0, 0))
            for lane, word in enumerate(texel):
                at = offset + 16 * index + 4 * lane
                if at + 4 <= length:
                    struct.pack_into('<I', want, at, word)
            got = execute(
                code,
                Dispatch(index, b'', before, 0, offset, 0, length),
                Extras(constant_words=cb, texture_words=texture),
            )
            assert got == want, ('texture', route, offset, length, index)
            checks += 1
        cb = {(0, 0x720): 0, (1, 0): TARGET & MASK, (1, 4): TARGET >> 32, (1, 8): 1024, (1, 16): 0}
        whole = Dispatch(0, b'', before, 0, 0, 0, 1024)
        neutral = execute(code, whole, Extras(constant_words=cb, texture_words=texture))
        cb[(1, 16)] = 9
        bound = execute(code, whole, Extras(constant_words=cb, texture_words=texture))
        assert neutral != bound, 'texture descriptor negative control was ineffective'
    return checks


def run(root: Path) -> None:
    rng = random.Random(0x53574954)
    initial = rng.randbytes(1024)
    before = rng.randbytes(1024)
    checks = 0
    for name in ORACLES:
        source = SOURCES[name](rng) if name in SOURCES else initial
        checks += check_function(root, name, source, before)
    checks += check_texture(root, rng, before)
    print(f'PASS {checks} actual register-IR executions; changed SASS fields and seven negative controls verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    run(parser.parse_args().output)
