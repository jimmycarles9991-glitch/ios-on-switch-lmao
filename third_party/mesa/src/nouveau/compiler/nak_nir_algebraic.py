# Copyright (C) 2023 Red Hat, Inc.
# Copyright (C) 2021 Collabora, Ltd.
# Copyright (C) 2016 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import argparse
import sys

a = 'a'
b = 'b'
c = 'c'
s = 's'

# common conditions to improve readability
volta = 'nak->sm >= 70 && nak->sm < 73'
fp16_round_is_rtz = 'nir_is_rounding_mode_rtz(info->float_controls_execution_mode, 16)'

algebraic_lowering = [
    # Volta doesn't have `IMNMX`
    (('imin', 'a', 'b'), ('bcsel', ('ilt', a, b), a, b), volta),
    (('imax', 'a', 'b'), ('bcsel', ('ilt', a, b), b, a), volta),
    (('umin', 'a', 'b'), ('bcsel', ('ult', a, b), a, b), volta),
    (('umax', 'a', 'b'), ('bcsel', ('ult', a, b), b, a), volta),
    (('iadd', 'a@64', ('ineg', 'b@64')), ('isub', a, b)),

    (('iadd', ('iadd(is_only_used_by_iadd)', 'a(is_not_const)', '#b'), 'c(is_not_const)'),
        ('iadd3', a, b, c), 'options->has_iadd3'),
    (('iadd', ('iadd(is_only_used_by_iadd)', 'a(is_not_const)', 'b(is_not_const)'), '#c'),
        ('iadd3', a, b, c), 'options->has_iadd3'),

    (('iadd', ('ineg(is_only_used_by_iadd)', ('iadd(is_used_once)', 'a(is_not_const)', '#b')), 'c(is_not_const)'),
        ('iadd3', ('ineg', a), ('ineg', b), c), 'options->has_iadd3'),
    (('iadd', ('ineg(is_only_used_by_iadd)', ('iadd(is_used_once)', 'a(is_not_const)', 'b(is_not_const)')), '#c'),
        ('iadd3', ('ineg', a), c, ('ineg', b)), 'options->has_iadd3'),

    (('iadd', 'a@32', ('ishl', 'b@32', '#s@32')),
        ('lea_nv', a, b, s), 'nak->sm >= 50'),
    (('iadd', 'a@64', ('ishl', 'b@64', '#s@32')),
        ('lea_nv', a, b, s), 'nak->sm >= 70'),
]

for f2f16 in ['f2f16', 'f2f16_rtz', 'f2f16_rtne']:
    algebraic_lowering += [
        (('vec2', (f2f16 + '(is_used_once)', 'a@32'), (f2f16 + '(is_used_once)', 'b@32')), (f2f16, ('vec2', a, b)), 'nak->sm >= 86')
    ]

# ios-on-switch-lmao: metal2vulkan rounds a float to half precision, on a
# device without Float16, as fquantize2f16 (which flushes a result below half's
# smallest normal) and, where that gave zero, the float rounded to a multiple of
# half's smallest denormal, 2^-24: seven instructions for what is the round trip
# through f16 with round-to-nearest-even keeping denormals (the same for every
# f32 input, checked exhaustively). The two conversions keep denormals under
# the shader's fp16 mode unless it flushes them.
algebraic_lowering += [
    (('bcsel', ('feq', ('fquantize2f16', 'a@32'), 0.0),
               ('fmul', ('fround_even', ('fmul', a, 16777216.0)), 5.9604644775390625e-08),
               ('fquantize2f16', a)),
        ('f2f32', ('f2f16_rtne', a)),
        '!nir_is_denorm_flush_to_zero(info->float_controls_execution_mode, 16)'),
]

