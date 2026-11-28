//! Framebuffer fetch through the ordered pixel interlock.
//!
//! **Where.** Metal orders a fetch after every earlier fragment of the pixel, across draws.
//! Vulkan orders an input-attachment read so only under rasterization-order attachment access
//! (`color`). A device without that access but with `fragmentShaderPixelInterlock`
//! ([`interlock_orders`]: RADV, ANV, and MoltenVK where measured) orders a fetch instead through
//! the ordered pixel interlock, the mechanism of raster order groups and imageblocks: each color
//! attachment the function reads becomes a storage image of the attachment
//! (`ResourceKind::TileAttachment(n)`), read at the fragment's pixel (the integer part of
//! `FragCoord.xy`), at each use or, where the function decodes it itself, once in its first
//! block ([`emit_entry`]), and, when the function writes that attachment through
//! its first (index 0) output, written there at the return in place of the output
//! (`GraphicsState::storage_attachments`; colors it writes but does not read stay ordinary outputs,
//! but those the pipeline's state names: a color its draw's render pass instance keeps out of its
//! subpass for other draws' storage form, `VariantKey::interlocked_colors`). The whole body runs
//! inside the critical section (`fragment::wrap_entry`), so the read, the body, and the write of
//! every fragment of a pixel are one step in primitive order, across the draws and instances of a
//! subpass, and the device-scope coherence of every access inside the critical section
//! (`sync::memory_model`, the image is `Coherent` under `GLSL450`, each access carries
//! `MakeTexel*` under the Vulkan model, as the body's other accesses do) makes each step's write
//! visible to the next: Metal's order. A discarded fragment writes nothing: a demoted invocation's
//! image writes have no effect, a terminate-form discard returns before the write (the entry kills
//! after the critical section), and a predicated one skips it.
//!
//! **Exactness, from the pipeline's state** (`LoweringInputs::framebuffer_fetch`, which the
//! render pipeline compile sets on such a device; without it the function is
//! `NeedsInput(FramebufferFetch)`). The storage write happens inside the shader, so it is exact
//! only where nothing after the shader would drop or change Metal's color write ([`storage_form`],
//! which link applies again to the pipeline it links into):
//!
//! - one view at a time: an amplified pipeline's draws are drawn one amplification each, every
//!   one into its layer of the pass (the binder refuses its storage form where they would be
//!   multiview views, which one image of the pass's layers does not address), and so in the
//!   layered form, which a multisampled function has none of ([`plan_layer`]);
//! - for a written attachment whose write mask covers some of its format's channels (one that
//!   covers none stores nothing, so none of this bears on it): no blending but one and zero, added,
//!   for color and alpha, which stores the source as no blending does (a signed normalized format's
//!   aside: its NaN stores another code under it; any other blend would need Metal's blend
//!   arithmetic in the shader, which is not measured), no alpha to coverage (it can drop the
//!   fragment's samples), no signed normalized format under a write mask that keeps some of its
//!   channels (its two codes of -1 read alike, so a channel written back would lose the texel's
//!   bits), and, with a depth or stencil attachment, early fragment tests: the module declares them
//!   there when they keep the function's writes Metal's (Metal's function declares them, or it runs
//!   no discard, exports no depth, stencil, or coverage, and writes no other memory), so a fragment
//!   the tests fail never runs. Without a depth or stencil attachment no test can fail, and the
//!   module declares none (unless Metal's function does). A function that only its discard keeps
//!   from them, under `RequireOrdered`, stores before the tests, which each draw's plan checks
//!   (`RuntimeFact::TestedStorageWrites`): a draw whose tests drop no fragment runs it, one whose
//!   tests write neither attachment and count nothing runs the variant that declares them
//!   (`LoweringInputs::read_only_tests`), and any other is refused. A function that writes other
//!   memory, or returns depth, stencil, or a sample mask, stores before the tests too, and states
//!   the fact without that variant: Metal runs its memory writes for the fragments its tests then
//!   drop and tests what it returns (EV XRR2-fx1-m4), which early tests would not, so only a draw
//!   whose tests drop no fragment runs it, and any other is refused at the depth (or stencil)
//!   attachment;
//! - a direct format, declared without a format where the device grants the format's storage
//!   access without one, so that one module serves every format the function reads alike, else with
//!   its SPIR-V image format where it has one (no format feature beyond storage,
//!   `shaderStorageImageExtendedFormats` for an extended one); an 8-bit four-channel sRGB format
//!   (`RGBA8Unorm_sRGB`, `BGRA8Unorm_sRGB`) through a view of the attachment in `R32_UINT`
//!   (`StorageView::uint_view`), whose bytes the function converts by Metal's measured conversions
//!   itself: a read decodes each color byte to its twelve-bit step over 4095 and alpha's to its
//!   byte over 255, a write encodes by Metal's thresholds (`math::unpack` and `math::pack` of
//!   `unorm4x8_srgb`; EV XRR2-srgb1-m4 on the M4: every byte read, a texel an earlier draw of the
//!   pass wrote read as the decode of its stored byte, every encode threshold, 4, 194, 304 floats
//!   and every half written). The one- and two-channel sRGB formats are not measured, and stay
//!   refused. `BGR5A1Unorm` and `BGR10A2Unorm`, where the device grants `A1R5G5B5_UNORM_PACK16`
//!   and `A2R10G10B10_UNORM_PACK32` no storage access (NVK), are viewed in `R16_UINT` and
//!   `R32_UINT` alike, their channels unpacked and packed by the function (`packed`).
//!
//! **Per sample**. Metal runs a function that reads a color attachment of a multisampled pass once
//! per covered sample, reading its own sample, in draw order per sample, with `[[position]]`'s x
//! and y at the pixel center (measured). Compiled for such a pipeline
//! (`LoweringInputs::multisampled`), the function reads (and writes) the attachment's sample at its
//! `SampleId` ([`plan_sample`]), which runs it once per covered sample under Vulkan's sample
//! shading; link's sample invocations then read its pixel inputs where Metal does or refuse them
//! ([`StorageSamples`]). Where the device has multisampled storage images, the image is
//! multisampled, of the pipeline's samples (`GraphicsState::rasterization_samples`, which link
//! checks against `storageImageSampleCounts`), read and written with the `Sample` operand.
//! Elsewhere (ANV, MoltenVK) it holds the samples as planes: a single-sample 2D-array image of one
//! layer per sample, sample `s` of pixel `(x, y)` at texel `(x, y, s)`, which the consumer fills
//! from the multisampled attachment before the draws and writes back after them
//! (`StorageAttachmentUse::view`). Either needs `sampleRateShading`. The pixel interlock orders
//! every invocation covering the pixel, and so each sample's. A returned sample mask keeps an
//! invocation's write exactly when it sets the invocation's sample's bit (measured), so each write
//! is gated on that bit ([`sample_bit`]); at one sample on bit 0 (measured). A driver that drops
//! the image writes of an early-tests fragment shader in a multisampled pass
//! (`DriverWorkaround::EarlyTestsDropMultisampledImageWrites`: MoltenVK on Apple's GPU) refuses the
//! per-sample form's writes where the module declares early tests (`Needs::DriverDefect`). Where
//! only a depth or stencil attachment would have the module declare them, it declares none there:
//! it stores before the tests and states `TestedStorageWrites` without the early-tests variant, so
//! only a draw whose tests drop no fragment runs it; a function whose Metal source declares early
//! tests keeps the refusal.
//!
//! **A write mask** short of the format's channels: Metal keeps the channels the mask leaves out.
//! The write reads the texel at the pixel first and writes those channels back as it read them
//! ([`StorageView::kept`]); inside the interlock no other fragment of the pixel writes the texel in
//! between, so every channel keeps its value (a `half` texel's NaN in the device's decode,
//! `PrecisionNote::DeviceNanEncoding`). A mask that covers none of the format's channels writes
//! nothing. Link checks the kept channels against the pipeline it links into. Every mask that keeps
//! some of a format's channels compiles alike: the function compiles under the mask's class
//! ([`compiled_write_mask`]: none, every channel, or some), and a write of the third class keeps
//! the lanes whose bits the draw's `PushField::StorageKeptChannels` sets
//! ([`StorageView::kept_at_draw`]), which the binder writes from the pipeline's
//! `StorageAttachmentUse::kept`, so the masks of a class share one module and one fragment key
//! (Metal's own backend compiles each new mask, EV SUB-mwm-m4). An sRGB attachment merges its
//! bytes, so it compiles each mask (no draw word).
//!
//! **An extended-range attachment** (`metal::format::ExtendedRange`) takes no storage form: one of
//! Apple's 10-bit extended-range formats renders only through its `RGBA16Float` stand-in, and a
//! padded or bfloat attachment's tile value and blends are not measured.
//!
//! A blocked attachment is refused under `RequireOrdered` with the blocker's reason (what the
//! device lacks, or `NotImplemented`), and read unordered as before under `AcceptUnordered`
//! (`PrecisionNote::UnorderedFramebufferFetch`): no exact path exists there. The storage form
//! stores Vulkan's texel output conversion of the output, the rule a color attachment write
//! states for the same formats.

use std::collections::BTreeSet;

use crate::air::abi::{AbiInt, OutputRole, ParamRole};
use crate::air::intrinsics::{AirIntrinsic, Fragment, PackDir, PackFormat};
use crate::air::ir::{ArgIdx, Callee, InstKind, Type as AirType, TypeId as AirTypeId, Value};
use crate::device::profile::{DriverWorkaround, FramebufferFetchPolicy};
use crate::device::requirements::{FormatUsage, VkFeature};
use crate::device::DeviceProfile;
use crate::device::HalfBlendPolicy;
use crate::error::{
    Attachment, Capability, DescriptorError, Error, Feature, RuntimeInput, Unsupported,
};
use crate::lower::arith::{extended_range, math, narrow, ops, srgb, FloatFmt, Form, Held};
use crate::lower::emit::EmitCx;
use crate::lower::select::SelectCx;
use crate::lower::stage_io::{self, Returned};
use crate::lower::sync::memory_model;
use crate::metal::format::{
    accessed_without_format, color_output_components, sampled_type, spirv_image_format,
    without_format_gaps, ExtendedRange, FormatEmulation, StorageUse,
};
use crate::metal::ColorWriteMask;
use crate::pipeline::artifact::PrecisionNote;
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::lower_inputs::{BlendEquation, FetchedAttachment, FramebufferFetchInputs};
use crate::pipeline::sym::{PushField, ResourceKind, ResourceSlot, SampledType, Stage};
use crate::pipeline::StorageAttachmentUse;
use crate::spirv::builder::constructs::{ArmEnd, ArmFn};
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::error::BResult;
use crate::spirv::ids::{GlobalId, Value as SValue};
use crate::spirv::ir::{
    BinOp, ConvOp, Dim, ImageOperands, ImageType, ImageUsage, MemAccess, StorageClass,
    Type as SType, UnOp,
};
use crate::vk;

