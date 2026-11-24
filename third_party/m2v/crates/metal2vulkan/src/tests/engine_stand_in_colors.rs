//! Color attachments held in an `RGBA16Float` stand-in of one of Apple's extended-range formats
//! (`ColorAttachmentDescriptor::stand_in`, `LoweringInputs::stand_in_colors`): the fragment
//! function's float output there carries Metal's clamp before its conversion, two `GLSL.std.450`
//! `NClamp`s, its color lanes to the form's bounds (a blended output to Metal's blend source
//! range, an unblended one to that range's low end and code 1023's value) and its alpha to
//! `[0, 1]`, and its half-float channels round to nearest even, as Metal's tile of those formats
//! holds a float output. An attachment of one of those formats without a stand-in is refused.
//!
//! The owned fixture `engine/stage_io`'s render pipeline, its color 0 `RGBA16Float`, goes through
//! `Device::compile_render_pipeline` on every committed profile. No GPU is used. `llvm-as`
//! assembles the fixture; a missing tool skips visibly.

use crate::budget::Budget;
use crate::device::{Device, DeviceDescription, DevicePolicy};
use crate::error::{Attachment, Error, Feature, Needs};
use crate::library::Library;
use crate::metal::format::ExtendedRange;
use crate::metal::{BlendFactor, PixelFormat, VertexFormat};
use crate::pipeline::descriptor::{
    ColorAttachmentDescriptor, VertexAttributeDescriptor, VertexBufferLayoutDescriptor,
    VertexDescriptor,
};
use crate::pipeline::testing::{engine_dir, fixtures_dir, llvm_assemble, AssembleError};
use crate::pipeline::{FunctionDescriptor, RenderPipeline, RenderPipelineDescriptor, StageSource};
use crate::vk;

const NCLAMP: u32 = 81;

/// Every committed profile as a device under the default policy, by stem.
fn devices() -> Vec<(String, Device)> {
    let dir = fixtures_dir().join("../../profiles");
    let mut out = Vec::new();
    for entry in std::fs::read_dir(&dir).unwrap_or_else(|e| panic!("{}: {e}", dir.display())) {
        let path = entry.expect("profile directory entry").path();
        if path.extension().is_none_or(|x| x != "json") {
            continue;
        }
        let stem = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let text = std::fs::read_to_string(&path).expect("read profile");
        let desc = DeviceDescription::from_khronos_json(&text, None).expect("profile parses");
        let dev = Device::new(desc, DevicePolicy::default()).expect("device derives");
        out.push((stem, dev));
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    out
}

fn fixture() -> Option<Vec<u8>> {
    match llvm_assemble(&engine_dir().join("stage_io.ll")) {
        Ok(b) => Some(b),
        Err(AssembleError::Missing(why)) => {
            eprintln!("SKIPPED: {why}");
            None
        }
        Err(AssembleError::Rejected(why)) => panic!("llvm-as rejected stage_io: {why}"),
    }
}

/// `stage_io`'s vertex and fragment functions, color 0 `color` (its color 2 has no attachment).
fn descriptor(bc: &[u8], color: ColorAttachmentDescriptor) -> RenderPipelineDescriptor {
    let lib = Library::from_air(bc).expect("library");
    let v = lib.function("stage_io_vertex").expect("vertex");
    let f = lib.function("stage_io_fragment").expect("fragment");
    let mut layout = VertexDescriptor::new();
    layout
        .layouts
        .insert(0, VertexBufferLayoutDescriptor::new(Some(24)));
    for (index, format, offset) in [
        (0, VertexFormat::Float3, 0),
        (1, VertexFormat::Half2, 12),
        (4, VertexFormat::UShort2, 16),
        (7, VertexFormat::UChar4, 20),
    ] {
        layout
            .attributes
            .insert(index, VertexAttributeDescriptor::new(format, offset, 0));
    }
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(v)));
    d.fragment = Some(StageSource::Function(FunctionDescriptor::new(f)));
    d.vertex_descriptor = Some(layout);
    d.color_attachments[0] = Some(color);
    d.depth_format = Some(PixelFormat::Depth32Float);
    d
}

