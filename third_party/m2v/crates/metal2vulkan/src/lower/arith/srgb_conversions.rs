//! The compile's sRGB table: [`srgb::declare`]'s 256 words, then each conversion's answer read
//! once. `math::unpack` of `unorm4x8_srgb` reads a color byte's decode and alpha's quotient as
//! their `f32` bits, and `math::pack` reads an encode index's byte, where the runtime's table
//! (bind's kernels) computes them from the steps: `k / 4095` by two integer divisions a channel,
//! and an eight-load search. The answers are those computations' own, made here on the host
//! (`tests`), so a conversion's result does not change, only its cost: one load a channel.
//!
//! - words 0 to 255: [`srgb::declare`]'s, which `srgb::step` and `srgb::search` read;
//! - [`DECODED`] + `b`: the bits of `k / 4095` correctly rounded, `k` byte `b`'s decode step;
//! - [`QUOTIENT`] + `c`: the bits of `c / 255` correctly rounded;
//! - [`ENCODED`] + `i / 4`: the bytes of encode indices `i` to `i + 3`, the lowest index's in the
//!   low byte, each the largest byte whose first encode index is at most `i` (`srgb::search`).

use crate::lower::arith::srgb::{self, SRGB_DECODE_STEPS, SRGB_ENCODE_FIRST};
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::error::BResult;
use crate::spirv::ids::{GlobalId, Value};
use crate::spirv::ir::{BinOp, ConstBits, Constant, MemAccess, StorageClass, Type};

pub(crate) const DECODED: u32 = 256;
pub(crate) const QUOTIENT: u32 = 512;
pub(crate) const ENCODED: u32 = 768;
const LEN: u32 = ENCODED + 4096 / 4;

/// An upper estimate of the SPIR-V words of the table (its constants, the composite, and the
/// variable), charged to every use.
pub const TABLE_WORDS: u32 = LEN * 4 + LEN + 6 + 8;

/// An upper estimate of the SPIR-V words of one channel's read besides its operand (an access
/// chain, a load, the offset, and an encode's shift and mask).
pub const READ_WORDS: u32 = 8 * 6;

/// `k / d` correctly rounded to `f32`, as bits, for `0 <= k <= d < 2^16`, by integer arithmetic
/// (`exact_division::div_exact`'s, run on the host).
pub(crate) fn quotient_bits(k: u32, d: u32) -> u32 {
    if k == 0 {
        return 0;
    }
    let mut e = (31 - d.leading_zeros()) as i32 - (31 - k.leading_zeros()) as i32;
    let mut a = k << e;
    if a < d {
        a <<= 1;
        e += 1;
    }
    let hi = a << 12;
    let q1 = hi / d;
    let lo = (hi - q1 * d) << 11;
    let q2 = lo / d;
    let r2 = lo - q2 * d;
    let q = (q1 << 11) | q2;
    let up = 2 * r2 > d || (2 * r2 == d && q & 1 == 1);
    ((((126 - e) as u32) << 23) + q) + u32::from(up)
}

/// Encode index `i`'s byte (`i < 4096`).
pub(crate) fn encoded_byte(i: u32) -> u32 {
    SRGB_ENCODE_FIRST[1..].iter().filter(|&&first| u32::from(first) <= i).count() as u32
}

fn words() -> impl Iterator<Item = u32> {
    let steps = SRGB_DECODE_STEPS.iter().zip(SRGB_ENCODE_FIRST);
    let own = steps.map(|(step, first)| u32::from(*step) | (u32::from(first) << 16));
    let decoded = SRGB_DECODE_STEPS.iter().map(|k| quotient_bits(u32::from(*k), 4095));
    let quotient = (0..256).map(|c| quotient_bits(c, 255));
    let encoded =
        (0..1024).map(|w| (0..4).fold(0, |word, b| word | encoded_byte(4 * w + b) << (8 * b)));
    own.chain(decoded).chain(quotient).chain(encoded)
}

