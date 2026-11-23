//! Exact roundings the other targets compute by integer sequences, on the Sm53 target in float
//! arithmetic mended by an exact residual: an exact product rounded to an integer
//! ([`round_exact_product_lanes`], for `math::round_exact_product_lanes`), and a quotient by a
//! constant ([`ratio_to_f32`], for `texel_round::ratio_to_f32`).

use super::exact_division::per_lane;
use super::legal::{FloatFmt, Form, Held};
use super::narrow::u32_form;
use super::ops::{fsplat, splat, ty};
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value;
use crate::spirv::ir::{BinOp, Contraction, ConvOp, GlslOp, Type, UnOp};

/// Whether the module rounds exact products by [`round_exact_product_lanes`]: on the Sm53 target,
/// whose `Fma` kept apart is one FFMA, a single rounding, which the residual must be.
pub(super) fn takes(fb: &mut FunctionBuilder<'_>) -> bool {
    fb.module().native_half_conversions()
}

/// `v` (`f32` lanes of form `s`, in `[0, 1.5]`, or `[-1, 1]` when `snorm`) times `m[i]` for lane
/// `i` (each below `2^16`), rounded to the nearest integer, ties to even, from the exact product,
/// as `u32` lanes (two's complement for a negative `snorm` result).
///
/// With `p = fl(a·m)` and `r` its integer to nearest even, `r` is the exact product's rounding
/// unless `p` is a half-integer the exact product is not: every half-integer below `2^22` is an
/// `f32`, so one strictly between the exact product and `p` would be nearer than `p`. Such a `p`
/// is at least `1/2`, so `a` is at least `2^-17` and the residual `fma(a, m, -p)` a multiple of
/// `a`'s step, `2^-40` or more: exact, normal, and of the sign that says which way the exact
/// product lies from the tie.
pub(super) fn round_exact_product_lanes(
    fb: &mut FunctionBuilder<'_>,
    s: Form,
    v: Value,
    m: &[u32],
    snorm: bool,
) -> BResult<Value> {
    let u = u32_form(s);
    let a = match snorm {
        true => fb.glsl(GlslOp::FAbs, &[v])?,
        false => v,
    };
    let scales: Vec<u64> = m.iter().map(|m| s.const_bits(f64::from(*m))).collect();
    let mk = per_lane(fb, s, &scales)?;
    let p = fb.binary_no_contraction(BinOp::FMul, a, mk)?;
    // `p + 2^23` holds `p` rounded to an integer, nearest even, in its low 23 bits (p < 2^22).
    let magic = fsplat(fb, s, 8_388_608.0)?;
    let q = fb.binary_no_contraction(BinOp::FAdd, p, magic)?;
    let r = fb.binary_no_contraction(BinOp::FSub, q, magic)?;
    let d = fb.binary_no_contraction(BinOp::FSub, p, r)?;
    let minus_p = fb.unary(UnOp::FNegate, p)?;
    let residual = fb.glsl_with(GlslOp::Fma, &[a, mk, minus_p], Contraction::Forbidden)?;
    let (zero, half, minus_half) = (
        fsplat(fb, s, 0.0)?,
        fsplat(fb, s, 0.5)?,
        fsplat(fb, s, -0.5)?,
    );
    let tie_below = fb.binary(BinOp::FOrdEqual, d, half)?;
    let above = fb.binary(BinOp::FOrdGreaterThan, residual, zero)?;
    let up = fb.binary(BinOp::LogicalAnd, tie_below, above)?;
    let tie_above = fb.binary(BinOp::FOrdEqual, d, minus_half)?;
    let below = fb.binary(BinOp::FOrdLessThan, residual, zero)?;
    let down = fb.binary(BinOp::LogicalAnd, tie_above, below)?;
    let ut = ty(fb, u)?;
    let bits = fb.convert(ConvOp::Bitcast, q, ut)?;
    let bias = splat(fb, u, 0x4B00_0000)?;
    let n = fb.binary(BinOp::ISub, bits, bias)?;
    let (one, none) = (splat(fb, u, 1)?, splat(fb, u, 0)?);
    let inc = fb.select(up, one, none)?;
    let n = fb.binary(BinOp::IAdd, n, inc)?;
    let dec = fb.select(down, one, none)?;
    let n = fb.binary(BinOp::ISub, n, dec)?;
    if !snorm {
        return Ok(n);
    }
    let neg = fb.binary(BinOp::FOrdLessThan, v, zero)?;
    let negated = fb.binary(BinOp::ISub, none, n)?;
    fb.select(neg, negated, n)
}

/// `k / d[i]` for lane `i` correctly rounded to `f32` (nearest even), for `u32` lanes `k` (form
/// `u`) with `0 <= k <= d[i]` and `d[i]` one of `EXACT_DIVISORS`: `q = fl(k · fl(1/d))` mended
/// once by its exact residual, `fma(fma(-q, d, k), fl(1/d), q)`. That is the correctly rounded
/// quotient at every such `k` for those divisors (each checked, `tests/native_sm53.rs`), not for
/// every divisor.
pub(super) fn ratio_to_f32(
    fb: &mut FunctionBuilder<'_>,
    u: Form,
    k: Value,
    d: &[u32],
) -> BResult<Value> {
    let f = u.with(Held::Float(FloatFmt::F32));
    let ft = ty(fb, f)?;
    let kf = fb.convert(ConvOp::ConvertUToF, k, ft)?;
    let divisor: Vec<u64> = d.iter().map(|d| f.const_bits(f64::from(*d))).collect();
    let divisor = per_lane(fb, f, &divisor)?;
    let reciprocal: Vec<u64> = d
        .iter()
        .map(|d| u64::from((1.0 / *d as f32).to_bits()))
        .collect();
    let reciprocal = per_lane(fb, f, &reciprocal)?;
    let q = fb.binary_no_contraction(BinOp::FMul, kf, reciprocal)?;
    let minus_q = fb.unary(UnOp::FNegate, q)?;
    let residual = fb.glsl_with(
        GlslOp::Fma,
        &[minus_q, divisor, kf],
        Contraction::Forbidden,
    )?;
    fb.glsl_with(
        GlslOp::Fma,
        &[residual, reciprocal, q],
        Contraction::Forbidden,
    )
}

/// The divisors [`ratio_to_f32`] answers correctly rounded for every `k` from 0 to the divisor:
/// an extended-range channel's 1020, a two-plane alpha byte's 255, a padded channel's 1023.
#[cfg(test)]
pub(super) const EXACT_DIVISORS: [u32; 3] = [1020, 255, 1023];

/// `v` (`f32` lanes of form `f`) rounded to `half` to nearest even, held as its `f32`, by GM20B's
/// conversion pair on the Sm53 target; `None` on any other target.
pub(super) fn half_nearest_even(
    fb: &mut FunctionBuilder<'_>,
    f: Form,
    v: Value,
) -> BResult<Option<Value>> {
    if !takes(fb) {
        return Ok(None);
    }
    let h = fb.module().ty(Type::Float { width: 16 })?;
    let h = match f.lanes {
        1 => h,
        len => fb.module().ty(Type::Vector { elem: h, len })?,
    };
    let n = fb.convert(ConvOp::FConvert, v, h)?;
    let ft = ty(fb, f.with(Held::Float(FloatFmt::F32)))?;
    fb.convert(ConvOp::FConvert, n, ft).map(Some)
}