# ios-on-switch-lmao: two halves added or multiplied in f32 and rounded back to
# half are their half sum or product (24 >= 2 x 11 + 2 bits make the double
# rounding innocuous; checked exhaustively against native f16 arithmetic), so
# metal2vulkan's f32 emulation of half arithmetic runs as SM53's HADD2/HMUL2.
# Each stays exact, as metal2vulkan's sum is: fused into an HFMA2 a product
# would skip the rounding metal2vulkan gave it.
half_denorms_kept = '!nir_is_denorm_flush_to_zero(info->float_controls_execution_mode, 16)'
half_exact = 'nak->sm >= 53 && ' + half_denorms_kept
for op in ['fadd', 'fmul']:
    for na in [False, True]:
        for nb in [False, True]:
            x = ('fneg', ('f2f32', 'a@16')) if na else ('f2f32', 'a@16')
            y = ('fneg', ('f2f32', 'b@16')) if nb else ('f2f32', 'b@16')
            algebraic_lowering += [
                (('f2f16_rtne', (op, x, y)),
                    ('!' + op, ('fneg', a) if na else a, ('fneg', b) if nb else b), half_exact),
            ]

# ios-on-switch-lmao: a comparison of two halves widened to f32 is their
# comparison as halves (the widening is exact, NaNs included), one HSETP2.
for cmp in ['flt', 'fge', 'feq', 'fneu', 'fltu', 'fgeu', 'fequ', 'fneo']:
    algebraic_lowering += [
        ((cmp, ('f2f32', 'a@16'), ('f2f32', 'b@16')), (cmp, a, b), 'nak->sm >= 53 && nak->sm < 70'),
    ]

# ios-on-switch-lmao: metal2vulkan rounds a half sum (or difference) as
# fquantize2f16 and, where that gave zero, the f32 sum itself: a sum of two
# halves below half's smallest normal is a multiple of 2^-24, exact in f32 and
# as a half denormal. For every pair of halves that is their half sum (checked
# exhaustively), so it runs as HADD2 too.
for na in [False, True]:
    for nb in [False, True]:
        x = ('fneg', ('f2f32', 'a@16')) if na else ('f2f32', 'a@16')
        y = ('fneg', ('f2f32', 'b@16')) if nb else ('f2f32', 'b@16')
        s = ('fadd', x, y)
        algebraic_lowering += [
            (('bcsel', ('feq', ('fquantize2f16', s), 0.0), s, ('fquantize2f16', s)),
                ('f2f32', ('!fadd', ('fneg', a) if na else a, ('fneg', b) if nb else b)), half_exact),
        ]

# ios-on-switch-lmao: on a device without Float16 metal2vulkan moves a half
# between its bits (a load, a store, a reinterpretation) and the f32 holding it
# with integer sequences of eleven and fifteen instructions a lane. Each is one
# F2F, exactly (checked exhaustively): the widening for every bit pattern (the
# bits above the low 16 ignored, as u2u16 ignores them), the narrowing for
# every f32 that holds a half, which is every f32 metal2vulkan narrows this
# way. A NaN stays a NaN, its payload the device's.
def m2v_half_widened(h):
    em = ('iand', h, 0x7fff)
    normal = ('iadd', ('ishl', em, 13), ('bcsel', ('uge', em, 0x7c00), 0x70000000, 0x38000000))
    denormal = ('fmul', ('u2f32', em), 5.9604644775390625e-08)
    return ('ior', ('ishl', ('iand', h, 0x8000), 16), ('bcsel', ('ult', em, 0x400), denormal, normal))

def m2v_half_bits(x):
    mag = ('iand', x, 0x7fffffff)
    special = ('uge', mag, 0x7f800000)
    normal = ('umin', ('iadd', ('ushr', mag, 13), ('bcsel', special, 0xfffc8000, 0xfffe4000)),
                      ('bcsel', special, 0x23c00, 0x7c00))
    normal = ('bcsel', ('ult', ('iadd', mag, 0x807fffff), 0x1fff), 0x7e00, normal)
    denormal = ('ult', mag, 0x38800000)
    scaled = ('f2u32', ('fmul', ('bcsel', denormal, mag, 0), 16777216.0))
    return ('ior(many-comm-expr)', ('iand', ('extract_u16', x, 1), 0x8000), ('bcsel', denormal, scaled, normal))