/// Color 0 in `RGBA16Float`, the stand-in of `stand_in`, blended source-over where `blended`.
fn attachment(stand_in: Option<ExtendedRange>, blended: bool) -> ColorAttachmentDescriptor {
    let mut c = ColorAttachmentDescriptor::new(PixelFormat::RGBA16Float);
    c.stand_in = stand_in;
    if blended {
        c.blending_enabled = true;
        c.source_rgb_blend_factor = BlendFactor::One;
        c.source_alpha_blend_factor = BlendFactor::One;
        c.destination_rgb_blend_factor = BlendFactor::OneMinusSourceAlpha;
        c.destination_alpha_blend_factor = BlendFactor::OneMinusSourceAlpha;
    }
    c
}

/// The fragment module's `GLSL.std.450` `NClamp`s, and the 32-bit values of its `OpConstant`s.
fn clamps(pipeline: &RenderPipeline) -> (usize, Vec<u32>) {
    let (mut nclamps, mut constants) = (0, Vec::new());
    for s in pipeline
        .stages
        .iter()
        .filter(|s| s.stage == vk::ShaderStage::FRAGMENT)
    {
        let words = &s.spirv;
        let mut at = 5;
        while at < words.len() {
            let count = (words[at] >> 16) as usize;
            if count == 0 {
                break;
            }
            let operands = &words[at + 1..at + count];
            match words[at] & 0xffff {
                op if op == crate::spv::Op::ExtInst as u32 => {
                    nclamps += usize::from(operands.get(3) == Some(&NCLAMP));
                }
                op if op == crate::spv::Op::Constant as u32 && operands.len() == 3 => {
                    constants.push(operands[2]);
                }
                _ => {}
            }
            at += count;
        }
    }
    (nclamps, constants)
}

#[test]
fn a_stand_in_output_carries_metals_clamp() {
    let Some(bc) = fixture() else {
        return;
    };
    let cases = [
        (ExtendedRange::Bgra10 { srgb: false }, true, 1281.0 / 1024.0),
        (
            ExtendedRange::Bgra10 { srgb: false },
            false,
            1283.0 / 1024.0,
        ),
        (ExtendedRange::Bgr10 { srgb: false }, true, 1281.0 / 1024.0),
        (
            ExtendedRange::Planes { srgb: false },
            false,
            1283.0 / 1024.0,
        ),
        (ExtendedRange::Bgra10 { srgb: true }, true, 1709.0 / 1024.0),
        (ExtendedRange::Bgr10 { srgb: true }, true, 1281.0 / 1024.0),
        (ExtendedRange::Bgr10 { srgb: true }, false, 1715.0 / 1024.0),
    ];
    let mut checked = 0;
    for (stem, dev) in devices() {
        let compile = |stand_in, blended| {
            let mut d = descriptor(&bc, attachment(stand_in, blended));
            // An unblended stand-in rounds its halves as a plain attachment does under mode 2.
            if stand_in.is_none() && !blended {
                d.texture_write_rounding_mode = crate::metal::TextureWriteRoundingMode::NearestEven;
            }
            dev.compile_render_pipeline(&d, &Budget::standard())
        };
        let plain = match (compile(None, false), compile(None, true)) {
            (Ok(unblended), Ok(blended)) => [clamps(&unblended), clamps(&blended)],
            (Err(Error::Unsupported(u)), _) | (_, Err(Error::Unsupported(u)))
                if u.needs != Needs::NotImplemented =>
            {
                continue
            }
            (Err(e), _) | (_, Err(e)) => panic!("{stem}: {e}"),
        };
        checked += 1;
        for (form, blended, high) in cases {
            let what = format!("{stem}: {form:?} blended {blended}");
            let p = compile(Some(form), blended).unwrap_or_else(|e| panic!("{what}: {e}"));
            let (nclamps, constants) = clamps(&p);
            let (before, before_constants) = &plain[usize::from(blended)];
            assert_eq!(nclamps, before + 2, "{what}: a color and an alpha clamp");
            let high = f32::to_bits(high);
            assert!(constants.contains(&high), "{what}: the high bound");
            assert!(
                !before_constants.contains(&high),
                "{what}: no bound without a stand-in"
            );
        }
    }
    assert!(checked > 0, "no committed profile compiled the fixture");
}