use super::{not_lowered, ColorInput};

/// Whether `p` orders framebuffer fetch only through the ordered pixel interlock: no
/// rasterization-order color attachment access, with `fragmentShaderPixelInterlock`.
pub(crate) fn interlock_orders(p: &DeviceProfile) -> bool {
    let f = p.fields().fragment;
    !f.rasterization_order_color_access && f.pixel_interlock
}

/// Whether `p` asks for Metal's 16-bit float blend of a `half4` output, computed by the function
/// in the storage form (`DevicePolicy::half_blend`), which a device that orders fetch through
/// the pixel interlock alone ([`interlock_orders`]) takes.
pub(crate) fn blends_half_in_shader(p: &DeviceProfile) -> bool {
    p.fields().half_blend == HalfBlendPolicy::Exact && interlock_orders(p)
}

/// Whether `pf` is a 16-bit float color format, whose blend of a `half4` output of a module
/// without fast math Metal computes in `half` arithmetic.
pub(crate) fn half_float_format(pf: crate::metal::PixelFormat) -> bool {
    use crate::metal::PixelFormat as P;
    matches!(pf, P::R16Float | P::RG16Float | P::RGBA16Float)
}

/// Whether entry `e`, of a module compiled without fast math (`air.compile.fast_math_disable`),
/// writes color attachment `color` through a first (index 0) output of four `half`s and through
/// no second (index 1) one, whose 16-bit float blend Metal computes in `half` arithmetic.
pub(crate) fn half4_output(e: &crate::air::Entry, color: u32) -> bool {
    if e.compile.fast_math != Some(false) {
        return false;
    }
    let types = e.module.types();
    let Some(func) = e.module.func(e.func) else {
        return false;
    };
    let Some(AirType::Function { ret, .. }) = types.get(func.ty) else {
        return false;
    };
    let field = |i: u32| match types.get(*ret) {
        Some(AirType::Struct { fields, .. }) => fields.get(i as usize).copied(),
        Some(AirType::Void) | None => None,
        Some(_) => Some(*ret),
    };
    let half4 = |ty: AirTypeId| match types.get(ty) {
        Some(AirType::Vector { elem, len: 4 }) => matches!(types.get(*elem), Some(AirType::Half)),
        _ => false,
    };
    let to = |o: &crate::air::abi::OutputBinding, index: u32| {
        matches!(
            o.role,
            OutputRole::RenderTarget {
                attachment: AbiInt::Const(a),
                index: i,
                ..
            } if a == color && i == index
        )
    };
    let outputs = &e.ep.outputs;
    outputs
        .iter()
        .any(|o| to(o, 0) && field(o.field).is_some_and(half4))
        && !outputs.iter().any(|o| to(o, 1))
}

/// Whether a color attachment of `pf` whose blend is enabled where `blending_enabled`, whose
/// write mask is `mask` and whose blend is `equation` takes Metal's 16-bit float blend of a
/// `half4` output in the shader: a 16-bit float format, blended, some channel written, under a
/// blend Metal's model holds there (`extended_range::Blend::half_float`: no factor of the second
/// source).
pub(crate) fn half_blend_attachment(
    pf: crate::metal::PixelFormat,
    blending_enabled: bool,
    mask: ColorWriteMask,
    equation: Option<BlendEquation>,
) -> bool {
    half_float_format(pf)
        && blending_enabled
        && !mask.contains(ColorWriteMask::UNSPECIALIZED)
        && !channels(pf).intersection(mask).is_empty()
        && equation
            .is_some_and(|eq| extended_range::Blend::half_float(&eq, ColorWriteMask::ALL).is_ok())
}

/// What the function does that the storage form of one color depends on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ColorUse {
    /// The function writes the attachment through a first (index 0) output.
    pub writes: bool,
    /// The module declares early fragment tests: Metal's function does, or the pipeline has a
    /// depth or stencil attachment and the function's writes are Metal's under them (see the
    /// module docs).
    pub early_tests: bool,
    /// The module declares no early tests only because the function discards, and each draw's
    /// plan checks that its depth and stencil tests drop no fragment
    /// (`RuntimeFact::TestedStorageWrites`): a write then stores before them as Metal's does
    /// after them.
    pub draw_tested: bool,
    /// The function's math, which picks whether Metal's 16-bit float blend of a `half4` output
    /// is the shader's (`extended_range::blend::BlendMath::Rounded`).
    pub math: extended_range::blend::BlendMath,
}

/// How the storage form's image holds the attachment's samples.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StorageSamples {
    /// A pipeline of one sample: a single-sample 2D image, pixel `(x, y)` at texel `(x, y)`.
    One,
    /// The pipeline's samples in a multisampled 2D image, sample `s` of pixel `(x, y)` read and
    /// written at texel `(x, y)` with the `Sample` operand `s`.
    Multisampled,
    /// The pipeline's samples as planes, on a device without multisampled storage images: a
    /// single-sample 2D-array image of one layer per sample, sample `s` of pixel `(x, y)` at
    /// texel `(x, y, s)`.
    Planes,
}

/// How the storage form declares and views one attachment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StorageView {
    /// The attachment's Vulkan format, which the view takes (but an sRGB attachment's,
    /// [`uint_view`](Self::uint_view)).
    pub format: vk::Format,
    /// The declared SPIR-V image format: `Unknown` for a formatless declaration.
    pub declared: crate::spv::ImageFormat,
    /// The numeric type of the attachment's texels, the declaration's sampled type.
    pub numeric: SampledType,
    /// How the image holds the attachment's samples.
    pub samples: StorageSamples,
    /// The format's channels a write of the function keeps from the texel it overwrites: those
    /// outside the descriptor's write mask, which the storage write takes from the texel it reads
    /// first. Empty where the function does not write the attachment, or the mask covers every
    /// channel of the format; all four (`R`, `G`, `B` and `A`) where it covers none of them, so
    /// that the write stores nothing. Where [`kept_at_draw`](Self::kept_at_draw), the class's: the
    /// channels the descriptor's mask keeps are the draw's.
    pub kept: vk::ColorComponentFlags,
    /// The write keeps some channels and takes which from the draw: the bits of
    /// `PushField::StorageKeptChannels` for the attachment's color (`GraphicsState`'s
    /// `StorageAttachmentUse::kept`), not [`kept`](Self::kept), which names the class.
    pub kept_at_draw: bool,
    /// The blend a write computes in the shader, from the texel it reads first: a 16-bit float
    /// attachment's under Metal's `half` blend, or ([`rop`](Self::rop)) the device's
    /// fixed-function blend of any other directly held format. `None` for an unblended write, no
    /// write, or one the device blends.
    pub blend: Option<BlendEquation>,
    /// [`blend`](Self::blend) is the device's fixed-function blend, computed in the shader by its
    /// model (`rop_blend`), the attachment's device blend off.
    pub rop: bool,
    /// The unsigned-integer format of the texel size an sRGB or `BGR5A1Unorm` attachment is
    /// viewed in (`StorageAttachmentUse::uint_view`): the declaration's texels are its `uint`s,
    /// which a read unpacks and decodes and a write encodes and packs, by Metal's measured
    /// conversions. `None` for another format.
    pub uint_view: Option<vk::Format>,
}

impl StorageView {
    /// The numeric class of the function's reads and writes: a `uint`-viewed attachment's values
    /// are floats, whatever its texels.
    pub fn read_class(&self) -> SampledType {
        match self.uint_view {
            Some(_) => SampledType::Float,
            None => self.numeric,
        }
    }

    /// Whether a write stores nothing: the mask covers none of the format's channels.
    pub fn stores_nothing(&self) -> bool {
        self.kept == vk::ColorComponentFlags::known_bits()
    }

    /// Whether a write merges: it keeps some of the format's channels and stores the others.
    pub fn merges(&self) -> bool {
        !self.kept.is_empty() && !self.stores_nothing()
    }

    /// Whether the declaration names no format.
    pub fn formatless(&self) -> bool {
        self.declared == crate::spv::ImageFormat::Unknown
    }
}

/// One color attachment the function reads in the storage form.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StorageColor {
    pub view: StorageView,
    /// The function writes the attachment.
    pub writes: bool,
    /// The returned value's field that holds the write (0 for a returned value itself) and the
    /// output's form, once the stage interface planned it.
    pub output: Option<(u32, Form)>,
    /// The returned value's field that holds the function's second (index-1) output to the
    /// attachment and its form, once the stage interface planned it: the second source of the
    /// device's blend the shader computes ([`select_second_output`]).
    pub second: Option<(u32, Form)>,
}

fn not_implemented() -> Unsupported {
    Unsupported::not_implemented(Feature::FramebufferFetch)
}

/// Whether `format`'s float channels are narrower than 32 bits (a half-float or a packed float
/// format), whose texels a read decodes into `float`.
fn narrow_float(format: vk::Format) -> bool {
    format.block().is_some_and(|b| match b.color {
        Some(vk::NumericFormat::Sfloat) => b.component_bits != Some(32),
        Some(vk::NumericFormat::Ufloat) => true,
        _ => false,
    })
}

/// The channels of `mask` a write of every channel of a color attachment of `pf` must keep: its
/// first `color_output_components(pf)` of red, green, blue, and alpha (all four for a format of
/// none).
fn channels(pf: crate::metal::PixelFormat) -> ColorWriteMask {
    let n = color_output_components(pf).unwrap_or(4);
    let order = [
        ColorWriteMask::RED,
        ColorWriteMask::GREEN,
        ColorWriteMask::BLUE,
        ColorWriteMask::ALPHA,
    ];
    order
        .iter()
        .take(usize::from(n))
        .fold(ColorWriteMask::NONE, |m, &c| m.union(c))
}

/// Whether a storage-form write to an attachment of `pf` that keeps some of its channels takes
/// them from the draw (`PushField::StorageKeptChannels`), so that every such write mask compiles
/// alike: every format but an extended-range one, which takes no storage form, and an sRGB one,
/// whose write merges the bytes of its `uint` view.
pub(crate) fn kept_at_draw(pf: crate::metal::PixelFormat) -> bool {
    let srgb = crate::metal::format::direct_format(pf)
        .and_then(|f| f.block())
        .is_some_and(|b| b.color == Some(vk::NumericFormat::Srgb));
    ExtendedRange::of(pf).is_none() && !srgb
}

/// The channels a write of the attachment of `pf` under write mask `mask` keeps from its texel:
/// none where the function does not write it (`writes` false) or the mask covers every channel of
/// the format, all four (`R`, `G`, `B` and `A`) where it covers none of them (the write stores
/// nothing), else those of the format the mask leaves out.
pub(crate) fn kept_channels(
    pf: crate::metal::PixelFormat,
    mask: ColorWriteMask,
    writes: bool,
) -> ColorWriteMask {
    let all = channels(pf);
    if !writes {
        ColorWriteMask::NONE
    } else if all.intersection(mask).is_empty() {
        ColorWriteMask::ALL
    } else {
        all.difference(mask)
    }
}