algebraic_lowering += [
    (m2v_half_widened('a@32'), ('f2f32', ('u2u16', a)), half_denorms_kept),
    (m2v_half_bits('a@32'), ('u2u32', ('f2f16_rtne', a)), half_denorms_kept),
    (('f2f16_rtne', ('f2f32', 'a@16')), a, half_denorms_kept),
    (('ior', ('u2u32', 'a@16'), ('ishl', ('u2u32', 'b@16'), 16)), ('pack_32_2x16_split', a, b)),
]

# ios-on-switch-lmao: a half metal2vulkan widens to negate, clamp or select in
# f32 and narrows again is that operation on the half itself, bit for bit.
for op in ['fneg', 'fsat']:
    algebraic_lowering += [
        (('f2f16_rtne', (op, ('f2f32', 'a@16'))), (op, a), half_denorms_kept),
    ]
algebraic_lowering += [
    (('f2f16_rtne', ('bcsel', c, ('f2f32', 'a@16'), ('f2f32', 'b@16'))), ('bcsel', c, a, b)),
    (('f2f16_rtne', ('bcsel', c, ('f2f32', 'a@16'), '#b@32(is_representable_as_f16)')),
        ('bcsel', c, a, ('f2f16_rtne', b))),
    (('f2f16_rtne', ('bcsel', c, '#b@32(is_representable_as_f16)', ('f2f32', 'a@16'))),
        ('bcsel', c, ('f2f16_rtne', b), a)),
]

# ios-on-switch-lmao: metal2vulkan holds a sampled texel rounded to half in an
# f32 (a NaN kept as sampled, anything else the round trip through f16), and
# narrows it again for the half arithmetic that reads it. Narrowed, both arms
# are the texel's own f16: the round trip of a half is exact, and a NaN arm
# narrows the texel itself. Each channel's guard (mask, compare, select, F2F
# back) goes, leaving one F2F.
algebraic_lowering += [
    (('f2f16_rtne', ('bcsel', c, 'a@32', ('f2f32', ('f2f16_rtne', a)))), ('f2f16_rtne', a)),
    (('f2f16_rtne', ('bcsel', c, ('f2f32', ('f2f16_rtne', 'a@32')), a)), ('f2f16_rtne', a)),
]

# ios-on-switch-lmao: metal2vulkan flushes a float's denormal to a signed zero
# with integer ops wherever Metal does. An FTZ instruction (fp32 unless the
# shader keeps denormals) flushes its float sources itself: those take the float
# unflushed.
def fp32_flushed(x):
    return ('bcsel', ('ieq', ('iand', x, 0x7f800000), 0), ('iand', x, 0x80000000), x)

fp32_ftz = '!nir_is_denorm_preserve(info->float_controls_execution_mode, 32)'
for op in ['fadd', 'fmul', 'fmin', 'fmax', 'feq', 'fneu', 'flt', 'fge']:
    algebraic_lowering += [
        ((op, fp32_flushed('a@32'), b), (op, a, b), fp32_ftz),
        ((op, b, fp32_flushed('a@32')), (op, b, a), fp32_ftz),
    ]
for op in ['fsat', 'f2i32', 'f2u32']:
    algebraic_lowering += [((op, fp32_flushed('a@32')), (op, a), fp32_ftz)]
# An fp32 denormal is a zero of its sign in half precision either way
for op in ['f2f16', 'f2f16_rtne', 'f2f16_rtz', 'fquantize2f16']:
    algebraic_lowering += [((op, fp32_flushed('a@32')), (op, a))]
