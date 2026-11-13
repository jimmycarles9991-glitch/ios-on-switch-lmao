//! A two-plane extended-range attachment's writes through output variables
//! (`pipeline::two_plane`): the raw texel's color word through the attachment's own variable, a
//! `uint`, and its alpha byte through one at the alpha index, a `uint` too.

use crate::error::{Error, Feature, Unsupported};
use crate::lower::arith::{Form, Held};
use crate::lower::emit::EmitCx;
use crate::lower::select::SelectCx;
use crate::metal::format::ExtendedRange;
use crate::pipeline::lower_inputs::ExtendedRangeColor;
use crate::pipeline::stage_io::{IoNumeric, IoRole, IoType, IoVariable};
use crate::pipeline::sym::VaryingKey;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value;
use crate::spirv::ir::{IoDecor, IoDir};

use super::plan::push_var;
use super::{Link, Place};

const UINT: IoType = IoType {
    numeric: IoNumeric::Uint,
    components: 1,
    source_bits: 32,
    bits: 32,
};

/// Plan the alpha variable of extended-range attachment `color` (written as `e`) where the
/// profile holds it in two planes: `Some` of the color word's own declared type then, `None`
/// where it is held in one image. A two-plane attachment without an alpha index is refused.
pub(super) fn plan(
    cx: &mut SelectCx<'_>,
    color: u32,
    e: ExtendedRangeColor,
) -> Result<Option<IoType>, Error> {
    if !matches!(e.form, ExtendedRange::Planes { .. }) {
        return Ok(None);
    }
    let Some(alpha) = cx.inputs.two_plane_alphas.get(&color).copied() else {
        if crate::pipeline::two_plane::held(cx.profile) {
            return Err(Unsupported::not_implemented(Feature::Format).into());
        }
        return Ok(None);
    };
    let sp = &mut cx.families.stage_io;
    let key = VaryingKey::color(alpha, 0);
    let object = super::plan::user(sp, key.clone(), IoDir::Output, UINT, IoDecor::default())?;
    push_var(
        &mut sp.interface.outputs,
        IoVariable {
            key,
            role: IoRole::Color {
                attachment: alpha,
                index: 0,
            },
            ty: UINT,
        },
    );
    sp.interface.two_plane_alphas.insert(color, alpha);
    sp.two_plane.insert(color, object);
    cx.descriptors.color_attachments = cx.descriptors.color_attachments.saturating_add(1);
    Ok(Some(UINT))
}

/// Store the raw components `texel` (the word, then the alpha byte) of a two-plane attachment
/// whose word's variable is `link` and whose alpha byte's is object `alpha`.
pub(super) fn store(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    link: &Link,
    alpha: usize,
    texel: Value,
) -> BResult<()> {
    let one = Form {
        held: Held::Int {
            bits: 32,
            container: 32,
        },
        lanes: 1,
    };
    let word = fb.composite_extract(texel, &[0])?;
    let byte = fb.composite_extract(texel, &[1])?;
    let word_link = Link { form: one, ..*link };
    super::store(cx, fb, &word_link, word)?;
    let alpha_link = Link {
        object: alpha,
        place: Place::Whole,
        form: one,
    };
    super::store(cx, fb, &alpha_link, byte)
}