/// The write mask a fragment function compiles the storage form of an attachment of `pf` under,
/// for the descriptor's `mask` (`FramebufferFetchInputs::of`): where the write takes its kept
/// channels from the draw ([`kept_at_draw`]), the class of `mask` alone (none where it covers
/// none of the format's channels, all where it covers every one, and for a mask covering some,
/// every channel of the format but the first, the one mask that stands for them all), so the
/// write masks of a class share one key and one module; any other format's `mask` as it is.
pub(crate) fn compiled_write_mask(
    pf: crate::metal::PixelFormat,
    mask: ColorWriteMask,
) -> ColorWriteMask {
    if !kept_at_draw(pf) || mask.contains(ColorWriteMask::UNSPECIALIZED) {
        return mask;
    }
    let all = channels(pf);
    let written = all.intersection(mask);
    if written.is_empty() {
        ColorWriteMask::NONE
    } else if written == all {
        ColorWriteMask::ALL
    } else {
        all.difference(ColorWriteMask::RED)
    }
}

/// The storage form of the read of color attachment `color` of a pipeline whose state is
/// `inputs` (`multisampled`: it rasterizes more than one sample, so the form is per sample) by a
/// function whose use of it is `use_`, on `p` (see the module docs): `Ok(Ok(view))` where the
/// form is exact, `Ok(Err(why))`
/// where the pipeline's state, the function, or the device blocks it, and `Err` for a
/// descriptor error that refuses the pipeline whatever the form (no attachment at `color`, a
/// pixel format no color attachment can have).
pub(crate) fn storage_form(
    color: u32,
    inputs: &FramebufferFetchInputs,
    multisampled: bool,
    use_: ColorUse,
    p: &DeviceProfile,
) -> Result<Result<StorageView, Unsupported>, Error> {
    let attachment: FetchedAttachment =
        inputs
            .attachments
            .get(&color)
            .copied()
            .flatten()
            .ok_or(Error::InvalidDescriptor(DescriptorError::ColorAttachment {
                index: color,
            }))?;
    let pf = attachment.pixel_format;
    let usages = BTreeSet::from([FormatUsage::ColorAttachment]);
    let mapping = crate::metal::format::vk_format(pf, &usages, p)?;
    // A bfloat attachment's fetch is not measured, as `color` refuses it.
    match ExtendedRange::of(pf) {
        Some(e) if e.bfloat() => return Ok(Err(Unsupported::not_implemented(Feature::Format))),
        Some(_) => return Ok(Err(not_implemented())),
        None => {}
    }
    let format = mapping.format;
    let srgb = format
        .block()
        .is_some_and(|b| b.color == Some(vk::NumericFormat::Srgb));
    // An 8-bit four-channel sRGB attachment is viewed in `R32_UINT`, whose bytes the function
    // decodes and encodes by Metal's measured conversions (EV XRR2-srgb1-m4: a fetch reads Metal's
    // twelve-bit decode of the stored byte, `math::unpack`'s, even after an earlier draw of the
    // pass wrote it, and a write stores `math::pack`'s encode). The one- and two-channel sRGB
    // formats are not measured, nor is a blend into one.
    // A `BGR5A1Unorm` or `BGR10A2Unorm` attachment, which NVK grants no storage access, is viewed
    // in `R16_UINT` or `R32_UINT`, its channels unpacked and packed by the function (`packed`).
    let uint_view = StorageAttachmentUse::uint_view_of(format).filter(|&u| {
        mapping.emulation == FormatEmulation::Direct
            && ((srgb && u == vk::Format::R32_UINT) || packed::is_packed(format))
    });
    let viewed = uint_view.unwrap_or(format);
    let writes = use_.writes;
    // The channels a write keeps from the texel: those of the format the descriptor's mask leaves
    // out, or all four where it leaves out every one (the write stores nothing). An `Unspecialized`
    // mask is a placeholder, which no form serves.
    let all = channels(pf);
    let kept = kept_channels(pf, attachment.write_mask, writes);
    let merged = !kept.is_empty() && kept != ColorWriteMask::ALL;
    // A write that keeps some channels of a format whose kept channels are the draw's: the
    // write mask is its class's (`compiled_write_mask`), and the draw's word names them.
    let kept_draw = merged && kept_at_draw(pf);
    let snorm = format
        .block()
        .is_some_and(|b| b.color == Some(vk::NumericFormat::Snorm));
    // A returned sample mask keeps the invocation's own sample when it sets its bit: the per-sample
    // form gates its write on that bit, and a write at one sample on bit 0. A kept channel of a
    // signed normalized format beside a written one is not modelled either: its two codes of -1
    // read alike, so writing back what was read would not keep the texel's bits. A write whose
    // mask covers none of the format's channels stores nothing, so nothing a store depends on
    // (the blend, coverage, the sample mask, the depth and stencil tests) bears on it; and a blend
    // of one and zero, added, for color and alpha stores the source as blending disabled does, a
    // signed normalized NaN aside.
    let stores = writes && kept != ColorWriteMask::ALL;
    // The blend of a 16-bit float attachment of a module without fast math, which the shader
    // computes in Metal's `half` arithmetic where the profile asks for it
    // (`DevicePolicy::half_blend`), for a `half4` output, which [`plan`] checks; its gap is
    // refused by name (`extended_range::Blend::half_float`).
    let half_blend = p.fields().half_blend == HalfBlendPolicy::Exact
        && use_.math == extended_range::blend::BlendMath::Rounded
        && half_float_format(pf)
        && attachment.blending_enabled;
    // Any other blend of a directly held attachment the write stores: the device's fixed-function
    // blend, computed in the shader by its model (`rop_blend`), so that the read, the blend and
    // the store are one step inside the interlock. A replacing blend stores the source as no
    // blending does, but into an sRGB attachment, whose blend runs in linear space.
    let rop = attachment.blending_enabled
        && !half_blend
        && stores
        && (!attachment.replaces || snorm || uint_view.is_some())
        && attachment
            .equation
            .is_some_and(|eq| super::rop_blend::RopBlend::of(&eq, format).is_ok());
    let blend = attachment
        .equation
        .filter(|_| (half_blend || rop) && stores);
    if let Some(eq) = blend.filter(|_| !rop) {
        let written = all.intersection(attachment.write_mask);
        match extended_range::Blend::half_float(&eq, written) {
            Ok(_) => {}
            Err(Error::Unsupported(gap)) => return Ok(Err(gap)),
            Err(e) => return Err(e),
        }
    }
    let shader_blends = blend.is_some();
    let blends = attachment.blending_enabled && !(attachment.replaces && !snorm) && !shader_blends;
    let not_modelled = mapping.emulation != FormatEmulation::Direct
        || (inputs.amplified && multisampled)
        || (srgb && uint_view.is_none())
        // Metal's blend into an sRGB attachment, a replacing one too, is not measured: the device's
        // is modelled.
        || (uint_view.is_some() && stores && attachment.blending_enabled && !rop)
        || (writes
            && attachment
                .write_mask
                .contains(ColorWriteMask::UNSPECIALIZED))
        || (stores
            && (blends
                || (snorm && merged)
                || inputs.alpha_to_coverage
                || (inputs.depth_or_stencil && !use_.early_tests && !use_.draw_tested)));
    if not_modelled {
        return Ok(Err(not_implemented()));
    }
    let kept = crate::metal::state::color_write_mask(kept)?;
    let f = p.fields();
    // The per-sample form's writes are storage image writes of the fragment shader; where it
    // declares early tests, a driver that drops such writes in a pass of more than one sample
    // would lose every one.
    let defect = DriverWorkaround::EarlyTestsDropMultisampledImageWrites;
    if multisampled && writes && use_.early_tests && f.workarounds.contains(&defect) {
        return Ok(Err(Unsupported::driver_defect(
            Feature::FramebufferFetch,
            defect,
        )));
    }
    let mut missing = Vec::new();
    // The per-sample form: the image read and written at the invocation's `SampleId`, which needs
    // sample-rate shading. It is multisampled where the device has multisampled storage images (its
    // sample count is the pipeline's, which link checks against `storageImageSampleCounts`), and
    // planes elsewhere.
    let samples = match (multisampled, f.images.storage_multisample) {
        (false, _) => StorageSamples::One,
        (true, true) => StorageSamples::Multisampled,
        (true, false) => StorageSamples::Planes,
    };
    if multisampled && !f.fragment.sample_rate_shading {
        missing.push(Capability::Feature(VkFeature::SampleRateShading));
    }
    if writes && !f.leaf.fragment_stores_and_atomics {
        missing.push(Capability::Feature(VkFeature::FragmentStoresAndAtomics));
    }
    if !p.supports_format(viewed, FormatUsage::StorageImage) {
        missing.push(Capability::Format {
            format: viewed,
            usage: FormatUsage::StorageImage,
        });
    }
    // Declared without a format where the device grants the format that access without one, so
    // that the declaration, and with it the module, is the same for every format the function
    // reads alike (a `BGRA8Unorm` attachment's and an `RGBA16Float` one's); else with its SPIR-V
    // image format where the device can (no format feature beyond storage).
    let use_without = StorageUse::texture(true, writes);
    let typed = spirv_image_format(viewed).filter(|d| {
        !crate::spirv::requirements::extended_format(*d) || f.images.storage_extended_formats
    });
    let declared = match typed {
        _ if accessed_without_format(viewed, use_without, p) => {
            Some(crate::spv::ImageFormat::Unknown)
        }
        Some(d) => Some(d),
        None => {
            match spirv_image_format(viewed) {
                Some(_) => missing.push(Capability::Feature(
                    VkFeature::ShaderStorageImageExtendedFormats,
                )),
                None => missing.extend(without_format_gaps(viewed, use_without, p)),
            }
            None
        }
    };
    let numeric = match uint_view {
        Some(_) => SampledType::Uint,
        None => sampled_type(pf).ok_or_else(not_implemented)?,
    };
    match declared {
        Some(declared) if missing.is_empty() => Ok(Ok(StorageView {
            format,
            declared,
            numeric,
            samples,
            kept,
            kept_at_draw: kept_draw,
            blend,
            rop,
            uint_view,
        })),
        _ => Ok(Err(Unsupported::needs_any_of(
            Feature::FramebufferFetch,
            vec![missing],
        ))),
    }
}

/// What the function's body holds that the storage form depends on, from one walk.
#[derive(Default)]
struct BodyFacts {
    /// The LLVM parameters the body reads.
    used: BTreeSet<u32>,
    discards: bool,
    side_effects: bool,
}

/// How the depth and stencil tests bear on the function's storage writes, which it makes before
/// them where the module declares no early tests (module docs).
#[derive(Clone, Copy)]
struct StoredBeforeTests {
    /// Only its discard keeps the function from early tests: its stores are Metal's where a
    /// draw's tests drop no fragment, and under early tests where they write and count nothing.
    tested: bool,
    /// It writes other memory, or returns depth, stencil, or a sample mask, or early tests would
    /// lose its writes (`dropped`): its stores are Metal's only where a draw's tests drop no
    /// fragment.
    untested: bool,
}