/// An unblended stand-in's half-float channels round to nearest even ([`HALF_NEAREST`]) under
/// either `textureWriteRoundingMode` on every committed profile, and a plain `RGBA16Float`
/// attachment's under Metal's default do not: Metal's extended-range tile holds the half nearest a
/// float output, where its `RGBA16Float` attachment stores the half toward zero. A blended
/// stand-in keeps a blended plain attachment's writes.
#[test]
fn an_unblended_stand_ins_halves_round_to_nearest_even() {
    use crate::metal::TextureWriteRoundingMode;
    use crate::pipeline::color_writes::{exact_writes, HALF_NEAREST};
    let mut checked = 0;
    for (stem, dev) in devices() {
        let p = dev.profile();
        let writes = |stand_in, blended| {
            exact_writes(
                [0],
                &[Some(attachment(stand_in, blended))],
                (false, TextureWriteRoundingMode::TowardZero),
                p,
            )
        };
        let plain = writes(None, false);
        assert!(
            plain
                .get(&0)
                .is_none_or(|r| !r.contains(&Some(HALF_NEAREST))),
            "{stem}: plain {plain:?}"
        );
        for form in [
            ExtendedRange::Planes { srgb: false },
            ExtendedRange::Planes { srgb: true },
            ExtendedRange::Bgra10 { srgb: false },
            ExtendedRange::Bgra10 { srgb: true },
            ExtendedRange::Bgr10 { srgb: false },
            ExtendedRange::Bgr10 { srgb: true },
        ] {
            assert_eq!(
                writes(Some(form), true),
                writes(None, true),
                "{stem}: {form:?} blended"
            );
            for mode in [
                TextureWriteRoundingMode::TowardZero,
                TextureWriteRoundingMode::NearestEven,
            ] {
                let rules = exact_writes(
                    [0],
                    &[Some(attachment(Some(form), false))],
                    (false, mode),
                    p,
                );
                assert_eq!(
                    rules.get(&0),
                    Some(&[Some(HALF_NEAREST); 4]),
                    "{stem}: {form:?} under {mode:?}"
                );
            }
        }
        checked += 1;
    }
    assert!(checked > 0, "no committed profile");
}

/// A color attachment of one of Apple's 10-bit extended-range formats without a stand-in, blended
/// or not, is refused `NotImplemented(Format)` at that attachment on every committed profile.
#[test]
fn an_extended_range_attachment_without_a_stand_in_is_refused() {
    let Some(bc) = fixture() else {
        return;
    };
    let formats = [
        PixelFormat::BGR10XR,
        PixelFormat::BGR10XRSRGB,
        PixelFormat::BGRA10XR,
        PixelFormat::BGRA10XRSRGB,
        PixelFormat::RGB10A82PXR10,
        PixelFormat::RGB10A82PXR10SRGB,
    ];
    let mut checked = 0;
    for (stem, dev) in devices() {
        checked += 1;
        for pf in formats {
            for blended in [false, true] {
                let what = format!("{stem}: {pf:?} blended {blended}");
                let mut c = attachment(None, blended);
                c.pixel_format = pf;
                match dev.compile_render_pipeline(&descriptor(&bc, c), &Budget::standard()) {
                    Err(Error::Unsupported(u)) => {
                        assert_eq!(
                            (u.feature, u.needs),
                            (Feature::Format, Needs::NotImplemented),
                            "{what}"
                        );
                        let at = u.location.as_ref().and_then(|l| l.attachment());
                        assert_eq!(at, Some(Attachment::Color(0)), "{what}");
                    }
                    other => panic!("{what}: {:?}", other.map(|_| ())),
                }
            }
        }
    }
    assert!(checked > 0, "no committed profile");
}
