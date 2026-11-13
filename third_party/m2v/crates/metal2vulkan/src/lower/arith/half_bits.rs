// SM53 F2F keeps finite halves, including denormals and signed zeros. NaN conversion payloads are the device's;
// narrow::from_bits retains the original bits separately for bitcasts and unchanged memory values.
use super::legal::{FloatFmt, Form, Held};
use super::narrow::u32_form;
use super::ops::{fsplat, splat, ty};
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value;
use crate::spirv::ir::{BinOp, ConvOp, GlslOp, Type};

fn packed(fb: &mut FunctionBuilder<'_>, f: Form, x: Value) -> BResult<Value> {
    let half = fb.module().ty(Type::Float { width: 16 })?;
    let half = if f.lanes == 1 {
        half
    } else {
        fb.module().ty(Type::Vector {
            elem: half,
            len: f.lanes,
        })?
    };
    let rounded = fb.convert(ConvOp::FConvertRte, x, half)?;
    let wide = ty(fb, f)?;
    let x = fb.convert(ConvOp::FConvert, rounded, wide)?;
    let scalar = Form { lanes: 1, ..f };
    let pair = ty(fb, Form { lanes: 2, ..f })?;
    let u = u32_form(scalar);
    let zero = fsplat(fb, scalar, 0.0)?;
    let low = splat(fb, u, 0xffff)?;
    let shift = splat(fb, u, 16)?;
    let mut bits = Vec::with_capacity(usize::from(f.lanes));
    for i in (0..f.lanes).step_by(2) {
        let a = if f.lanes == 1 {
            x
        } else {
            fb.composite_extract(x, &[u32::from(i)])?
        };
        let b = if i + 1 < f.lanes {
            fb.composite_extract(x, &[u32::from(i + 1)])?
        } else {
            zero
        };
        let lanes = fb.composite_construct(pair, &[a, b])?;
        let word = fb.glsl(GlslOp::PackHalf2x16, &[lanes])?;
        bits.push(fb.binary(BinOp::BitwiseAnd, word, low)?);
        if i + 1 < f.lanes {
            bits.push(fb.binary(BinOp::ShiftRightLogical, word, shift)?);
        }
    }
    if f.lanes == 1 {
        Ok(bits[0])
    } else {
        let t = ty(fb, u32_form(f))?;
        fb.composite_construct(t, &bits)
    }
}

pub(super) fn encode(fb: &mut FunctionBuilder<'_>, u: Form, b: Value) -> BResult<Value> {
    let f = u.with(Held::Float(FloatFmt::F32));
    let ft = ty(fb, f)?;
    let x = fb.convert(ConvOp::Bitcast, b, ft)?;
    packed(fb, f, x)
}

pub(super) fn decode(fb: &mut FunctionBuilder<'_>, f: Form, h: Value) -> BResult<Value> {
    let mut lanes = Vec::with_capacity(usize::from(f.lanes));
    for i in 0..f.lanes {
        let lane = if f.lanes == 1 {
            h
        } else {
            fb.composite_extract(h, &[u32::from(i)])?
        };
        let pair = fb.glsl(GlslOp::UnpackHalf2x16, &[lane])?;
        lanes.push(fb.composite_extract(pair, &[0])?);
    }
    let ft = ty(fb, f)?;
    if f.lanes == 1 {
        Ok(lanes[0])
    } else {
        fb.composite_construct(ft, &lanes)
    }
}
