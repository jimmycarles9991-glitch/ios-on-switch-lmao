//! The fixed-function blend of a directly held color attachment, computed by its storage form.
//!
//! A function that reads the color it also blends into can only keep Metal's order on a device
//! that orders fetch through the pixel interlock alone if the blend happens inside the interlock:
//! the storage form reads the texel, blends the output over it, and stores the result, with the
//! attachment out of the subpass (no device blend). The result is meant to be the texel the
//! device's own blender would have stored for the same draw, and that blender's arithmetic is not
//! measured. The model is Vulkan's blend equations, which Metal documents alike, under these
//! assumptions ([`Assumption`]), each recorded with the pipeline so a measurement can retire it:
//!
//! - the source, second source and blend color of a normalized format are clamped to `[0, 1]`, a
//!   NaN to 0; a float format's are not clamped;
//! - the destination is the texel's exact value: a normalized code `c` of `b` bits is the `f32`
//!   nearest `c / (2^b - 1)`, a `half` its `f32`, an sRGB channel Metal's decode;
//! - each term is the operand times its factor rounded to `f32`, never fused with the sum, and the
//!   sum or difference of the terms is rounded to `f32`; a factor of zero drops its term (the
//!   result is the other term, negated where subtracted, whatever the dropped operand: an infinity
//!   or a NaN too), a factor of one is the operand itself; an output without alpha reads a source
//!   alpha of one, so its alpha factors are zero or one;
//! - `Min` and `Max` take the source and destination, the factors unread, and answer the other
//!   operand at a NaN;
//! - a format without alpha reads a destination alpha of one;
//! - `f32` denormals are flushed to zero, as the shader's arithmetic flushes them;
//! - the store converts as the device's color write does: a normalized channel clamped to `[0, 1]`
//!   (a NaN to 0) and rounded to the nearest code, a `half` rounded to nearest even, an sRGB
//!   channel by Metal's encode.

use crate::error::{Error, Feature, Unsupported};
use crate::lower::arith::legal::{FloatFmt, Form, Held};
use crate::lower::arith::ops::{fsplat, ty};
use crate::metal::{BlendFactor, BlendOperation};
use crate::pipeline::lower_inputs::BlendEquation;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value;
use crate::spirv::ir::{BinOp, ConvOp, GlslOp};
use crate::vk;

/// What the model assumes of the device's blender where it is not measured, as bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Assumption {
    /// Terms and their sum in `f32`, each rounded to nearest even, unfused.
    F32Arithmetic = 1,
    /// A factor of zero drops its term, a factor of one passes its operand.
    ZeroOneFactors = 2,
    /// A normalized source clamped to `[0, 1]` (NaN to 0), and the store rounding to the nearest
    /// code.
    UnormRounding = 4,
    /// A `half` store rounding to nearest even.
    HalfRounding = 8,
    /// An sRGB channel decoded and encoded by Metal's tables.
    SrgbTables = 16,
    /// `Min` and `Max` answer the non-NaN operand.
    MinMaxNumber = 32,
    /// `f32` denormals flushed to zero.
    Denormals = 64,
}

/// How a format's channels enter the blend.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Channels {
    /// Normalized: every operand clamped to `[0, 1]`, the code's step per lane.
    Unorm { steps: [u32; 4], srgb: bool },
    /// Float: nothing clamped, of a `half` (`half`) or an `f32` format.
    Float { half: bool },
}

/// The device's blend of one attachment, computed in the shader.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct RopBlend {
    pub eq: BlendEquation,
    pub channels: Channels,
    /// The format has an alpha channel.
    pub alpha: bool,
    /// The output has no alpha lane: the source's alpha is one.
    pub opaque_source: bool,
}

fn factors(eq: &BlendEquation) -> [BlendFactor; 4] {
    [
        eq.source_rgb,
        eq.destination_rgb,
        eq.source_alpha,
        eq.destination_alpha,
    ]
}

fn reads_second(f: BlendFactor) -> bool {
    use BlendFactor as F;
    matches!(
        f,
        F::Source1Color | F::OneMinusSource1Color | F::Source1Alpha | F::OneMinusSource1Alpha
    )
}

fn reads_constant(f: BlendFactor) -> bool {
    use BlendFactor as F;
    matches!(
        f,
        F::BlendColor | F::OneMinusBlendColor | F::BlendAlpha | F::OneMinusBlendAlpha
    )
}