/// [`StoredBeforeTests`] of a function whose body holds `body`, that Metal runs under early tests
/// where `declared_early` and returns depth, stencil, or a sample mask where `exports`, on `p`,
/// in a pipeline with a depth or stencil attachment where `depth_or_stencil`, where early tests
/// would drop its storage writes where `dropped`. Under `AcceptUnordered` neither: the function
/// reads unordered, which no draw's tests refuse.
fn stored_before_tests(
    p: &DeviceProfile,
    body: &BodyFacts,
    (declared_early, exports): (bool, bool),
    (depth_or_stencil, dropped): (bool, bool),
) -> StoredBeforeTests {
    let ordered = p.fields().framebuffer_fetch == FramebufferFetchPolicy::RequireOrdered;
    let testable = depth_or_stencil && !(exports || body.side_effects);
    StoredBeforeTests {
        tested: !declared_early && testable && body.discards && ordered,
        untested: !declared_early
            && depth_or_stencil
            && (exports || body.side_effects || dropped)
            && ordered,
    }
}

fn body_facts(cx: &SelectCx<'_>) -> BodyFacts {
    let mut out = BodyFacts::default();
    for (id, inst) in cx.body.insts.iter() {
        inst.kind.for_each_value(|v| {
            if let Value::Arg(a) = v {
                out.used.insert(a.raw());
            }
        });
        if let InstKind::Call(call) = &inst.kind {
            if matches!(
                call.callee,
                Callee::Air {
                    op: AirIntrinsic::Fragment(Fragment::Discard),
                    ..
                }
            ) {
                out.discards = true;
            }
        }
        if super::discard::side_effect(cx, id, inst) {
            out.side_effects = true;
        }
    }
    out
}

/// The color attachments the function reads: those of its used `[[color(n)]]` parameters of a
/// constant index (another index is refused when the parameter is planned). Each such
/// parameter's type is checked here, so a function no pipeline state would make lowerable is
/// refused before it needs one.
fn colors_read(cx: &SelectCx<'_>, used: &BTreeSet<u32>) -> Result<BTreeSet<u32>, Error> {
    let mut out = BTreeSet::new();
    for b in cx
        .entry
        .ep
        .params
        .iter()
        .filter(|b| used.contains(&b.param))
    {
        if let ParamRole::ColorInput {
            index: AbiInt::Const(color),
            ..
        } = b.role
        {
            super::color::param_form(cx, b)
                .map_err(|e| e.at_attachment(Attachment::Color(color)))?;
            out.insert(color);
        }
    }
    Ok(out)
}

/// The first extended-range attachment the function writes through a first output where the
/// pipeline blends it (`LoweringInputs::extended_range_colors`).
fn blended_extended_range(cx: &SelectCx<'_>) -> Option<u32> {
    cx.entry.ep.outputs.iter().find_map(|o| match o.role {
        OutputRole::RenderTarget {
            attachment: AbiInt::Const(color),
            index: 0,
            ..
        } if cx
            .inputs
            .extended_range_colors
            .get(&color)
            .is_some_and(|e| e.blended) =>
        {
            Some(color)
        }
        _ => None,
    })
}

/// Decide the form of every color attachment the function reads, and of every other attachment
/// the pipeline's state names that it writes (a color its draw's instance keeps out of its
/// subpass), before the stage interface is planned (its outputs to a storage-form attachment are
/// not interface variables): on a device that orders fetch only through the interlock
/// ([`interlock_orders`]), the storage form where it is exact; elsewhere, and where it is blocked
/// under `AcceptUnordered`, the input attachment (`color`). A blocked attachment under
/// `RequireOrdered` refuses the function.
pub(super) fn plan(cx: &mut SelectCx<'_>) -> Result<(), Error> {
    if cx.stage != Stage::Fragment || !interlock_orders(cx.profile) {
        return Ok(());
    }
    let body = body_facts(cx);
    let outputs = &cx.entry.ep.outputs;
    let returns = |role: fn(&OutputRole) -> bool| outputs.iter().any(|o| role(&o.role));
    let exports = returns(|r| {
        matches!(
            r,
            OutputRole::Depth(_) | OutputRole::Stencil | OutputRole::SampleMask
        )
    });
    let declared_early = matches!(
        cx.entry.ep.stage,
        crate::air::abi::Stage::Fragment {
            early_fragment_tests: true
        }
    );
    // A draw may move a color the function writes through its first output into the storage
    // form: a color its render pass instance keeps out of its subpass for other draws'
    // interlocked form (`VariantKey::interlocked_colors`), single-sampled, so no driver drops its
    // writes under early tests. The plan of the pipeline as compiled, which
    // stores no color so, selects the tests such a draw's variant needs from this.
    let movable = stored_before_tests(cx.profile, &body, (declared_early, exports), (true, false));
    if (movable.tested || movable.untested)
        && returns(|r| matches!(r, OutputRole::RenderTarget { index: 0, .. }))
    {
        cx.facts.insert(RuntimeFact::MovableStorageWrites {
            early_variant: movable.tested && !movable.untested,
        });
    }
    // A padded or bfloat attachment's blend has no model (Metal's validation refuses a bfloat
    // one's): no storage form blends it.
    if let Some(color) = blended_extended_range(cx) {
        let refused = Unsupported::not_implemented(Feature::Format);
        return Err(refused.at_attachment(Attachment::Color(color)).into());
    }
    let fetched = colors_read(cx, &body.used)?;
    let mut read = fetched.clone();
    // Every other attachment the pipeline's state names that the function writes through its
    // first output: a color the draw moved into the storage form (above), written through it as a
    // fetched one is, or one named for Metal's 16-bit float blend alone.
    if let Some(inputs) = &cx.inputs.framebuffer_fetch {
        read.extend(outputs.iter().filter_map(|o| match o.role {
            OutputRole::RenderTarget {
                attachment: AbiInt::Const(color),
                index: 0,
                ..
            } if inputs.attachments.contains_key(&color) => Some(color),
            _ => None,
        }));
    }
    if read.is_empty() {
        return Ok(());
    }
    let Some(inputs) = cx.inputs.framebuffer_fetch.clone() else {
        return Err(Error::NeedsInput(RuntimeInput::FramebufferFetch));
    };
    // The module declares early fragment tests so that a depth or stencil test the fragment
    // fails drops its storage writes, as it drops Metal's color write; it may only where they
    // keep the function's writes Metal's (no discard, no depth, stencil, or coverage export, no
    // other memory write). Without a depth or stencil attachment no test can fail, so the
    // declaration would change nothing, and it is left out: Apple's GPU drops the image writes
    // of an early-tests fragment shader in a pass of more than one sample.
    let testable = inputs.depth_or_stencil && !(exports || body.side_effects);
    // A driver that drops the image writes of an early-tests fragment shader in a pass of more
    // than one sample would lose every storage write of a function that writes a fetched color
    // beside a depth or stencil attachment under early tests: such a function declares none (unless
    // Metal's does), and stores before the tests, Metal's where its draws' tests drop no fragment;
    // it has no early-tests variant. (its form checks the driver's defect itself, and the device
    // blends it where that blocks it)
    let writes_read = read
        .iter()
        .filter(|c| !inputs.half_blends.contains(c))
        .any(|&color| {
            outputs.iter().any(|o| {
                matches!(
                    o.role,
                    OutputRole::RenderTarget {
                        attachment: AbiInt::Const(a),
                        index: 0,
                        ..
                    } if a == color
                )
            })
        });
    let dropped = cx.inputs.multisampled
        && inputs.depth_or_stencil
        && writes_read
        && cx
            .profile
            .fields()
            .workarounds
            .contains(&DriverWorkaround::EarlyTestsDropMultisampledImageWrites);
    // A function that only its discard keeps from early tests, on a device that requires ordered
    // fetch (`tested`): its storage writes are Metal's where its draws' tests drop no fragment,
    // before the tests, and where they write neither attachment and count no visibility result,
    // under early tests (`LoweringInputs::read_only_tests`), which then drop no fragment Metal's
    // later tests keep and leave the attachments as Metal's do; the plan of each draw checks
    // which (`RuntimeFact::TestedStorageWrites`). One that writes other memory, or returns depth,
    // stencil, or a sample mask, beside a depth or stencil attachment, or whose driver would drop
    // its writes under early tests (`untested`): Metal runs its memory writes for every fragment,
    // those its tests then drop included, and tests what it returns (measured on Apple's M4), which
    // early tests would not, so its storage writes before the tests are Metal's where its draws'
    // tests drop no fragment, which the plan of each draw checks; it has no early-tests variant.
    // Under `AcceptUnordered` it reads unordered, as before, which no draw's tests refuse.
    let StoredBeforeTests { tested, untested } = stored_before_tests(
        cx.profile,
        &body,
        (declared_early, exports),
        (inputs.depth_or_stencil, dropped),
    );
    let early_tests = declared_early
        || (testable && !body.discards && !untested)
        || (tested && cx.inputs.read_only_tests && !untested);
    for color in read {
        // Compiled under the state of a pipeline for another function.
        if !inputs.attachments.contains_key(&color) {
            return Err(Error::NeedsInput(RuntimeInput::FramebufferFetch));
        }
        let writes = outputs.iter().any(|o| {
            matches!(
                o.role,
                OutputRole::RenderTarget {
                    attachment: AbiInt::Const(a),
                    index: 0,
                    ..
                } if a == color
            )
        });
        // An attachment the pipeline names only for Metal's 16-bit float blend: the storage form
        // where it is exact before the draw's tests, else the device's blend. Its write never
        // waits on each draw's check of its tests.
        let optional = inputs.half_blends.contains(&color);
        let use_ = ColorUse {
            writes,
            early_tests,
            draw_tested: (tested || untested) && !early_tests && !optional,
            math: blend_math(cx),
        };
        let tied = |e: Error| e.at_attachment(Attachment::Color(color));
        let form =
            storage_form(color, &inputs, cx.inputs.multisampled, use_, cx.profile).map_err(tied)?;
        // The device blends a color the function writes but never reads: its blend stays the
        // device's, outside the interlock.
        let form = match form {
            Ok(view) if view.rop && !fetched.contains(&color) => Err(not_implemented()),
            form => form,
        };
        let form = match form {
            Ok(view) if view.rop => match rop_of(&view).is_some_and(|b| b.reads_second()) {
                false => Ok(view),
                true => match second_output4(cx, color)? {
                    Some(Some(_)) => Ok(view),
                    _ => Err(not_implemented()),
                },
            },
            // Metal's 16-bit float blend, modelled for a `half4` output alone.
            Ok(view) if view.blend.is_some() => match half4_output(cx.entry, color) {
                true => Ok(view),
                false => Err(not_implemented()),
            },
            form => form,
        };
        let form = match form {
            // Where the storage form is not exact, the device blends the attachment.
            Err(_) if optional => continue,
            form => form,
        };
        match form {
            Ok(view) => {
                // The blend color as the encoder set it, where Metal's 16-bit float blend reads
                // it.
                if let Some(eq) = view.blend {
                    let reads = match view.rop {
                        true => rop_of(&view).is_some_and(|b| b.reads_constant()),
                        false => extended_range::Blend::half_float(&eq, written(view.kept))
                            .is_ok_and(|h| h.reads_constant()),
                    };
                    if reads {
                        cx.push.insert(PushField::HalfBlendColor);
                    }
                }
                // The device's blend, by its model: the stored value stands for the device's.
                if view.rop {
                    cx.precision.insert(PrecisionNote::DeviceColorConversion);
                }
                // The channels a partial write mask keeps, as the draw's word.
                if view.kept_at_draw {
                    cx.push.insert(PushField::StorageKeptChannels);
                }
                cx.families.fragment.fetch.insert(
                    color,
                    StorageColor {
                        view,
                        writes,
                        output: None,
                        second: None,
                    },
                );
                cx.facts.insert(RuntimeFact::InterlockedFramebufferFetch {
                    color,
                    format: view.format,
                    writes,
                    early_tests,
                    kept: view.kept,
                    blend: view.blend,
                    rounded: view.blend.is_some()
                        && use_.math == extended_range::blend::BlendMath::Rounded,
                });
                cx.descriptors.storage_images = cx.descriptors.storage_images.saturating_add(1);
                // A merged write stores the kept channels as it read them: a `half` (or a packed
                // float) texel's NaN is the device's decode, as for any read of one.
                if view.merges() && narrow_float(view.format) {
                    cx.precision.insert(PrecisionNote::DeviceNanEncoding);
                }
            }
            Err(blocked) => match cx.profile.fields().framebuffer_fetch {
                FramebufferFetchPolicy::RequireOrdered => return Err(tied(blocked.into())),
                // No exact path: the input attachment reads unordered (`color`).
                FramebufferFetchPolicy::AcceptUnordered => {}
            },
        }
    }
    if !cx.families.fragment.fetch.is_empty() {
        cx.families.fragment.fetch_early_tests = early_tests && !declared_early;
    }
    // Whether a write stores is the draw's tests' to decide. A write whose mask covers none of
    // its attachment's channels stores nothing, so no test bears on it: a moved color's neither,
    // for which the plan of the pipeline before the move selects no tests either
    // (`RuntimeFact::MovableStorageWrites`).
    let stores = cx
        .families
        .fragment
        .fetch
        .values()
        .any(|sc| sc.writes && !sc.view.stores_nothing());
    if (tested || untested) && stores {
        cx.facts.insert(RuntimeFact::TestedStorageWrites {
            early_tests,
            early_variant: tested && !untested,
        });
        // Compiled for a draw whose depth test alone can drop a fragment, the function runs that
        // test itself, before its store (`depth_test`).
        if cx.inputs.shader_tests {
            // A returned depth is tested, a returned stencil is the stencil test's reference, and
            // a returned sample mask keeps the fragment's samples before the tests.
            let field = |role: fn(&OutputRole) -> bool| {
                outputs.iter().find(|o| role(&o.role)).map(|o| o.field)
            };
            let exports = super::depth_test::Exports {
                depth: field(|r| matches!(r, OutputRole::Depth(_))),
                stencil: field(|r| matches!(r, OutputRole::Stencil)),
                mask: field(|r| matches!(r, OutputRole::SampleMask)),
            };
            super::depth_test::plan(cx, exports)?;
        }
    }
    // A per-sample function's returned mask gates its storage writes. At one sample too, on the
    // mask's bit 0 (measured: Metal stores no color there without it, EV DSX-mask1-m4).
    let written = cx.families.fragment.fetch.values().any(|sc| sc.writes);
    if written {
        cx.families.fragment.fetch_mask = outputs
            .iter()
            .find(|o| matches!(o.role, OutputRole::SampleMask))
            .map(|o| o.field);
    }
    Ok(())
}

