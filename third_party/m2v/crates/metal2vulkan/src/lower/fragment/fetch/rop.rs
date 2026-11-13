//! The storage form's write of an attachment whose fixed-function blend the shader computes
//! (`StorageView::rop`, `super::super::rop_blend`).

use super::{as_held, image_and_texel, srgb_decode, StorageColor, StorageView};
use crate::lower::arith::ops::fsplat;
use crate::lower::arith::{FloatFmt, Form, Held};
use crate::lower::emit::EmitCx;
use crate::lower::fragment::rop_blend::{blend, RopBlend};
use crate::lower::sync::memory_model;
use crate::pipeline::sym::PushField;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value as SValue;
use crate::spirv::ir::{ConvOp, Type as SType};

/// An upper estimate of the words of one modelled blend: the operands' clamps, the
/// destination's exact decode, two lane classes' factors and terms.
pub(super) const WORDS: u64 = 400;

/// The device's blend `view` computes in the shader, where it does.
pub(super) fn rop_of(view: &StorageView) -> Option<RopBlend> {
    view.blend
        .filter(|_| view.rop)
        .and_then(|eq| RopBlend::of(&eq, view.format).ok())
}

fn vec4_f32() -> Form {
    Form {
        held: Held::Float(FloatFmt::F32),
        lanes: 4,
    }
}

/// Output `part` of `form` as a `vec4` of `f32`: a narrower float widened, missing color lanes 0
/// and a missing alpha 1.
fn widened(fb: &mut FunctionBuilder<'_>, form: Form, part: SValue) -> BResult<SValue> {
    let f32_ = fb.module().ty(SType::Float { width: 32 })?;
    let to = crate::lower::arith::ops::ty(fb, form.with(Held::Float(FloatFmt::F32)))?;
    let part = match form.float_fmt().map(FloatFmt::container) {
        Some(32) => part,
        _ => fb.convert(ConvOp::FConvert, part, to)?,
    };
    if form.lanes == 4 {
        return Ok(part);
    }
    let scalar = Form {
        lanes: 1,
        ..vec4_f32()
    };
    let mut lanes = Vec::with_capacity(4);
    for k in 0..4u32 {
        lanes.push(match k {
            0 if form.lanes == 1 => part,
            k if k < u32::from(form.lanes) => fb.composite_extract(part, &[k])?,
            3 => fsplat(fb, scalar, 1.0)?,
            _ => fsplat(fb, scalar, 0.0)?,
        });
    }
    let vec4 = fb.module().ty(SType::Vector { elem: f32_, len: 4 })?;
    fb.composite_construct(vec4, &lanes)
}

/// The texel at the invocation's pixel (and sample) as the blend reads it: a `vec4` of `f32`, an
/// sRGB one decoded by Metal's table.
fn destination(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
) -> BResult<SValue> {
    let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
    let mem = memory_model::coherence(cx.plan).texel(fb)?;
    let texel = fb.read(image, at, &ops, mem)?;
    if sc.view.uint_view.is_none() {
        return as_held(fb, texel, sc.view.numeric);
    }
    if super::packed::is_packed(sc.view.format) {
        let word = fb.composite_extract(texel, &[0])?;
        return super::packed::decode(fb, sc.view.format, word);
    }
    let Some(table) = cx.decl.families.arith.srgb else {
        return Err(cx.fail("an sRGB blend without Metal's table"));
    };
    let word = fb.composite_extract(texel, &[0])?;
    srgb_decode(fb, sc.view.format, word, table)
}

/// Output `part` (of `form`) of attachment `color` blended over its texel by the device's blend,
/// computed by its model (`rop_blend`): a `vec4` of `f32` the store converts, the channels a
/// write mask keeps still to be merged. `second` is the function's index-1 output, where the
/// blend reads the second source.
pub(super) fn rop_blended(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    (form, part): (Form, SValue),
    second: Option<(Form, SValue)>,
) -> BResult<(Form, SValue)> {
    let Some(b) = rop_of(&sc.view) else {
        return Err(cx.fail("a modelled blend of a format outside the model"));
    };
    let b = b.with_opaque_source(form.lanes < 4);
    let s = widened(fb, form, part)?;
    let s1 = match (b.reads_second(), second) {
        (true, Some((form, v))) => Some(widened(fb, form, v)?),
        (true, None) => return Err(cx.fail("a blend of the second source without its output")),
        (false, _) => None,
    };
    let d = destination(cx, fb, color, sc)?;
    let constant = match b.reads_constant() {
        true => {
            let field = cx.push_field(fb, PushField::HalfBlendColor)?;
            Some(fb.composite_extract(field, &[0])?)
        }
        false => None,
    };
    let v = blend(fb, &b, (s, s1, d), constant)?;
    Ok((vec4_f32(), v))
}