/// Each channel's largest code of a normalized `format`, red first; `None` for a format whose
/// channels the model does not hold.
fn unorm_steps(format: vk::Format, bits: Option<u32>) -> Option<[u32; 4]> {
    match (format, bits) {
        (_, Some(b @ (8 | 16))) => Some([(1 << b) - 1; 4]),
        (vk::Format::A2B10G10R10_UNORM_PACK32 | vk::Format::A2R10G10B10_UNORM_PACK32, _) => {
            Some([1023, 1023, 1023, 3])
        }
        (vk::Format::A1R5G5B5_UNORM_PACK16, _) => Some([31, 31, 31, 1]),
        _ => None,
    }
}

impl RopBlend {
    /// The blend `eq` of an attachment held directly in `format`: `Err` for a format the model
    /// does not hold (a signed normalized, integer, packed float or narrow packed one).
    pub(crate) fn of(eq: &BlendEquation, format: vk::Format) -> Result<RopBlend, Error> {
        let refused = || Error::from(Unsupported::not_implemented(Feature::FramebufferFetch));
        for f in factors(eq) {
            if f == BlendFactor::Unspecialized {
                return Err(refused());
            }
        }
        for op in [eq.rgb_operation, eq.alpha_operation] {
            if op == BlendOperation::Unspecialized {
                return Err(refused());
            }
        }
        let block = format.block().ok_or_else(refused)?;
        let channels = match block.color {
            Some(vk::NumericFormat::Unorm) => Channels::Unorm {
                steps: unorm_steps(format, block.component_bits).ok_or_else(refused)?,
                srgb: false,
            },
            Some(vk::NumericFormat::Srgb) if block.component_bits == Some(8) => Channels::Unorm {
                steps: [255; 4],
                srgb: true,
            },
            Some(vk::NumericFormat::Sfloat) => match block.component_bits {
                Some(16) => Channels::Float { half: true },
                Some(32) => Channels::Float { half: false },
                _ => return Err(refused()),
            },
            _ => return Err(refused()),
        };
        let alpha = block.color_components.contains(vk::ColorComponentFlags::A);
        Ok(RopBlend {
            eq: *eq,
            channels,
            alpha,
            opaque_source: false,
        })
    }

    /// The blend of an output whose source alpha is one where `opaque`.
    pub(crate) fn with_opaque_source(self, opaque: bool) -> RopBlend {
        RopBlend {
            opaque_source: opaque,
            ..self
        }
    }

    pub(crate) fn reads_constant(&self) -> bool {
        factors(&self.eq).into_iter().any(reads_constant)
    }

    pub(crate) fn reads_second(&self) -> bool {
        factors(&self.eq).into_iter().any(reads_second)
    }

    /// The model's [`Assumption`]s this blend relies on, as a mask.
    pub(crate) fn assumptions(&self) -> u8 {
        use Assumption as A;
        let mut out = A::F32Arithmetic as u8 | A::Denormals as u8;
        let ops = [self.eq.rgb_operation, self.eq.alpha_operation];
        if ops
            .iter()
            .any(|op| matches!(op, BlendOperation::Min | BlendOperation::Max))
        {
            out |= A::MinMaxNumber as u8;
        }
        if factors(&self.eq)
            .iter()
            .any(|f| matches!(f, BlendFactor::Zero | BlendFactor::One))
        {
            out |= A::ZeroOneFactors as u8;
        }
        out | match self.channels {
            Channels::Unorm { srgb: true, .. } => A::UnormRounding as u8 | A::SrgbTables as u8,
            Channels::Unorm { .. } => A::UnormRounding as u8,
            Channels::Float { half: true } => A::HalfRounding as u8,
            Channels::Float { half: false } => 0,
        }
    }
}

fn floats() -> Form {
    Form {
        held: Held::Float(FloatFmt::F32),
        lanes: 4,
    }
}

/// `x` clamped to `[0, 1]`, a NaN to 0.
fn unit(fb: &mut FunctionBuilder<'_>, x: Value) -> BResult<Value> {
    let zero = fsplat(fb, floats(), 0.0)?;
    let one = fsplat(fb, floats(), 1.0)?;
    let above = fb.binary(BinOp::FOrdGreaterThan, x, zero)?;
    let x = fb.select(above, x, zero)?;
    fb.glsl(GlslOp::FMin, &[x, one])
}

/// The `f32` nearest `c / m` for each lane's code `c`, from `d`, a value within an `f32` step of
/// it (a decode that multiplies by the reciprocal): the code recovered, then one Newton step of
/// the quotient, which is exact for every code of up to 16 bits.
fn exact_unorm(fb: &mut FunctionBuilder<'_>, d: Value, steps: [u32; 4]) -> BResult<Value> {
    let m = per_lane(fb, steps.map(|s| s as f32))?;
    let scaled = fb.binary_no_contraction(BinOp::FMul, d, m)?;
    let code = fb.glsl(GlslOp::RoundEven, &[scaled])?;
    exact_quotient(fb, code, steps)
}