/// Plan the invocation's sample for a function compiled for a multisampled pipeline
/// (`LoweringInputs::multisampled`), which reads each color it fetches, in either form, at its
/// `SampleId`: Metal runs a function that reads a color attachment once per covered sample, reading
/// its own sample, and a `SampleId` input runs a Vulkan fragment shader once per covered sample.
/// Nothing for one sample.
pub(super) fn plan_sample(cx: &mut SelectCx<'_>) -> Result<(), Error> {
    if !cx.inputs.multisampled || cx.families.fragment.fetch_sample.is_some() {
        return Ok(());
    }
    if !cx.profile.fields().fragment.sample_rate_shading {
        return Err(Unsupported::needs(
            Feature::FramebufferFetch,
            Capability::Feature(VkFeature::SampleRateShading),
        )
        .into());
    }
    cx.families.fragment.fetch_sample = Some(stage_io::sample_id(cx));
    Ok(())
}

/// Plan the invocation's layer for a function compiled for a layered pass
/// (`LoweringInputs::layered_storage`), at which each storage-form image is addressed; nothing
/// otherwise. A multisampled function's storage form in a layered pass is not implemented (its
/// sample planes are an image's layers already).
pub(super) fn plan_layer(cx: &mut SelectCx<'_>) -> Result<(), Error> {
    if !cx.inputs.layered_storage || cx.families.fragment.fetch_layer.is_some() {
        return Ok(());
    }
    if cx.inputs.multisampled {
        return Err(not_implemented().into());
    }
    cx.families.fragment.fetch_layer = Some(stage_io::layer(cx)?);
    cx.facts.insert(RuntimeFact::LayeredStorage);
    Ok(())
}

/// Whether the function reads color attachment `color` in the storage form.
pub(crate) fn is_storage(cx: &SelectCx<'_>, color: u32) -> bool {
    cx.families.fragment.fetch.contains_key(&color)
}

/// Whether a value of `form` has the numeric class of `numeric` (a float of `Float`, an integer
/// of `Uint` or `Sint`). A narrower integer written to a signed-integer attachment is not
/// lowered: which extension fills the attachment's lanes is not established (as for a color
/// output). A read takes the texel's low bits, as an input attachment's does.
fn class_of(form: Form, numeric: SampledType, color: u32, write: bool) -> Result<(), Error> {
    let ok = match numeric {
        SampledType::Float => form.is_float(),
        SampledType::Uint => !form.is_float(),
        SampledType::Sint => !form.is_float(),
        // No color attachment's format reads as `ulong`.
        SampledType::Uint64 => false,
    };
    if !ok {
        return Err(Error::InvalidDescriptor(DescriptorError::ColorAttachment {
            index: color,
        }));
    }
    if write && numeric == SampledType::Sint && !matches!(form.held, Held::Int { bits: 32, .. }) {
        return Err(not_lowered());
    }
    Ok(())
}

/// Plan the read of storage-form color input `c`: its class against the attachment's, and the
/// pixel's `FragCoord`.
pub(super) fn select_read(cx: &mut SelectCx<'_>, c: ColorInput) -> Result<(), Error> {
    let Some(sc) = cx.families.fragment.fetch.get(&c.color).copied() else {
        return Err(cx.invariant("a storage-form read of an attachment of another form"));
    };
    class_of(c.form, sc.view.read_class(), c.color, false)?;
    // An sRGB texel decodes through Metal's twelve-bit table.
    if sc.view.uint_view.is_some() && !packed::is_packed(sc.view.format) {
        cx.families.arith.srgb.get_or_insert(PackDir::Unpack);
    }
    plan_sample(cx)?;
    plan_layer(cx)?;
    let coord = stage_io::frag_coord(cx);
    cx.families.fragment.fetch_coord = Some(coord);
    Ok(())
}

/// The function's second (index-1) output to attachment `color`: `None` where it writes none,
/// `Some(Some(fmt))` where it is four lanes of `fmt`, a `half` or a `float`, and `Some(None)` for
/// another.
pub(super) fn second_output4(
    cx: &SelectCx<'_>,
    color: u32,
) -> Result<Option<Option<FloatFmt>>, Error> {
    for (o, ty) in cx
        .entry
        .ep
        .outputs
        .iter()
        .zip(stage_io::output_types(cx)?.1)
    {
        if let OutputRole::RenderTarget {
            attachment: AbiInt::Const(c),
            index: 1,
            ..
        } = o.role
        {
            if c == color {
                let f = super::color::form(cx, ty)?;
                let fmt = f.float_fmt().filter(|_| f.lanes == 4);
                return Ok(Some(
                    fmt.filter(|f| matches!(f, FloatFmt::Half | FloatFmt::F32)),
                ));
            }
        }
    }
    Ok(None)
}

/// Whether the function's second (index-1) output to attachment `color` is the second source of
/// the device's blend the shader computes in the storage form ([`select_second_output`]), which
/// no interface variable holds.
pub(crate) fn blends_second(cx: &SelectCx<'_>, color: u32) -> bool {
    cx.families
        .fragment
        .fetch
        .get(&color)
        .is_some_and(|sc| sc.view.rop && sc.view.blend.is_some())
}

/// Plan the second (index-1) output of AIR type `ty` held in field `field` of the returned value
/// to storage-form attachment `color`, whose device blend the shader computes ([`blends_second`]):
/// the blend reads it where its factors name the second source; else nothing stores it, as the
/// blend there reads none.
pub fn select_second_output(
    cx: &mut SelectCx<'_>,
    field: u32,
    color: u32,
    ty: AirTypeId,
) -> Result<(), Error> {
    let form = super::color::form(cx, ty)?;
    let Some(sc) = cx.families.fragment.fetch.get_mut(&color) else {
        return Err(cx.invariant("a second output of an attachment of another form"));
    };
    sc.second = Some((field, form));
    Ok(())
}