algebraic_lowering += [
    (('ffma', fp32_flushed('a@32'), b, c), ('ffma', a, b, c), fp32_ftz),
    (('ffma', b, fp32_flushed('a@32'), c), ('ffma', b, a, c), fp32_ftz),
    (('ffma', b, c, fp32_flushed('a@32')), ('ffma', b, c, a), fp32_ftz),
]
# A negated or absolute flushed float reaches an FTZ instruction as the flush's
# source would (negation and absolute value keep a denormal denormal)
for m in ['fneg', 'fabs']:
    for op in ['fadd', 'fmul', 'fmin', 'fmax', 'feq', 'fneu', 'flt', 'fge']:
        algebraic_lowering += [
            ((op, (m, fp32_flushed('a@32')), b), (op, (m, a), b), fp32_ftz),
            ((op, b, (m, fp32_flushed('a@32'))), (op, b, (m, a)), fp32_ftz),
        ]
    for op in ['fsat', 'f2i32', 'f2u32']:
        algebraic_lowering += [((op, (m, fp32_flushed('a@32'))), (op, (m, a)), fp32_ftz)]
    algebraic_lowering += [
        (('ffma', (m, fp32_flushed('a@32')), b, c), ('ffma', (m, a), b, c), fp32_ftz),
        (('ffma', b, (m, fp32_flushed('a@32')), c), ('ffma', b, (m, a), c), fp32_ftz),
        (('ffma', b, c, (m, fp32_flushed('a@32'))), ('ffma', b, c, (m, a)), fp32_ftz),
    ]
# What an FTZ instruction computes is never a denormal (it gives a zero of the
# result's sign instead), nor is a widened half (2^-24 is a normal f32): a flush
# of either is the value itself
for op in ['fadd', 'fmul', 'fmin', 'fmax']:
    algebraic_lowering += [(fp32_flushed((op, 'a@32', b)), (op, a, b), fp32_ftz)]
algebraic_lowering += [
    (fp32_flushed(('ffma', 'a@32', b, c)), ('ffma', a, b, c), fp32_ftz),
    (fp32_flushed(('fsat', 'a@32')), ('fsat', a), fp32_ftz),
    (fp32_flushed(('f2f32', 'a@16')), ('f2f32', a)),
]

# If we find mufu surrounded by bit_size conversions, just do the op in the
# original bit_size.
# MUFU.F16 internally appears to operate with the same precision as F32 does
# with the result being rounded towards zero to F16. EXP2 and RCP seem to be
# off by one around Inf, so it's only safe if we can ignore inf for those.
#
# This was verified with the `hw_tests::test_op_mufu_f16_down`.

# mufu.f16 for those is identical to mufu.f32 with rtz rounding except for results around infinity
for op in ['fexp2', 'frcp']:
    algebraic_lowering += [
        (('f2f16_rtz(ninf)', (op + '(is_used_once)', ('f2f32', 'a@16'))), (op, a), 'nak->sm >= 73'),
        (('f2f16(ninf)', (op + '(is_used_once)', ('f2f32', 'a@16'))),
            (op, a),
            'nak->sm >= 73 && ' + fp16_round_is_rtz),
    ]

# mufu.f16 for those is identical to mufu.f32 with rtz rounding
for op in ['fcos_normalized_2_pi', 'flog2', 'frsq', 'fsin_normalized_2_pi', 'fsqrt']:
    algebraic_lowering += [
        (('f2f16_rtz', (op + '(is_used_once)', ('f2f32', 'a@16'))), (op, a), 'nak->sm >= 73'),
        (('f2f16', (op + '(is_used_once)', ('f2f32', 'a@16'))),
            (op, a),
            'nak->sm >= 73 && ' + fp16_round_is_rtz),
    ]

# If contract is on we can always remove the conversions
for op in ['fcos_normalized_2_pi', 'fexp2', 'flog2', 'frcp', 'frsq', 'fsin_normalized_2_pi', 'fsqrt']:
    for f2f16 in ['f2f16_rtz', 'f2f16_rtne', 'f2f16']:
        algebraic_lowering += [
            ((f2f16 + '(contract)', (op + '(is_used_once)', ('f2f32', 'a@16'))),
                (op, a), 'nak->sm >= 73'),
            (('f2f32(contract)', (op + '(is_used_once)', (f2f16, 'a@32'))), (op, a)),
    ]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True, help='Output file.')
    parser.add_argument('-p', '--import-path', required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.import_path)

    import nir_algebraic  # pylint: disable=import-error

    try:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write('#include "nak_private.h"')
            f.write(nir_algebraic.AlgebraicPass(
                "nak_nir_lower_algebraic_late",
                algebraic_lowering,
                [
                    ("const struct nak_compiler *", "nak"),
                ]).render())
    except Exception:
        sys.exit(1)

if __name__ == '__main__':
    main()