/// The `f32` nearest `c / m` for each lane's code `c` (a `vec4` of `f32`) and largest code `m`:
/// the product with `m`'s reciprocal and one Newton step, exact for every code of up to 16 bits.
pub(crate) fn exact_quotient(
    fb: &mut FunctionBuilder<'_>,
    code: Value,
    steps: [u32; 4],
) -> BResult<Value> {
    let m = per_lane(fb, steps.map(|s| s as f32))?;
    let r = per_lane(fb, steps.map(|s| 1.0 / s as f32))?;
    let q = fb.binary_no_contraction(BinOp::FMul, code, r)?;
    let negq = fb.unary(crate::spirv::ir::UnOp::FNegate, q)?;
    let e = fb.glsl(GlslOp::Fma, &[negq, m, code])?;
    fb.glsl(GlslOp::Fma, &[e, r, q])
}

fn per_lane(fb: &mut FunctionBuilder<'_>, v: [f32; 4]) -> BResult<Value> {
    let parts: Vec<Value> = v
        .into_iter()
        .map(|x| {
            let one = Form {
                lanes: 1,
                ..floats()
            };
            fsplat(fb, one, f64::from(x))
        })
        .collect::<BResult<_>>()?;
    let t = ty(fb, floats())?;
    fb.composite_construct(t, &parts)
}

fn splat_lane(fb: &mut FunctionBuilder<'_>, x: Value, lane: u32) -> BResult<Value> {
    fb.vector_shuffle(x, x, &[lane; 4])
}

fn one_minus(fb: &mut FunctionBuilder<'_>, x: Value) -> BResult<Value> {
    let one = fsplat(fb, floats(), 1.0)?;
    fb.binary_no_contraction(BinOp::FSub, one, x)
}

struct Operands {
    s: Value,
    s1: Option<Value>,
    d: Value,
    c: Option<Value>,
    opaque_source: bool,
}

/// `f` where the source's alpha is one: a factor of its alpha alone is one, of one minus it zero.
fn opaque_factor(f: BlendFactor) -> BlendFactor {
    use BlendFactor as F;
    match f {
        F::SourceAlpha => F::One,
        F::OneMinusSourceAlpha => F::Zero,
        f => f,
    }
}

/// The factor `f` of the color lanes (alpha's where `alpha`) as a `vec4`; `None` for zero and one,
/// which [`term`] takes apart.
fn factor(
    fb: &mut FunctionBuilder<'_>,
    o: &Operands,
    f: BlendFactor,
    alpha: bool,
) -> BResult<Option<Value>> {
    use BlendFactor as F;
    let second = |fb: &mut FunctionBuilder<'_>| match o.s1 {
        Some(v) => Ok(v),
        None => fsplat(fb, floats(), 0.0),
    };
    let constant = |fb: &mut FunctionBuilder<'_>| match o.c {
        Some(v) => Ok(v),
        None => fsplat(fb, floats(), 0.0),
    };
    Ok(Some(match f {
        F::Zero | F::One | F::Unspecialized => return Ok(None),
        F::SourceAlphaSaturated if alpha => return Ok(None),
        F::SourceColor => o.s,
        F::OneMinusSourceColor => one_minus(fb, o.s)?,
        F::SourceAlpha => splat_lane(fb, o.s, 3)?,
        F::OneMinusSourceAlpha => {
            let a = splat_lane(fb, o.s, 3)?;
            one_minus(fb, a)?
        }
        F::DestinationColor => o.d,
        F::OneMinusDestinationColor => one_minus(fb, o.d)?,
        F::DestinationAlpha => splat_lane(fb, o.d, 3)?,
        F::OneMinusDestinationAlpha => {
            let a = splat_lane(fb, o.d, 3)?;
            one_minus(fb, a)?
        }
        F::SourceAlphaSaturated => {
            let sa = splat_lane(fb, o.s, 3)?;
            let da = splat_lane(fb, o.d, 3)?;
            let m = one_minus(fb, da)?;
            fb.glsl(GlslOp::FMin, &[sa, m])?
        }
        F::BlendColor => constant(fb)?,
        F::OneMinusBlendColor => {
            let c = constant(fb)?;
            one_minus(fb, c)?
        }
        F::BlendAlpha => {
            let c = constant(fb)?;
            splat_lane(fb, c, 3)?
        }
        F::OneMinusBlendAlpha => {
            let c = constant(fb)?;
            let a = splat_lane(fb, c, 3)?;
            one_minus(fb, a)?
        }
        F::Source1Color => second(fb)?,
        F::OneMinusSource1Color => {
            let s1 = second(fb)?;
            one_minus(fb, s1)?
        }
        F::Source1Alpha => {
            let s1 = second(fb)?;
            splat_lane(fb, s1, 3)?
        }
        F::OneMinusSource1Alpha => {
            let s1 = second(fb)?;
            let a = splat_lane(fb, s1, 3)?;
            one_minus(fb, a)?
        }
    }))
}

