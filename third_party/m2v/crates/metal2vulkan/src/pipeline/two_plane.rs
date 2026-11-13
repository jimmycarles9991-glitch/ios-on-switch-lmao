//! `RGB10A8_2P_XR10` attachments held in Metal's two planes (`VkProperty::TwoPlaneExtendedRange`,
//! `LoweringInputs::two_plane_alphas`).
//!
//! Metal's bytes of such a texture are two planes: the color word (`BGR10_XR`'s, its top two bits
//! clear), 4 bytes a texel, and the alpha byte. A consumer that renders into the planes themselves
//! asks for a profile that holds the attachment so. The raw texel the function computes is the one
//! `ExtendedRange::Planes` holds in one image, the word in its first component and the byte in its
//! second; on such a profile each access of it is split at the image: the word goes to the
//! attachment's own index as `R32_UINT`, the byte to its alpha index as `R8_UINT`, and a read
//! takes both and joins them. Every output, input attachment and storage view of the attachment
//! has its twin at the alpha index, which the consumer binds to the alpha plane.
//!
//! The alpha index is the lowest color index the descriptor leaves without an attachment, taken
//! by the two-plane attachments in their order, so it is a fact of the descriptor alone, which a
//! pass of the pipeline's attachments shares. An attachment past the eighth free index has none,
//! and its writes and reads are refused (`NotImplemented(Format)`).

use std::collections::BTreeMap;

use crate::device::requirements::VkProperty;
use crate::device::DeviceProfile;
use crate::error::{Error, RuntimeInput};
use crate::library::FunctionArtifact;
use crate::metal::format::ExtendedRange;
use crate::pipeline::descriptor::ColorAttachmentDescriptor;
use crate::pipeline::graphics::{ColorAttachmentState, StorageAttachmentUse};
use crate::vk;

/// The color attachment count Metal's descriptors index.
const COLORS: u32 = 8;

/// Whether `p` holds two-plane attachments in two planes.
pub(crate) fn held(p: &DeviceProfile) -> bool {
    p.fields()
        .enabled
        .properties
        .contains(&VkProperty::TwoPlaneExtendedRange)
}

/// The alpha index of each two-plane attachment of `colors` on `p`, by the attachment's own index
/// (see the module docs); empty on a profile that holds them in one image.
pub(crate) fn alphas(
    colors: &[Option<ColorAttachmentDescriptor>],
    p: &DeviceProfile,
) -> BTreeMap<u32, u32> {
    if !held(p) {
        return BTreeMap::new();
    }
    let present = |i: u32| colors.get(i as usize).copied().flatten().is_some();
    let mut free = (0..COLORS).filter(|&i| !present(i));
    colors
        .iter()
        .enumerate()
        .filter(|(_, c)| {
            c.is_some_and(|c| {
                matches!(
                    ExtendedRange::of(c.pixel_format),
                    Some(ExtendedRange::Planes { .. })
                )
            })
        })
        .filter_map(|(i, _)| free.next().map(|a| (i as u32, a)))
        .collect()
}

/// Whether fragment stage `fragment` compiled under the alpha indices of a pipeline whose color
/// attachments are `colors` on `p`, for the two-plane attachments it writes or reads; else
/// `NeedsInput(ColorWrites)`.
pub(crate) fn check(
    fragment: Option<&FunctionArtifact>,
    colors: &[Option<ColorAttachmentDescriptor>],
    p: &DeviceProfile,
) -> Result<(), Error> {
    let Some(fragment) = fragment else {
        return Ok(());
    };
    let compiled = &fragment.interface.io.two_plane_alphas;
    let wanted = alphas(colors, p);
    match compiled
        .iter()
        .all(|(color, alpha)| wanted.get(color) == Some(alpha))
    {
        true => Ok(()),
        false => Err(Error::NeedsInput(RuntimeInput::ColorWrites)),
    }
}

/// The alpha index whose two-plane attachment `color` is, if it is one of `alphas`.
pub(crate) fn plane_of(alphas: &BTreeMap<u32, u32>, color: u32) -> Option<u32> {
    alphas
        .iter()
        .find_map(|(&word, &alpha)| (alpha == color).then_some(word))
}

/// A one-image raw texel's write mask (`R` the word, `G` the byte) as the word's and the byte's.
fn split_mask(mask: vk::ColorComponentFlags) -> [vk::ColorComponentFlags; 2] {
    let one = |bit: vk::ColorComponentFlags| match mask.contains(bit) {
        true => vk::ColorComponentFlags::R,
        false => vk::ColorComponentFlags::empty(),
    };
    [one(vk::ColorComponentFlags::R), one(vk::ColorComponentFlags::G)]
}

/// `colors` (link's states of a pipeline's color attachments) with each two-plane attachment's
/// one-image state split into its word's and its alpha byte's.
pub(crate) fn split_colors(
    colors: &mut Vec<Option<ColorAttachmentState>>,
    alphas: &BTreeMap<u32, u32>,
) {
    for (&word, &alpha) in alphas {
        let Some(state) = colors.get(word as usize).copied().flatten() else {
            continue;
        };
        let [w, a] = split_mask(state.write_mask);
        if colors.len() <= alpha as usize {
            colors.resize(alpha as usize + 1, None);
        }
        colors[word as usize] = Some(ColorAttachmentState {
            format: vk::Format::R32_UINT,
            write_mask: w,
            ..state
        });
        colors[alpha as usize] = Some(ColorAttachmentState {
            format: vk::Format::R8_UINT,
            write_mask: a,
            ..state
        });
    }
}

/// `storage` (link's storage-form attachments) with each two-plane attachment's one-image view
/// split into its word's and its alpha byte's, the byte's beside the word's. The kept channels
/// stay Metal's, which the function merges into the raw texel before the split.
pub(crate) fn split_storage(
    storage: Vec<StorageAttachmentUse>,
    alphas: &BTreeMap<u32, u32>,
) -> Vec<StorageAttachmentUse> {
    let mut out = Vec::with_capacity(storage.len());
    for s in storage {
        let Some(&alpha) = alphas.get(&s.color_index) else {
            out.push(s);
            continue;
        };
        out.push(StorageAttachmentUse {
            format: vk::Format::R32_UINT,
            ..s
        });
        out.push(StorageAttachmentUse {
            color_index: alpha,
            format: vk::Format::R8_UINT,
            formatless: false,
            ..s
        });
    }
    out
}
