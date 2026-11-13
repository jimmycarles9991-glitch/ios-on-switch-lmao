#!/usr/bin/env python3
import argparse
import math
import random
import re
import struct
from pathlib import Path

MASK = (1 << 32) - 1
SOURCE, TARGET = 0x100000000, 0x200000000


def f32(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def bits(number):
    return struct.unpack('<I', struct.pack('<f', number))[0]


def ftz(word):
    return word if word & 0x7F800000 else word & 0x80000000


def half(word):
    return bits(struct.unpack('<e', struct.pack('<H', word & 0xFFFF))[0])


def program(log, name, route):
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
    assert blocks and labels
    return blocks, labels


def execute(
    code,
    index,
    source,
    before,
    src_offset,
    dst_offset,
    src_length,
    dst_length,
    *,
    constant_words=None,
    system_values=None,
    texture_words=None,
):
    blocks, labels = code
    registers, local, output = {}, bytearray(4096), bytearray(before)
    constants = {
        (0, 0): 0,
        (0, 0x720): src_offset,
        (0, 0x724): dst_offset,
        (1, 0): SOURCE & MASK,
        (1, 4): SOURCE >> 32,
        (1, 8): src_length,
        (1, 16): TARGET & MASK,
        (1, 20): TARGET >> 32,
        (1, 24): dst_length,
    }
    constants.update(constant_words or {})
    sysvals = {'sr[0x25]': index // 32, 'sr[0x21]': index % 32}
    sysvals.update(system_values or {})

    def value(arg):
        if arg == 'rZ':
            return 0
        if arg == 'pT':
            return 1
        if arg == 'pF':
            return 0
        if arg.endswith('.yy'):
            return value(arg[:-3]) >> 16
        if arg.startswith('!'):
            return int(not value(arg[1:]))
        if arg.startswith('-'):
            return -value(arg[1:]) & MASK
        if arg.startswith('0x'):
            return int(arg, 16)
        if match := re.fullmatch(r'c\[(0x[0-9a-f]+)\]\[(0x[0-9a-f]+)\]', arg):
            return constants[(int(match[1], 16), int(match[2], 16))]
        if match := re.fullmatch(r'r(\d+)\.\.(\d+)', arg):
            return sum(value(f'r{i}') << (32 * (i - int(match[1]))) for i in range(int(match[1]), int(match[2])))
        return registers[arg]

    def address(arg):
        return sum(value(a) for a in arg.strip('[],').split('+'))

    def memory(at, size, write=None):
        if at < len(local):
            data, offset, limit = local, at, len(local)
        elif SOURCE <= at < SOURCE + len(source):
            data, offset, limit = source, at - SOURCE, src_length
            assert write is None, 'source write'
        elif TARGET <= at < TARGET + len(output):
            data, offset, limit = output, at - TARGET, dst_length
        else:
            raise AssertionError(f'unmapped access {at:x}')
        assert 0 <= offset and offset + size <= limit, ('out-of-range access', offset, size, limit)
        if write is None:
            return int.from_bytes(data[offset : offset + size], 'little')
        data[offset : offset + size] = write.to_bytes(size, 'little')

    block = 0
    for _ in range(4096):
        instructions, successors = blocks[block]
        next_block = successors[0] if successors else None
        for line in instructions:
            if line.startswith('@'):
                predicate, line = line.split(None, 1)
                if not value(predicate[1:]):
                    continue
            outputs, expression = line.split('=', 1) if '=' in line else ('', line)
            outputs = outputs.split()
            op, *args = expression.strip().split()
            result = 0
            if op in ('nop', 'ssy', 'pbk', 'pcnt', 'bar.sync', 'membar.sc.cta'):
                continue
            if op == 'exit':
                return output
            if op in ('bra', 'sync', 'brk', 'cont'):
                if op == 'bra' and not value(args[0]):
                    continue
                next_block = labels[args[-1]]
                break
            if op == 's2r':
                result = sysvals[args[0]]
            elif op == 'tld.2d.lz.rgba':
                assert args[0] == 'bindless' and texture_words is not None
                match = re.fullmatch(r'r(\d+)\.\.(\d+)', outputs[0])
                assert match and int(match[2]) - int(match[1]) == 4
                xy = value(args[1])
                texel = texture_words.get((value(args[2]), xy & MASK, xy >> 32), (0, 0, 0, 0))
                for reg, word in zip(range(int(match[1]), int(match[2])), texel):
                    registers[f'r{reg}'] = word
                continue
            elif op == 'mov':
                result = value(args[0])
            elif op == 'ldc.b32':
                match = re.fullmatch(r'c\[(0x[0-9a-f]+)\]\[(r\d+)\]', args[0])
                assert match
                result = constants[(int(match[1], 16), value(match[2]))]
            elif op == 'prmt':
                a, b = value(args[0]), value(args[2])
                selectors = int(args[1].strip('[]'), 16)
                words = a | b << 32
                result = 0
                for lane in range(4):
                    selector = (selectors >> (4 * lane)) & 15
                    byte = (words >> (8 * (selector & 7))) & 255
                    if selector & 8:
                        byte = 255 if byte & 128 else 0
                    result |= byte << (8 * lane)
            elif op.startswith('iadd2'):
                result = sum(value(a) for a in args)
            elif op == 'shl.w':
                result = value(args[0]) << (value(args[1]) & 31)
            elif op == 'shr.w.u32':
                result = value(args[0]) >> (value(args[1]) & 31)
            elif op == 'imul.u32.u32':
                result = value(args[0]) * value(args[1])
            elif op == 'imul.hi.u32.u32':
                result = (value(args[0]) * value(args[1])) >> 32
            elif op == 'lop2.and':
                result = value(args[0]) & value(args[1])
            elif op == 'lop2.xor':
                result = value(args[0]) ^ value(args[1])
            elif op == 'lop2.or':
                result = value(args[0]) | value(args[1])
            elif op.startswith('isetp.'):
                a, b = map(value, args)
                if op.endswith('.i32'):
                    a, b = (v - (1 << 32) if v >> 31 else v for v in (a, b))
                cmp = op.split('.')[1]
                result = {'lt': a < b, 'gt': a > b, 'le': a <= b, 'ge': a >= b, 'eq': a == b, 'ne': a != b}[cmp]
            elif op == 'sel':
                result = value(args[1] if value(args[0]) else args[2])
            elif op == 'fmnmx.ftz':
                a, b = (f32(ftz(value(arg))) for arg in args[:2])
                result = bits((min if value(args[2]) else max)(a, b))
            elif op == 'mufu.rcp.f32':
                number = f32(value(args[0]))
                assert math.isfinite(number) and number > 0
                result = bits(1.0 / number)
            elif op == 'fmul.ftz':
                result = ftz(bits(f32(ftz(value(args[0]))) * f32(ftz(value(args[1])))))
            elif op.startswith('f2f.ftz.int.f32.f32.'):
                word = ftz(value(args[0]))
                number = f32(word)
                if math.isfinite(number):
                    result = bits(float(math.floor(number) if op.endswith('.rm') else round(number)))
                    if result == 0:
                        result = word & 0x80000000
                else:
                    result = word
            elif op == 'f2f.f32.f16.re':
                result = half(value(args[0]))
            elif op == 'f2f.f16.f32.re':
                number = f32(value(args[0]))
                try:
                    result = int.from_bytes(struct.pack('<e', number), 'little')
                except OverflowError:
                    result = 0x7C00 | (value(args[0]) >> 16 & 0x8000)
            elif op in ('i2f.f32.u32.re', 'i2f.f32.i32.re'):
                number = value(args[0])
                if '.i32.' in op and number >> 31:
                    number -= 1 << 32
                result = bits(float(number))
            elif op in ('f2i.u32.f32.rz.ftz', 'f2i.i32.f32.rz.ftz'):
                result = math.trunc(f32(ftz(value(args[0]))))
                result = max(0, min(MASK, result)) if '.u32.' in op else max(-(1 << 31), min((1 << 31) - 1, result))
            elif op == 'fsetp.ge.ftz':
                result = f32(ftz(value(args[0]))) >= f32(ftz(value(args[1])))
            elif op.startswith('ld.'):
                assert op.endswith('.b32') and value(args[1])
                result = memory(address(args[0]), 4)
            elif op.startswith('st.'):
                size = int(op.rsplit('.b', 1)[1]) // 8
                memory(address(args[0]), size, value(args[1]))
                continue
            else:
                raise AssertionError(f'unsupported instruction {line}')
            assert len(outputs) in (1, 2), line
            registers[outputs[0]] = int(result) & MASK
            if len(outputs) == 2:
                assert outputs[1] == 'c0' and op.startswith('iadd2')
                registers['c0'] = int(result) >> 32
        assert next_block is not None, 'fell off program'
        block = next_block
    raise AssertionError('instruction budget exceeded')


def oracle(name, index, source, before, src_offset, dst_offset, src_length, dst_length):
    x = []
    stride = 8 if name == 'unpack_half' else 16
    for lane in range(stride // 4):
        at = src_offset + index * stride + lane * 4
        x.append(int.from_bytes(source[at : at + 4], 'little') if at + 4 <= src_length else 0)
    if name == 'vector':
        y = [
            ((x[3] << 1) ^ x[1]) & MASK,
            ((x[2] << 2) ^ x[0]) & MASK,
            ((x[1] << 3) ^ index) & MASK,
            ((x[0] << 4) ^ ((x[2] + 5) & MASK)) & MASK,
        ]
    elif name == 'private':
        for j in range(index & 3):
            x[(j + 1) & 3] ^= x[j]
        y = [x[index & 3]]
    elif name == 'extended':
        a, b, c = (f32(ftz(word)) for word in x[:3])
        a = float(math.floor(a)) if math.isfinite(a) else a
        b = float(round(b)) if math.isfinite(b) else b
        y = [
            bits(math.copysign(a, f32(x[0]))) if a == 0 else bits(a),
            bits(math.copysign(b, f32(x[1]))) if b == 0 else bits(b),
            bits(min(max(c, -0.25), 0.75)),
            x[3] & 0x7FFFFFFF,
        ]
    elif name == 'unpack_half':
        y = [half(word >> shift) for word in x for shift in (0, 16)]
    elif name == 'convert':
        a = f32(bits(float(x[0])))
        b = f32(bits(float(x[1] - (1 << 32) if x[1] >> 31 else x[1])))
        try:
            narrowed = struct.unpack('<e', struct.pack('<e', a))[0]
        except OverflowError:
            narrowed = math.inf
        y = [
            bits(a),
            bits(b),
            MASK if math.isinf(narrowed) else min(MASK, int(narrowed)),
            min((1 << 31) - 1, max(-(1 << 31), int(b))) & MASK,
        ]
    else:
        raise AssertionError(name)
    out = bytearray(before)
    for lane, word in enumerate(y):
        at = dst_offset + index * len(y) * 4 + lane * 4
        if at + 4 <= dst_length:
            out[at : at + 4] = struct.pack('<I', word)
    return out


def check_encoded_differences(root, name):
    a = (root / f'semantic_{name}.native.sass').read_bytes()
    b = (root / f'semantic_{name}.portable.sass').read_bytes()
    assert len(a) == len(b) and len(a) % 32 == 0
    compares = masks = 0
    for at in range(0, len(a), 8):
        x, y = (int.from_bytes(data[at : at + 8], 'little') for data in (a, b))
        if x == y:
            continue
        assert at % 32, 'scheduling controls differ'
        if (x ^ y) == (7 << 49):
            assert ((x >> 48) & ~15) == 0x5B60 and {(x >> 49) & 7, (y >> 49) & 7} == {2, 5}
            compares += 1
        else:
            assert x ^ y == 0xC00000 and x >> 48 == y >> 48 == 0x3947
            immediates = {((v >> 20) & 0x7FFFF) | (-1 << 19) for v in (x, y)}
            assert immediates == {-4, -16}
            masks += 1
    assert compares == (2 if name in ('vector', 'extended', 'convert') else 1) and masks == 2 * compares


def run(root):
    rng = random.Random(0x53574954)
    source = rng.randbytes(1024)
    before = rng.randbytes(1024)
    checks = 0
    for name in ('vector', 'private', 'extended', 'unpack_half', 'convert'):
        if name == 'extended':
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
            source = b''.join(struct.pack('<f', value) for value in values)
        elif name == 'unpack_half':
            values = [0, 0x8000, 1, 0x8001, 0x3C00, 0xBC00, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00]
            values += [rng.randrange(0x7C00) | rng.randrange(2) << 15 for _ in range(502)]
            source = b''.join(struct.pack('<H', value) for value in values)
        elif name == 'convert':
            values = [0, 1, 65504, 65520, 0x7FFFFFFF, 0x80000000, MASK, 0xFFFF0001]
            values += [rng.randrange(1 << 32) for _ in range(248)]
            source = b''.join(struct.pack('<I', value) for value in values)
        check_encoded_differences(root, name)
        log = (root / f'{name}-nir.txt').read_text()
        native, portable = (program(log, name, r) for r in ('native', 'portable'))
        broken = ({b: (list(ops), list(edges)) for b, (ops, edges) in native[0].items()}, native[1])
        entry = broken[0][0][0]
        entry, at = next((ops, i) for ops, _ in broken[0].values() for i, op in enumerate(ops) if 'isetp.ne.i32' in op)
        entry[at] = entry[at].replace('isetp.ne.i32', 'isetp.eq.i32')
        control = (1, source, before, 4, 4, 1024, 1024)
        assert execute(broken, *control) != oracle(name, *control), 'negative predicate control was ineffective'
        for src_offset in (0, 4, 8, 12, 16):
            for dst_offset in (0, 4, 8, 12, 16):
                for src_length, dst_length in (
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
                ):
                    for index in range(16):
                        args = (index, source, before, src_offset, dst_offset, src_length, dst_length)
                        expected = oracle(name, *args)
                        assert execute(native, *args) == expected, (
                            name,
                            'native',
                            src_offset,
                            dst_offset,
                            src_length,
                            dst_length,
                            index,
                        )
                        assert execute(portable, *args) == expected, (
                            name,
                            'portable',
                            src_offset,
                            dst_offset,
                            src_length,
                            dst_length,
                            index,
                        )
                        checks += 2
    check_encoded_differences(root, 'texture')
    texture = {(9, x, y): tuple(rng.getrandbits(32) & 0x7EFFFFFF for _ in range(4)) for y in range(2) for x in range(8)}
    log = (root / 'texture-nir.txt').read_text()
    for route in ('native', 'portable'):
        code = program(log, 'texture', route)
        for offset in (0, 4, 8, 12, 16):
            for length in (0, 3, 4, 15, 16, 61, 127, 1024):
                cb = {(0, 0x720): offset, (1, 0): TARGET & MASK, (1, 4): TARGET >> 32, (1, 8): length, (1, 16): 9}
                for index in range(16):
                    want = bytearray(before)
                    texel = texture.get((9, index, 0), (0, 0, 0, 0))
                    for lane, word in enumerate(texel):
                        at = offset + 16 * index + 4 * lane
                        if at + 4 <= length:
                            struct.pack_into('<I', want, at, word)
                    got = execute(
                        code, index, b'', before, 0, offset, 0, length, constant_words=cb, texture_words=texture
                    )
                    assert got == want, ('texture', route, offset, length, index)
                    checks += 1
        cb = {(0, 0x720): 0, (1, 0): TARGET & MASK, (1, 4): TARGET >> 32, (1, 8): 1024, (1, 16): 0}
        neutral = execute(code, 0, b'', before, 0, 0, 0, 1024, constant_words=cb, texture_words=texture)
        cb[(1, 16)] = 9
        bound = execute(code, 0, b'', before, 0, 0, 0, 1024, constant_words=cb, texture_words=texture)
        assert neutral != bound, 'texture descriptor negative control was ineffective'
    print(f'PASS {checks} actual register-IR executions; changed SASS fields and seven negative controls verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    run(parser.parse_args().output)