/// `x` times factor `f` (as [`factor`] gives it): `None` for zero (the term is dropped), `x` for
/// one, else the product.
fn term(
    fb: &mut FunctionBuilder<'_>,
    o: &Operands,
    x: Value,
    f: BlendFactor,
    alpha: bool,
) -> BResult<Option<Value>> {
    let one = f == BlendFactor::One || (alpha && f == BlendFactor::SourceAlphaSaturated);
    match factor(fb, o, f, alpha)? {
        Some(k) => fb.binary_no_contraction(BinOp::FMul, x, k).map(Some),
        None if one => Ok(Some(x)),
        None => Ok(None),
    }
}

/// One lane class's result (`alpha`: alpha's factors and operation) as a `vec4`.
fn class(
    fb: &mut FunctionBuilder<'_>,
    o: &Operands,
    (fs, fd, op): (BlendFactor, BlendFactor, BlendOperation),
    alpha: bool,
) -> BResult<Value> {
    match op {
        BlendOperation::Min => return fb.glsl(GlslOp::NMin, &[o.s, o.d]),
        BlendOperation::Max => return fb.glsl(GlslOp::NMax, &[o.s, o.d]),
        _ => {}
    }
    let (fs, fd) = match o.opaque_source {
        true => (opaque_factor(fs), opaque_factor(fd)),
        false => (fs, fd),
    };
    let ts = term(fb, o, o.s, fs, alpha)?;
    let td = term(fb, o, o.d, fd, alpha)?;
    let (first, second) = match op {
        BlendOperation::ReverseSubtract => (td, ts),
        _ => (ts, td),
    };
    let add = op == BlendOperation::Add;
    match (first, second) {
        (Some(a), Some(b)) if add => fb.binary_no_contraction(BinOp::FAdd, a, b),
        (Some(a), Some(b)) => fb.binary_no_contraction(BinOp::FSub, a, b),
        (Some(a), None) => Ok(a),
        (None, Some(b)) if add => Ok(b),
        (None, Some(b)) => fb.unary(crate::spirv::ir::UnOp::FNegate, b),
        (None, None) => fsplat(fb, floats(), 0.0),
    }
}

/// Blend `b` of the output `s` (a `vec4` of `f32`, the output's lanes as the function computes
/// them) over `d`, the texel as the storage image reads it (a `vec4` of `f32`; an sRGB texel
/// already decoded), with second source `s1` where `b` reads it ([`RopBlend::reads_second`]) and
/// `color`, the encoder's blend color (a `uvec4` of its `f32`s' bits,
/// `PushField::HalfBlendColor`), where it reads that: the `vec4` the store converts (see the
/// module docs).
pub(crate) fn blend(
    fb: &mut FunctionBuilder<'_>,
    b: &RopBlend,
    (s, s1, d): (Value, Option<Value>, Value),
    color: Option<Value>,
) -> BResult<Value> {
    let t = ty(fb, floats())?;
    let c = match color {
        Some(words) => Some(fb.convert(ConvOp::Bitcast, words, t)?),
        None => None,
    };
    let (s, s1, c, d) = match b.channels {
        Channels::Unorm { steps, srgb } => {
            let s = unit(fb, s)?;
            let s1 = s1.map(|v| unit(fb, v)).transpose()?;
            let c = c.map(|v| unit(fb, v)).transpose()?;
            let d = match srgb {
                true => d,
                false => exact_unorm(fb, d, steps)?,
            };
            (s, s1, c, d)
        }
        Channels::Float { .. } => (s, s1, c, d),
    };
    let d = match b.alpha {
        true => d,
        false => {
            let one = fsplat(fb, floats(), 1.0)?;
            fb.vector_shuffle(d, one, &[0, 1, 2, 7])?
        }
    };
    let o = Operands {
        s,
        s1,
        d,
        c,
        opaque_source: b.opaque_source,
    };
    let eq = b.eq;
    let rgb = (eq.source_rgb, eq.destination_rgb, eq.rgb_operation);
    let a = (eq.source_alpha, eq.destination_alpha, eq.alpha_operation);
    let color_lanes = class(fb, &o, rgb, false)?;
    if rgb == a && !factors(&eq).contains(&BlendFactor::SourceAlphaSaturated) {
        return Ok(color_lanes);
    }
    let alpha_lanes = class(fb, &o, a, true)?;
    fb.vector_shuffle(color_lanes, alpha_lanes, &[0, 1, 2, 7])
}

#[cfg(test)]
mod tests;
