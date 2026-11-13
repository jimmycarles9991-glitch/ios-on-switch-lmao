//! A two-plane extended-range attachment's input attachments and storage views
//! (`pipeline::two_plane`): a read of its raw texel reads the word's image at the attachment's
//! index and the alpha byte's at its alpha index and joins them, the word in the first component
//! and the byte in the second, as one image holds them; a storage write splits the texel so.

use crate::lower::emit::EmitCx;
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::error::BResult;
use crate::spirv::ids::{GlobalId, Value as SValue};
use crate::spv::ImageFormat;

use super::fetch::StorageColor;

/// The alpha index of attachment `color`, where it is a two-plane one.
pub(super) fn alpha(cx: &EmitCx<'_>, color: u32) -> Option<u32> {
    cx.plan.families.fragment.two_plane.get(&color).copied()
}

/// The raw texel of a word's read `word` and an alpha byte's read `alpha`, each the `uvec4` its
/// one-component image gives.
pub(super) fn join(fb: &mut FunctionBuilder<'_>, word: SValue, alpha: SValue) -> BResult<SValue> {
    fb.vector_shuffle(word, alpha, &[0, 4, 2, 3])
}

/// The word's and the alpha byte's texels of raw texel `texel`, each in its first component.
pub(super) fn split(fb: &mut FunctionBuilder<'_>, texel: SValue) -> BResult<[SValue; 2]> {
    Ok([texel, fb.vector_shuffle(texel, texel, &[1, 1, 1, 1])?])
}

/// The storage views of the word and the alpha byte of two-plane storage-form attachment `color`
/// (alpha index `alpha`): `R32ui` and `R8ui`, the word's formatless where the attachment's is.
pub(super) fn declare_storage(
    mb: &mut ModuleBuilder,
    (color, alpha): (u32, u32),
    sc: &StorageColor,
    layered: bool,
) -> BResult<[GlobalId; 2]> {
    let mut word = *sc;
    if word.view.declared != ImageFormat::Unknown {
        word.view.declared = ImageFormat::R32ui;
    }
    let mut byte = *sc;
    byte.view.declared = ImageFormat::R8ui;
    Ok([
        super::fetch::declare(mb, color, &word, layered)?,
        super::fetch::declare(mb, alpha, &byte, layered)?,
    ])
}