/// Plan the output of AIR type `ty` held in field `field` of the returned value to storage-form
/// attachment `color` (`stage_io` plans the entry's other outputs): its form has the
/// attachment's class and at least its format's channels, as a color output must.
pub fn select_output(
    cx: &mut SelectCx<'_>,
    field: u32,
    color: u32,
    ty: AirTypeId,
) -> Result<(), Error> {
    let form = super::color::form(cx, ty)?;
    let Some(sc) = cx.families.fragment.fetch.get(&color).copied() else {
        return Err(cx.invariant("a storage-form output of an attachment of another form"));
    };
    class_of(form, sc.view.read_class(), color, true)?;
    // The device's blend of a float attachment, computed by its model in `f32`, keeps a product
    // with a zero and a sum with one as IEEE has them: a lane the output (or its widening to four
    // lanes) holds constant would otherwise fold away a `0 * inf` or a `-0 + 0` the ROP keeps.
    if rop::rop_of(&sc.view)
        .is_some_and(|b| matches!(b.channels, super::rop_blend::Channels::Float { .. }))
    {
        cx.families.arith.preserve.insert(32);
    }
    let pf = cx
        .inputs
        .framebuffer_fetch
        .as_ref()
        .and_then(|i| i.attachments.get(&color).copied().flatten())
        .map(|a| a.pixel_format)
        .ok_or_else(|| cx.invariant("a storage-form output without its attachment"))?;
    if color_output_components(pf).is_some_and(|n| form.lanes < n) {
        return Err(Error::InvalidDescriptor(DescriptorError::ColorAttachment {
            index: color,
        }));
    }
    plan_sample(cx)?;
    plan_layer(cx)?;
    // The write rounds to the code Metal stores where the format converts it; an sRGB attachment's
    // write encodes its bytes itself (`math::pack` of a `float` or `half` output), and records the
    // pipeline's map, which link checks, without rounding a lane.
    if sc.view.uint_view.is_some() {
        let measured = matches!(
            form.float_fmt(),
            Some(crate::lower::arith::FloatFmt::F32 | crate::lower::arith::FloatFmt::Half)
        );
        if !measured || form.lanes != 4 {
            return Err(Unsupported::not_implemented(Feature::Format).into());
        }
        if !packed::is_packed(sc.view.format) {
            cx.families.arith.srgb.get_or_insert(PackDir::Pack);
        }
        if let Some(rules) = cx.inputs.color_writes.get(&color).copied() {
            cx.families
                .stage_io
                .interface
                .color_writes
                .insert(color, rules);
        }
    } else {
        stage_io::color_write::plan(cx, color, form);
    }
    let coord = stage_io::frag_coord(cx);
    let fp = &mut cx.families.fragment;
    fp.fetch_coord = Some(coord);
    match fp.fetch.get_mut(&color) {
        Some(sc) if sc.writes && sc.output.is_none() => {
            sc.output = Some((field, form));
            Ok(())
        }
        _ => Err(invariant("a storage-form output the plan does not expect")),
    }
}

fn invariant(detail: &str) -> Error {
    Error::Internal(crate::error::InternalError::new(
        crate::error::InternalKind::Invariant,
        detail,
    ))
}

/// Check the plan once every output is planned: each written storage-form attachment has its
/// output.
pub(super) fn finish(cx: &SelectCx<'_>) -> Result<(), Error> {
    if cx
        .families
        .fragment
        .fetch
        .values()
        .any(|sc| sc.writes != sc.output.is_some())
    {
        return Err(invariant(
            "a written storage-form attachment without its output",
        ));
    }
    Ok(())
}

/// An upper estimate of the words the storage form adds: one object per attachment, the pixel's
/// coordinate (and sample) and a read per use, and at each of `rets` returns the coordinate, the
/// guard (with the returned mask's bit), and a write per written attachment (a read first where
/// it merges kept channels).
pub(super) fn words(cx: &SelectCx<'_>, uses: u64, rets: u64) -> u64 {
    const OBJECT: u64 = 40;
    const READ: u64 = 56;
    const WRITE: u64 = 56;
    let fetch = &cx.families.fragment.fetch;
    if fetch.is_empty() {
        return 0;
    }
    let written = fetch.values().filter(|sc| sc.writes).count() as u64;
    let merged = fetch.values().filter(|sc| sc.view.merges()).count() as u64;
    // A merge of the draw's kept channels tests four bits and selects.
    const KEPT_AT_DRAW: u64 = 40;
    let at_draw = fetch.values().filter(|sc| sc.view.kept_at_draw).count() as u64;
    // The device's blend by its model: the texel read, decoded, and the blend's lanes.
    let modelled = fetch.values().filter(|sc| sc.view.rop).count() as u64;
    // An sRGB attachment's read reorders its bytes and decodes three channels' steps and four
    // quotients.
    let read = match fetch.values().any(|sc| sc.view.uint_view.is_some()) {
        true => READ.saturating_add(u64::from(3 * srgb::STEP_WORDS) + 400),
        false => READ,
    };
    // An sRGB attachment's write encodes four channels (three through Metal's thresholds),
    // reorders its bytes, and merges kept ones from the texel it reads first.
    let srgb_writes = fetch
        .values()
        .filter(|sc| sc.writes && sc.view.uint_view.is_some())
        .count() as u64;
    let srgb_write = READ.saturating_add(u64::from(3 * srgb::SEARCH_WORDS) + 400);
    let write = WRITE
        .saturating_mul(written)
        .saturating_add(READ.saturating_mul(merged))
        .saturating_add(KEPT_AT_DRAW.saturating_mul(at_draw))
        .saturating_add((READ + rop::WORDS).saturating_mul(modelled))
        .saturating_add(srgb_write.saturating_mul(srgb_writes))
        .saturating_add(40);
    OBJECT
        .saturating_mul(fetch.len() as u64)
        .saturating_add(read.saturating_mul(uses))
        .saturating_add(rets.saturating_mul(write))
}

/// Declare storage-form attachment `color`'s storage image: a 2D storage image of its declared
/// format and its numeric type, multisampled or arrayed as it holds the samples
/// ([`StorageSamples`]), or arrayed over a layered pass's layers (`layered`).
pub(super) fn declare(
    mb: &mut ModuleBuilder,
    color: u32,
    sc: &StorageColor,
    layered: bool,
) -> BResult<GlobalId> {
    let sampled_type = scalar_type(mb, sc.view.numeric)?;
    let ty = mb.ty(SType::Image(ImageType {
        sampled_type,
        dim: Dim::D2,
        depth: false,
        arrayed: sc.view.samples == StorageSamples::Planes || layered,
        multisampled: sc.view.samples == StorageSamples::Multisampled,
        usage: ImageUsage::Storage,
        format: sc.view.declared,
    }))?;
    let slot = ResourceSlot {
        stage: Stage::Fragment,
        kind: ResourceKind::TileAttachment(color),
        index: color,
    };
    mb.resource(slot, StorageClass::UniformConstant, ty)
}

/// The 32-bit scalar type of `numeric`.
fn scalar_type(mb: &mut ModuleBuilder, numeric: SampledType) -> BResult<crate::spirv::ids::TypeId> {
    match numeric {
        SampledType::Float => mb.ty(SType::Float { width: 32 }),
        // A `ulong` texture's image is `RG32` unsigned.
        SampledType::Uint | SampledType::Uint64 => mb.ty(SType::Int {
            width: 32,
            signed: false,
        }),
        SampledType::Sint => mb.ty(SType::Int {
            width: 32,
            signed: true,
        }),
    }
}

/// The storage image of attachment `color` held as `samples`, the texel of the invocation's
/// sample, and the access's image operands: the invocation's [`pixel`] with its [`operands`],
/// or, for planes, the pixel and the invocation's `SampleId` as the layer.
/// The texel of storage-form attachment `color` at the invocation's pixel (at its sample as
/// `samples` holds them), read inside the interlock.
fn read_texel(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    samples: StorageSamples,
) -> BResult<SValue> {
    let (image, at, ops) = image_and_texel(cx, fb, color, samples)?;
    let mem = memory_model::coherence(cx.plan).texel(fb)?;
    fb.read(image, at, &ops, mem)
}

fn image_and_texel(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    samples: StorageSamples,
) -> BResult<(SValue, SValue, ImageOperands)> {
    let Some(g) = cx.decl.families.fragment.attachments.get(&color).copied() else {
        return Err(cx.fail(format!("storage attachment {color} is not declared")));
    };
    let p = fb.module().global_value(g)?;
    let image = fb.load(p, MemAccess::default())?;
    let pixel = storage_texel(cx, fb)?;
    if samples != StorageSamples::Planes {
        let ops = operands(cx, fb)?;
        return Ok((image, pixel, ops));
    }
    let Some(layer) = sample(cx, fb)? else {
        return Err(cx.fail("a planes access without its SampleId"));
    };
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let uvec3 = fb.module().ty(SType::Vector { elem: u32_, len: 3 })?;
    let x = fb.composite_extract(pixel, &[0])?;
    let y = fb.composite_extract(pixel, &[1])?;
    let texel = fb.composite_construct(uvec3, &[x, y, layer])?;
    Ok((image, texel, ImageOperands::default()))
}

/// The image operands of a fetched color's read or write in a multisampled image (an input
/// attachment, or a multisampled storage image): the invocation's `SampleId` for a per-sample
/// function (`FragmentPlan::fetch_sample`), none for one sample.
pub(super) fn operands(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
) -> BResult<ImageOperands> {
    let Some(sample) = sample(cx, fb)? else {
        return Ok(ImageOperands::default());
    };
    Ok(ImageOperands {
        sample: Some(sample),
        ..ImageOperands::default()
    })
}

/// The invocation's `SampleId`, loaded, for a per-sample function; `None` for one sample.
pub(super) fn sample(cx: &mut EmitCx<'_>, fb: &mut FunctionBuilder<'_>) -> BResult<Option<SValue>> {
    let Some(object) = cx.plan.families.fragment.fetch_sample else {
        return Ok(None);
    };
    let g = stage_io::object_global(cx, object)?;
    let p = fb.module().global_value(g)?;
    fb.load(p, MemAccess::default()).map(Some)
}

/// The invocation's pixel, a `uvec2` from the integer part of `FragCoord.xy` (the pixel's
/// center, single-sampled): the texel of a storage image the interlocked forms hold per pixel.
pub(super) fn pixel(cx: &mut EmitCx<'_>, fb: &mut FunctionBuilder<'_>) -> BResult<SValue> {
    let Some(coord) = cx.plan.families.fragment.fetch_coord else {
        return Err(cx.fail("a storage-form access without its FragCoord"));
    };
    let frag = stage_io::object_global(cx, coord)?;
    let frag = fb.module().global_value(frag)?;
    let frag = fb.load(frag, MemAccess::default())?;
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let uvec2 = fb.module().ty(SType::Vector { elem: u32_, len: 2 })?;
    let fx = fb.composite_extract(frag, &[0])?;
    let fy = fb.composite_extract(frag, &[1])?;
    let x = fb.convert(ConvOp::ConvertFToU, fx, u32_)?;
    let y = fb.convert(ConvOp::ConvertFToU, fy, u32_)?;
    fb.composite_construct(uvec2, &[x, y])
}

/// The texel of a storage-form image at the invocation's pixel: the [`pixel`], and under the
/// layered storage form (`FragmentPlan::fetch_layer`) the layer its primitive renders as well.
pub(super) fn storage_texel(cx: &mut EmitCx<'_>, fb: &mut FunctionBuilder<'_>) -> BResult<SValue> {
    let pixel = pixel(cx, fb)?;
    let Some(object) = cx.plan.families.fragment.fetch_layer else {
        return Ok(pixel);
    };
    let g = stage_io::object_global(cx, object)?;
    let p = fb.module().global_value(g)?;
    let layer = fb.load(p, MemAccess::default())?;
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let uvec3 = fb.module().ty(SType::Vector { elem: u32_, len: 3 })?;
    let x = fb.composite_extract(pixel, &[0])?;
    let y = fb.composite_extract(pixel, &[1])?;
    fb.composite_construct(uvec3, &[x, y, layer])
}