/// Declare the module's table: a `Private` `u32` array with its constant initializer.
pub fn declare(mb: &mut ModuleBuilder) -> BResult<GlobalId> {
    let u32_ = mb.ty(Type::Int {
        width: 32,
        signed: false,
    })?;
    let len = mb.constant(Constant::Scalar {
        ty: u32_,
        bits: ConstBits(u64::from(LEN)),
    })?;
    let arr = mb.ty(Type::Array {
        elem: u32_,
        len,
        stride: None,
    })?;
    let mut parts = Vec::with_capacity(LEN as usize);
    for w in words() {
        parts.push(mb.constant(Constant::Scalar {
            ty: u32_,
            bits: ConstBits(u64::from(w)),
        })?);
    }
    let init = mb.constant(Constant::Composite {
        ty: arr,
        parts: parts.into(),
    })?;
    mb.plain_global(StorageClass::Private, arr, Some(init))
}

fn word_at(fb: &mut FunctionBuilder<'_>, table: GlobalId, at: u32, i: Value) -> BResult<Value> {
    let at = fb.u32_const(at)?;
    let at = fb.module().const_value(at)?;
    let j = fb.binary(BinOp::IAdd, i, at)?;
    let base = fb.module().global_value(table)?;
    let p = fb.access_chain(base, &[j])?;
    fb.load(p, MemAccess::default())
}

/// The `f32` bits of Metal's decode of sRGB byte `b` (a `u32` scalar below 256).
pub fn decoded(fb: &mut FunctionBuilder<'_>, table: GlobalId, b: Value) -> BResult<Value> {
    word_at(fb, table, DECODED, b)
}

/// The `f32` bits of `c / 255` correctly rounded (`c` a `u32` scalar below 256).
pub fn quotient(fb: &mut FunctionBuilder<'_>, table: GlobalId, c: Value) -> BResult<Value> {
    word_at(fb, table, QUOTIENT, c)
}

/// The sRGB byte of encode index `i` (a `u32` scalar below 4096), as `srgb::search` finds it.
pub fn encoded(fb: &mut FunctionBuilder<'_>, table: GlobalId, i: Value) -> BResult<Value> {
    let k = |fb: &mut FunctionBuilder<'_>, v: u32| -> BResult<Value> {
        let c = fb.u32_const(v)?;
        fb.module().const_value(c)
    };
    let two = k(fb, 2)?;
    let w = fb.binary(BinOp::ShiftRightLogical, i, two)?;
    let w = word_at(fb, table, ENCODED, w)?;
    let three = k(fb, 3)?;
    let lane = fb.binary(BinOp::BitwiseAnd, i, three)?;
    let shift = fb.binary(BinOp::ShiftLeftLogical, lane, three)?;
    let byte = fb.binary(BinOp::ShiftRightLogical, w, shift)?;
    let mask = k(fb, 0xFF)?;
    fb.binary(BinOp::BitwiseAnd, byte, mask)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn search(i: u32) -> u32 {
        let mut b = 0u32;
        for step in [128u32, 64, 32, 16, 8, 4, 2, 1] {
            let j = b + step;
            if i >= u32::from(SRGB_ENCODE_FIRST[j as usize]) {
                b = j;
            }
        }
        b
    }

    #[test]
    fn quotients_are_correctly_rounded() {
        for (k, d) in (0..=4095).map(|k| (k, 4095)).chain((0..=255).map(|c| (c, 255))) {
            let exact = f64::from(k) / f64::from(d);
            let bits = quotient_bits(k, d);
            let got = f64::from(f32::from_bits(bits));
            let below = (bits > 0).then(|| bits - 1);
            for n in below.into_iter().chain([bits + 1]) {
                let other = f64::from(f32::from_bits(n));
                assert!((got - exact).abs() <= (other - exact).abs(), "{k} / {d}");
            }
            assert_eq!(got as f32, exact as f32, "{k} / {d}");
        }
    }

    #[test]
    fn every_index_encodes_as_the_search_does() {
        for i in 0..4096 {
            assert_eq!(encoded_byte(i), search(i), "index {i}");
        }
    }

    #[test]
    fn the_runtime_table_leads_the_compiles() {
        let own: Vec<u32> = words().take(256).collect();
        for (b, w) in own.iter().enumerate() {
            assert_eq!(w & 0xFFFF, u32::from(SRGB_DECODE_STEPS[b]));
            assert_eq!(w >> 16, u32::from(SRGB_ENCODE_FIRST[b]));
        }
        assert_eq!(words().count() as u32, LEN);
        let _ = srgb::TABLE_WORDS;
    }
}