/// A `uvec4` of a signed-integer texel's bits, or the texel itself.
fn as_held(fb: &mut FunctionBuilder<'_>, texel: SValue, numeric: SampledType) -> BResult<SValue> {
    if numeric != SampledType::Sint {
        return Ok(texel);
    }
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let uvec4 = fb.module().ty(SType::Vector { elem: u32_, len: 4 })?;
    fb.convert(ConvOp::Bitcast, texel, uvec4)
}

/// Read once, in the function's first block, each storage-form color input the function decodes
/// itself (an sRGB or packed format's word, through its integer view) and uses more than once. The
/// body runs inside the interlock, where no other fragment of the pixel writes the texel and the
/// invocation's own write comes at its return, so every use reads the same value: Hillshade's
/// `Blending::blend` reads `dst` and decodes its sRGB bytes in each of 19 modes. A color the image
/// unit converts is read at its uses: a repeated read there is one surface load, which the stage
/// compile merges within a block, and holding it from the first block lengthens its live range
/// over paths that never read it (RenderBox's gradients: 32 registers to 40).
pub(super) fn emit_entry(cx: &mut EmitCx<'_>, fb: &mut FunctionBuilder<'_>) -> BResult<()> {
    let fragment = &cx.plan.families.fragment;
    let uses = |param: u32| {
        let mut n = 0usize;
        for inst in cx.body.insts.values() {
            inst.kind.for_each_value(|v| n += usize::from(v == Value::Arg(ArgIdx::new(param))));
        }
        n
    };
    let held: Vec<ColorInput> = fragment
        .colors
        .iter()
        .filter(|c| {
            fragment
                .fetch
                .get(&c.color)
                .is_some_and(|sc| sc.view.uint_view.is_some() && uses(c.param) > 1)
        })
        .copied()
        .collect();
    for c in held {
        let Some(v) = cx.plan.values.id(Value::Arg(ArgIdx::new(c.param))) else {
            return Err(cx.fail(format!("color input parameter {} is out of range", c.param)));
        };
        let x = value(cx, fb, c)?;
        cx.hold(v, x);
    }
    Ok(())
}

/// The value of storage-form color input `c` in its lowered representation, read at its pixel
/// (as `color::value` reads an input attachment).
pub(super) fn value(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    c: ColorInput,
) -> BResult<SValue> {
    let Some(sc) = cx.plan.families.fragment.fetch.get(&c.color).copied() else {
        return Err(cx.fail(format!("attachment {} is not in the storage form", c.color)));
    };
    let texel = read_texel(cx, fb, c.color, sc.view.samples)?;
    let texel = match sc.view.uint_view {
        Some(_) if packed::is_packed(sc.view.format) => {
            let word = fb.composite_extract(texel, &[0])?;
            packed::decode(cx, fb, sc.view.format, word, Some(c.form))?
        }
        Some(_) => {
            let Some(table) = cx.decl.families.arith.srgb else {
                return Err(cx.fail("an sRGB storage read without Metal's table"));
            };
            let word = fb.composite_extract(texel, &[0])?;
            srgb_decode(fb, sc.view.format, word, table)?
        }
        None => as_held(fb, texel, sc.view.numeric)?,
    };
    let x = match c.form.lanes {
        4 => texel,
        1 => fb.composite_extract(texel, &[0])?,
        n => {
            let lanes: Vec<u32> = (0..u32::from(n)).collect();
            fb.vector_shuffle(texel, texel, &lanes)?
        }
    };
    let x = ops::mask(fb, c.form, x)?;
    narrow::round(fb, c.form, x)
}

/// Write each written storage-form attachment's output of returned value `v` (in the entry's
/// returned shape) at its pixel, unless a discard was recorded (`discarded`).
pub(super) fn emit_return(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    v: Option<crate::air::ir::ValueId>,
) -> BResult<()> {
    let writes: Vec<(u32, StorageColor, u32, Form)> = cx
        .plan
        .families
        .fragment
        .fetch
        .iter()
        .filter(|(_, sc)| !sc.view.stores_nothing())
        .filter_map(|(&color, sc)| sc.output.map(|(field, form)| (color, *sc, field, form)))
        .collect();
    if writes.is_empty() {
        return Ok(());
    }
    let Some(v) = v else {
        return Err(cx.fail("a storage-form output without a returned value"));
    };
    let x = cx.operand(fb, v)?;
    let mut texels = Vec::with_capacity(writes.len());
    for (color, sc, field, form) in writes {
        let part = match cx.plan.families.stage_io.returned {
            Returned::Struct => fb.composite_extract(x, &[field])?,
            Returned::Value => x,
            Returned::Void => return Err(cx.fail("a storage-form output of a void entry")),
        };
        // The second (index-1) output, the blend's second source where it reads one.
        let second = match (sc.second, cx.plan.families.stage_io.returned) {
            (Some((field, form)), Returned::Struct) => {
                Some((form, fb.composite_extract(x, &[field])?))
            }
            (Some(_), _) => return Err(cx.fail("a second output outside a returned structure")),
            (None, _) => None,
        };
        let texel = match sc.view.uint_view {
            Some(_) => {
                let (form, part) = match sc.view.rop {
                    true => rop_blended(cx, fb, color, &sc, (form, part), second)?,
                    false => (form, part),
                };
                if packed::is_packed(sc.view.format) {
                    packed::texel(cx, fb, color, &sc, form, part)?
                } else {
                    srgb_texel(cx, fb, color, &sc, form, part)?
                }
            }
            None => {
                // Metal's 16-bit float blend of a `half4` output, or the device's blend by its
                // model, over the texel.
                let (form, part) = match (sc.view.blend, sc.view.rop) {
                    (Some(_), true) => rop_blended(cx, fb, color, &sc, (form, part), second)?,
                    (Some(eq), false) => (form, half_blended(cx, fb, color, &sc, eq, part)?),
                    (None, _) => (form, part),
                };
                let part = if sc.view.merges() {
                    merge(cx, fb, color, &sc, form, part)?
                } else {
                    part
                };
                // The device's blend stores through the image's conversion, as its blender's.
                let part = match sc.view.rop {
                    true => part,
                    false => stage_io::color_write::round(cx, fb, color, form, part)?,
                };
                texel_of(fb, part, form, sc.view.numeric)?
            }
        };
        let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
        texels.push((image, at, texel, ops));
    }
    let mem = memory_model::coherence(cx.plan).texel(fb)?;
    let write = move |fb: &mut FunctionBuilder<'_>, texels: &mut Vec<Write>| {
        for (image, at, texel, ops) in texels.iter() {
            fb.write(*image, *at, *texel, ops, mem)?;
        }
        Ok(())
    };
    // The guard: not discarded, and, for a function that returns a sample mask, the mask's bit of
    // the invocation's own sample (bit 0 at one sample).
    let discarded = if cx.plan.families.fragment.records_discards() {
        let Some(g) = cx.decl.families.fragment.discarded else {
            return Err(cx.fail("a recorded discard without its predicate"));
        };
        let p = fb.module().global_value(g)?;
        let discarded = fb.load(p, MemAccess::default())?;
        Some(fb.unary(UnOp::LogicalNot, discarded)?)
    } else {
        None
    };
    let masked = match cx.plan.families.fragment.fetch_mask {
        Some(field) => Some(sample_bit(cx, fb, x, field)?),
        None => None,
    };
    let kept = match (discarded, masked) {
        (None, None) => return write(fb, &mut texels),
        (Some(k), None) | (None, Some(k)) => k,
        (Some(a), Some(b)) => fb.binary(BinOp::LogicalAnd, a, b)?,
    };
    let arm: ArmFn<'_, Vec<Write>> = Box::new(move |fb, texels| {
        write(fb, texels)?;
        Ok(ArmEnd::FallThrough(Vec::new()))
    });
    fb.if_(&mut texels, kept, arm, None)?;
    Ok(())
}

/// One storage write: the image, the texel's coordinate, the texel, and the image operands.
type Write = (SValue, SValue, SValue, ImageOperands);

/// The `half4` output `part` (four `f32`s holding its `half`s) of a module without fast math
/// blended into 16-bit float attachment `color` by Metal's blend `eq`
/// (`extended_range::Blend::half_float`): the texel read first at the invocation's pixel (and
/// sample), inside the interlock, as [`merge`] reads it, and Metal's `half` arithmetic of the
/// output over it (`extended_range::blend::blend_half_float`; the blend color from
/// `PushField::HalfBlendColor` where the blend reads it). The channels a write mask keeps are
/// [`merge`]'s to take back from the texel.
fn half_blended(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    eq: BlendEquation,
    part: SValue,
) -> BResult<SValue> {
    let Ok(b) = extended_range::Blend::half_float(&eq, written(sc.view.kept)) else {
        return Err(cx.fail("a 16-bit float blend outside Metal's model"));
    };
    let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
    let mem = memory_model::coherence(cx.plan).texel(fb)?;
    let texel = fb.read(image, at, &ops, mem)?;
    let d = as_held(fb, texel, sc.view.numeric)?;
    let constant = match b.reads_constant() {
        true => {
            let field = cx.push_field(fb, PushField::HalfBlendColor)?;
            Some(fb.composite_extract(field, &[0])?)
        }
        false => None,
    };
    extended_range::blend::blend_half_float(fb, &b, (part, d), constant)
}

/// Whether the function writes color attachment `color` through the storage form under Metal's
/// 16-bit float blend of a `half4` output, which the shader computes.
pub(super) fn blends_half(cx: &SelectCx<'_>, color: u32) -> bool {
    cx.families
        .fragment
        .fetch
        .get(&color)
        .is_some_and(|sc| sc.view.blend.is_some() && !sc.view.rop)
}

/// Output `part` (of `form`, in its lowered representation) of storage-form attachment `color`
/// whose write keeps some channels (`StorageView::kept`): each kept channel's lane taken from the
/// texel the write overwrites, read first at the invocation's pixel (and sample), as `value` reads
/// it. Inside the interlock no other fragment of the pixel writes the texel between the read and
/// the write, so the write stores the kept channels as they were (in the device's decode of the
/// texel: a `half` NaN's bits are the device's, `PrecisionNote::DeviceNanEncoding`).
fn merge(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    form: Form,
    part: SValue,
) -> BResult<SValue> {
    let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
    let mem = memory_model::coherence(cx.plan).texel(fb)?;
    let texel = fb.read(image, at, &ops, mem)?;
    let texel = as_held(fb, texel, sc.view.numeric)?;
    let n = u32::from(form.lanes);
    if n < 2 {
        return Err(cx.fail("a merged storage write of one lane"));
    }
    let channels = [
        vk::ColorComponentFlags::R,
        vk::ColorComponentFlags::G,
        vk::ColorComponentFlags::B,
        vk::ColorComponentFlags::A,
    ];
    // The kept channels the draw's word names: bit `c` of the color's four takes channel `c`
    // from the texel.
    if sc.view.kept_at_draw {
        let word = cx.push_field(fb, PushField::StorageKeptChannels)?;
        let shift = fb.u32_const(4 * color)?;
        let shift = fb.module().const_value(shift)?;
        let bits = fb.binary(BinOp::ShiftRightLogical, word, shift)?;
        let zero = fb.u32_const(0)?;
        let zero = fb.module().const_value(zero)?;
        let mut keeps = Vec::with_capacity(n as usize);
        for lane in 0..n {
            let bit = fb.u32_const(1 << lane)?;
            let bit = fb.module().const_value(bit)?;
            let set = fb.binary(BinOp::BitwiseAnd, bits, bit)?;
            keeps.push(fb.binary(BinOp::INotEqual, set, zero)?);
        }
        let bvec = fb.bool_ty(form.lanes)?;
        let keep = fb.composite_construct(bvec, &keeps)?;
        // The texel's lanes the output has.
        let lanes: Vec<u32> = (0..n).collect();
        let texel = fb.vector_shuffle(texel, texel, &lanes)?;
        return fb.select(keep, texel, part);
    }
    let lanes: Vec<u32> = (0..n)
        .zip(channels)
        .map(|(lane, c)| {
            if sc.view.kept.contains(c) {
                n + lane
            } else {
                lane
            }
        })
        .collect();
    fb.vector_shuffle(part, texel, &lanes)
}

/// The bytes of a texel of the sRGB format `format` held in one `u32` (`word`, the texel's bytes
/// in memory order), in red, green, blue, alpha order, as `math::pack` and `math::unpack` hold
/// `unorm4x8_srgb`'s: `B8G8R8A8_SRGB`'s blue and red bytes swapped, `R8G8B8A8_SRGB`'s as they
/// are. The swap is its own inverse.
fn rgba_order(fb: &mut FunctionBuilder<'_>, format: vk::Format, word: SValue) -> BResult<SValue> {
    if format != vk::Format::B8G8R8A8_SRGB {
        return Ok(word);
    }
    let k = |fb: &mut FunctionBuilder<'_>, v: u32| -> BResult<SValue> {
        let c = fb.u32_const(v)?;
        fb.module().const_value(c)
    };
    let (ga, low, sixteen) = (k(fb, 0xff00_ff00)?, k(fb, 0xff)?, k(fb, 16)?);
    let kept = fb.binary(BinOp::BitwiseAnd, word, ga)?;
    let down = fb.binary(BinOp::ShiftRightLogical, word, sixteen)?;
    let down = fb.binary(BinOp::BitwiseAnd, down, low)?;
    let up = fb.binary(BinOp::BitwiseAnd, word, low)?;
    let up = fb.binary(BinOp::ShiftLeftLogical, up, sixteen)?;
    let swapped = fb.binary(BinOp::BitwiseOr, kept, down)?;
    fb.binary(BinOp::BitwiseOr, swapped, up)
}

/// The `vec4` a fetch reads from the sRGB texel `word` of `format`: Metal's decode
/// (`math::unpack` of `unorm4x8_srgb` through the module's table `table`): each color channel's
/// twelve-bit step over 4095 and the alpha byte over 255, correctly rounded (EV XRR2-srgb1-m4:
/// every byte, and every texel an earlier draw of the pass wrote).
fn srgb_decode(
    fb: &mut FunctionBuilder<'_>,
    format: vk::Format,
    word: SValue,
    table: GlobalId,
) -> BResult<SValue> {
    let rgba = rgba_order(fb, format, word)?;
    let f4 = Form {
        held: Held::Float(crate::lower::arith::FloatFmt::F32),
        lanes: 4,
    };
    math::unpack(fb, PackFormat::Unorm4x8Srgb, f4, rgba, Some(table))
}

/// The `uvec4` texel (its first lane the texel's word, in `R32_UINT`) a write of output `part`
/// (four `float` or `half` lanes of `form`) to the sRGB attachment `color` stores: Metal's encode
/// (`math::pack` of `unorm4x8_srgb` through the module's table; a NaN 0, the value clamped to `[0,
/// 1]`, the exact product with 4095 rounded to nearest even through Metal's thresholds, alpha's
/// with 255; EV XRR2-srgb1-m4 checked every threshold, 4, 194, 304 floats and every half), its
/// bytes in `format`'s order; a write that keeps channels takes their bytes from the texel read
/// first at the invocation's pixel (and sample), inside the interlock, so they stay as they were,
/// bit for bit.
fn srgb_texel(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    form: Form,
    part: SValue,
) -> BResult<SValue> {
    let Some(table) = cx.decl.families.arith.srgb else {
        return Err(cx.fail("an sRGB storage write without Metal's table"));
    };
    if form.lanes != 4
        || form
            .float_fmt()
            .map(crate::lower::arith::FloatFmt::container)
            != Some(32)
    {
        return Err(cx.fail("an sRGB storage write of another output than four float lanes"));
    }
    let f4 = form.with(Held::Float(crate::lower::arith::FloatFmt::F32));
    let test = crate::lower::arith::nan::NanTest::for_modes(&cx.plan.families.arith.modes);
    let rgba = math::pack(fb, PackFormat::Unorm4x8Srgb, f4, part, Some(table), test)?;
    let mut word = rgba_order(fb, sc.view.format, rgba)?;
    if sc.view.merges() {
        // The kept channels' bytes, in memory order.
        let bytes = match sc.view.format {
            vk::Format::B8G8R8A8_SRGB => [16u32, 8, 0, 24],
            _ => [0u32, 8, 16, 24],
        };
        let channels = [
            vk::ColorComponentFlags::R,
            vk::ColorComponentFlags::G,
            vk::ColorComponentFlags::B,
            vk::ColorComponentFlags::A,
        ];
        let mask = channels
            .iter()
            .zip(bytes)
            .filter(|(c, _)| sc.view.kept.contains(**c))
            .fold(0u32, |m, (_, at)| m | 0xff << at);
        let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
        let mem = memory_model::coherence(cx.plan).texel(fb)?;
        let old = fb.read(image, at, &ops, mem)?;
        let old = fb.composite_extract(old, &[0])?;
        let keep = fb.u32_const(mask)?;
        let keep = fb.module().const_value(keep)?;
        let write = fb.u32_const(!mask)?;
        let write = fb.module().const_value(write)?;
        let old = fb.binary(BinOp::BitwiseAnd, old, keep)?;
        let new = fb.binary(BinOp::BitwiseAnd, word, write)?;
        word = fb.binary(BinOp::BitwiseOr, old, new)?;
    }
    let u32_ = fb.module().ty(SType::Int {
        width: 32,
        signed: false,
    })?;
    let uvec4 = fb.module().ty(SType::Vector { elem: u32_, len: 4 })?;
    let zero = fb.u32_const(0)?;
    let zero = fb.module().const_value(zero)?;
    fb.composite_construct(uvec4, &[word, zero, zero, zero])
}

/// The blend math of the function `cx` selects ([`extended_range::blend::BlendMath`]).
pub(super) fn blend_math(cx: &SelectCx<'_>) -> extended_range::blend::BlendMath {
    extended_range::blend::BlendMath::of(cx.entry.compile.fast_math)
}

/// The channels a write that keeps `kept` stores.
pub(super) fn written(kept: vk::ColorComponentFlags) -> ColorWriteMask {
    [
        (vk::ColorComponentFlags::R, ColorWriteMask::RED),
        (vk::ColorComponentFlags::G, ColorWriteMask::GREEN),
        (vk::ColorComponentFlags::B, ColorWriteMask::BLUE),
        (vk::ColorComponentFlags::A, ColorWriteMask::ALPHA),
    ]
    .into_iter()
    .filter(|(k, _)| !kept.contains(*k))
    .fold(ColorWriteMask::NONE, |m, (_, c)| m.union(c))
}

/// Whether returned value `x`'s `[[sample_mask]]` output, in field `field`, sets the bit of the
/// invocation's own sample: `((mask >> SampleId) & 1) != 0`. Under sample shading an invocation
/// shades its one sample, and Metal keeps its write exactly when the mask sets that sample's bit.
/// At one sample, where no `SampleId` is planned, bit 0 (Metal stores no color without it, EV
/// DSX-mask1-m4).
fn sample_bit(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    x: SValue,
    field: u32,
) -> BResult<SValue> {
    let mask = match cx.plan.families.stage_io.returned {
        Returned::Struct => fb.composite_extract(x, &[field])?,
        Returned::Value | Returned::Void => {
            return Err(cx.fail("a returned sample mask outside a returned struct"))
        }
    };
    let sample = match sample(cx, fb)? {
        Some(sample) => sample,
        None => {
            let zero = fb.u32_const(0)?;
            fb.module().const_value(zero)?
        }
    };
    let shifted = fb.binary(BinOp::ShiftRightLogical, mask, sample)?;
    let one = fb.u32_const(1)?;
    let one = fb.module().const_value(one)?;
    let bit = fb.binary(BinOp::BitwiseAnd, shifted, one)?;
    let zero = fb.u32_const(0)?;
    let zero = fb.module().const_value(zero)?;
    fb.binary(BinOp::INotEqual, bit, zero)
}

/// The `vec4` texel of the attachment's sampled type that writes output `x` of `form`: its lanes
/// then zeros (lanes past the format's channels are ignored), a signed-integer one's bits as
/// `int`.
fn texel_of(
    fb: &mut FunctionBuilder<'_>,
    x: SValue,
    form: Form,
    numeric: SampledType,
) -> BResult<SValue> {
    let elem = scalar_type(
        fb.module(),
        if form.is_float() {
            SampledType::Float
        } else {
            SampledType::Uint
        },
    )?;
    let vec4 = fb.module().ty(SType::Vector { elem, len: 4 })?;
    let texel = if form.lanes == 4 {
        x
    } else {
        let zero = ops::splat(
            fb,
            Form {
                held: form.held,
                lanes: 1,
            },
            0,
        )?;
        let mut parts = Vec::with_capacity(4);
        for lane in 0..4u32 {
            parts.push(match lane {
                0 if form.lanes == 1 => x,
                l if l < u32::from(form.lanes) => fb.composite_extract(x, &[l])?,
                _ => zero,
            });
        }
        fb.composite_construct(vec4, &parts)?
    };
    if numeric != SampledType::Sint {
        return Ok(texel);
    }
    let i32_ = scalar_type(fb.module(), SampledType::Sint)?;
    let ivec4 = fb.module().ty(SType::Vector { elem: i32_, len: 4 })?;
    fb.convert(ConvOp::Bitcast, texel, ivec4)
}

mod packed;
mod rop;

use rop::{rop_blended, rop_of};

/// The device's blend `view`'s write computes by its model, where it does.
pub(crate) fn modelled_blend(view: &StorageView) -> Option<super::rop_blend::RopBlend> {
    rop_of(view)
}

#[cfg(test)]
mod tests;
