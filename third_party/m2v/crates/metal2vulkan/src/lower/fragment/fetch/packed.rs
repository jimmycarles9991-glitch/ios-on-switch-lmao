//! The storage form of a packed unsigned normalized attachment a device grants no storage access
//! (NVK): `BGR5A1Unorm` (`A1R5G5B5_UNORM_PACK16`) and `B5G6R5Unorm` (`R5G6B5_UNORM_PACK16`), viewed
//! in `R16_UINT`, and `BGR10A2Unorm` (`A2R10G10B10_UNORM_PACK32`), viewed in `R32_UINT`
//! (`StorageView::uint_view`): the function reads and writes the texel's bits. A read is what
//! Metal's tile holds of a loaded texel: for `BGR10A2Unorm` each channel's exact value,
//! `c / (2^b - 1)` correctly rounded (`metal::texel_format`, `PackedTexels`), and for the 16-bit
//! formats, whose tile holds `half`s, that value rounded to the nearest `half` (an Apple M4 Max's
//! fetch of every 16-bit texel, as `float4` and as `half4`, 262,144 lanes each, for `BGR5A1Unorm`;
//! HW Test's host_gpu_tests PackedFetchReadTests for `B5G6R5Unorm`). `B5G6R5Unorm` has no alpha:
//! it reads 1 and a write leaves no bits of it. A write packs each channel's code as Metal's write of the
//! output rounds it (`color_write::round` by the attachment's rules, else the nearest code), and
//! takes the channels it keeps from the texel read first, inside the interlock.
//!
//! Within a pass Metal's tile keeps a `BGR5A1Unorm` write as the `half` of the output clamped to
//! `[0, 1]` and rounds it to the codes only at the store, so a later draw of the pass that reads it
//! reads that `half` where this form reads the stored code's (the same M4 Max, 61,436 of 61,444
//! lanes): exact only for a texel no earlier draw of the pass wrote.

use super::{image_and_texel, StorageColor};
use crate::lower::arith::color_write;
use crate::lower::arith::ops::fsplat;
use crate::lower::arith::{FloatFmt, Form, Held};
use crate::lower::emit::EmitCx;
use crate::lower::fragment::rop_blend::exact_quotient;
use crate::lower::sync::memory_model;
use crate::pipeline::lower_inputs::ChannelWrite;
use crate::pipeline::sym::PushField;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value as SValue;
use crate::spirv::ir::{BinOp, ConvOp, GlslOp, Type as SType};
use crate::vk;

/// One packed format: each channel's bit offset in the texel and its bits, red to alpha (0 bits:
/// the format has no such channel, which reads 1), and whether Metal's tile holds its channels as
/// `half`s.
struct Packed {
    format: vk::Format,
    fields: [(u32, u8); 4],
    half_tile: bool,
}

const PACKED: [Packed; 3] = [
    Packed {
        format: vk::Format::A1R5G5B5_UNORM_PACK16,
        fields: [(10, 5), (5, 5), (0, 5), (15, 1)],
        half_tile: true,
    },
    Packed {
        format: vk::Format::R5G6B5_UNORM_PACK16,
        fields: [(11, 5), (5, 6), (0, 5), (0, 0)],
        half_tile: true,
    },
    Packed {
        format: vk::Format::A2R10G10B10_UNORM_PACK32,
        fields: [(20, 10), (10, 10), (0, 10), (30, 2)],
        half_tile: false,
    },
];

/// Whether `format` is held in the storage form through this module's packing.
pub(super) fn is_packed(format: vk::Format) -> bool {
    PACKED.iter().any(|p| p.format == format)
}

/// The packing of `format`, one [`is_packed`] holds.
fn packing(format: vk::Format) -> &'static Packed {
    PACKED
        .iter()
        .find(|p| p.format == format)
        .unwrap_or(&PACKED[0])
}

/// Each channel's bit offset and largest code in `format`'s texel, red to alpha (0: no such
/// channel).
fn fields(format: vk::Format) -> [(u32, u32); 4] {
    packing(format)
        .fields
        .map(|(at, bits)| (at, (1u32 << bits) - 1))
}

/// How a write rounds each channel where the pipeline names no rule: to the nearest code.
fn nearest(format: vk::Format) -> [Option<ChannelWrite>; 4] {
    packing(format)
        .fields
        .map(|(_, bits)| (bits > 0).then_some(ChannelWrite::Unorm { bits }))
}

fn u32_const(fb: &mut FunctionBuilder<'_>, v: u32) -> BResult<SValue> {
    let c = fb.u32_const(v)?;
    fb.module().const_value(c)
}

fn vec4(fb: &mut FunctionBuilder<'_>, elem: SType) -> BResult<crate::spirv::ids::TypeId> {
    let e = fb.module().ty(elem)?;
    fb.module().ty(SType::Vector { elem: e, len: 4 })
}

fn u32_type() -> SType {
    SType::Int {
        width: 32,
        signed: false,
    }
}

/// The `vec4` a fetch reads from texel `word` (a `u32` of the view of a `format` attachment), for a
/// reader of form `reader` (`None`: the in-shader blend): each channel's exact value, and in a
/// compile for the native SM53 target the nearest `half` to it where the format's tile holds
/// `half`s and the reader does not round to a `half` itself. The m2v runtime's Vulkan modules keep
/// the exact value.
pub(super) fn decode(
    cx: &EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    format: vk::Format,
    word: SValue,
    reader: Option<Form>,
) -> BResult<SValue> {
    let half_reader = reader.is_some_and(|f| f.float_fmt() == Some(FloatFmt::Half));
    let half_tile = packing(format).half_tile && cx.plan.families.arith.sm53_half && !half_reader;
    channel_values(fb, format, word, half_tile)
}

/// [`decode`]'s value, rounded to the nearest `half` where `half_tile`.
fn channel_values(
    fb: &mut FunctionBuilder<'_>,
    format: vk::Format,
    word: SValue,
    half_tile: bool,
) -> BResult<SValue> {
    let fields = fields(format);
    let mut codes = Vec::with_capacity(4);
    for (at, top) in fields {
        if top == 0 {
            codes.push(u32_const(fb, 1)?);
            continue;
        }
        let shift = u32_const(fb, at)?;
        let mask = u32_const(fb, top)?;
        let field = fb.binary(BinOp::ShiftRightLogical, word, shift)?;
        codes.push(fb.binary(BinOp::BitwiseAnd, field, mask)?);
    }
    let uvec4 = vec4(fb, u32_type())?;
    let codes = fb.composite_construct(uvec4, &codes)?;
    let fvec4 = vec4(fb, SType::Float { width: 32 })?;
    let codes = fb.convert(ConvOp::ConvertUToF, codes, fvec4)?;
    let exact = exact_quotient(fb, codes, fields.map(|(_, top)| top.max(1)))?;
    if !half_tile {
        return Ok(exact);
    }
    // Every nonzero `c / 31` and `c / 63` below 1 is a normal `half` at least 65 `f32` steps from a
    // rounding boundary (tests::every_5_and_6_bit_quotient_is_far_from_a_half_boundary): its nearest
    // `half` is the `f32` with the 13 low fraction bits rounded off to nearest even.
    let bits = fb.convert(ConvOp::Bitcast, exact, uvec4)?;
    let shift = uvec4_splat(fb, 13)?;
    let one = uvec4_splat(fb, 1)?;
    let lsb = fb.binary(BinOp::ShiftRightLogical, bits, shift)?;
    let lsb = fb.binary(BinOp::BitwiseAnd, lsb, one)?;
    let below_half = uvec4_splat(fb, 0x0FFF)?;
    let biased = fb.binary(BinOp::IAdd, bits, below_half)?;
    let biased = fb.binary(BinOp::IAdd, biased, lsb)?;
    let kept = uvec4_splat(fb, !0x1FFF)?;
    let rounded = fb.binary(BinOp::BitwiseAnd, biased, kept)?;
    fb.convert(ConvOp::Bitcast, rounded, fvec4)
}

fn uvec4_splat(fb: &mut FunctionBuilder<'_>, v: u32) -> BResult<SValue> {
    let c = u32_const(fb, v)?;
    let uvec4 = vec4(fb, u32_type())?;
    fb.composite_construct(uvec4, &[c, c, c, c])
}

/// The `uvec4` texel (its first lane the view's `u32`) a write of output `part` (four `float` or
/// `half` lanes of `form`) to attachment `color` stores.
pub(super) fn texel(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    form: Form,
    part: SValue,
) -> BResult<SValue> {
    if form.lanes != 4 || form.float_fmt().is_none() {
        return Err(cx.fail("a packed storage write of another output than four float lanes"));
    }
    let f4 = Form {
        held: Held::Float(FloatFmt::F32),
        lanes: 4,
    };
    let fvec4 = vec4(fb, SType::Float { width: 32 })?;
    let x = match form.float_fmt().map(FloatFmt::container) {
        Some(32) => part,
        _ => fb.convert(ConvOp::FConvert, part, fvec4)?,
    };
    // A blend the shader computes stores as the model's ROP does, to the nearest code
    // (`rop_blend::Assumption::UnormRounding`); an unblended output as Metal writes it.
    let recorded = cx.plan.families.stage_io.interface.color_writes.get(&color);
    let rules = match (sc.view.rop, recorded) {
        (false, Some(rules)) => *rules,
        _ => nearest(sc.view.format),
    };
    let rounded = color_write::round(fb, f4, x, &rules, None)?;
    let mut word = None;
    for (lane, (at, top)) in fields(sc.view.format).into_iter().enumerate() {
        if top == 0 {
            continue;
        }
        let one = Form { lanes: 1, ..f4 };
        let v = fb.composite_extract(rounded, &[lane as u32])?;
        let m = fsplat(fb, one, f64::from(top))?;
        let scaled = fb.binary_no_contraction(BinOp::FMul, v, m)?;
        let code = fb.glsl(GlslOp::RoundEven, &[scaled])?;
        let u = fb.module().ty(u32_type())?;
        let code = fb.convert(ConvOp::ConvertFToU, code, u)?;
        let shift = u32_const(fb, at)?;
        let field = fb.binary(BinOp::ShiftLeftLogical, code, shift)?;
        word = Some(match word {
            Some(w) => fb.binary(BinOp::BitwiseOr, w, field)?,
            None => field,
        });
    }
    let Some(mut word) = word else {
        return Err(cx.fail("a packed storage write without channels"));
    };
    if sc.view.merges() {
        let keep = kept_bits(cx, fb, color, sc)?;
        let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
        let mem = memory_model::coherence(cx.plan).texel(fb)?;
        let old = fb.read(image, at, &ops, mem)?;
        let old = fb.composite_extract(old, &[0])?;
        let old = fb.binary(BinOp::BitwiseAnd, old, keep)?;
        let new = fb.binary(BinOp::BitwiseAnd, word, keep)?;
        let new = fb.binary(BinOp::BitwiseXor, word, new)?;
        word = fb.binary(BinOp::BitwiseOr, old, new)?;
    }
    let uvec4 = vec4(fb, u32_type())?;
    let zero = u32_const(fb, 0)?;
    fb.composite_construct(uvec4, &[word, zero, zero, zero])
}

/// The texel bits of the channels a write keeps: the draw's (`PushField::StorageKeptChannels`, bit
/// `c` of the color's four keeps channel `c`) where the format takes them at draw, else the
/// view's.
fn kept_bits(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
) -> BResult<SValue> {
    let channels = [
        vk::ColorComponentFlags::R,
        vk::ColorComponentFlags::G,
        vk::ColorComponentFlags::B,
        vk::ColorComponentFlags::A,
    ];
    if !sc.view.kept_at_draw {
        let mask = channels
            .iter()
            .zip(fields(sc.view.format))
            .filter(|(c, _)| sc.view.kept.contains(**c))
            .fold(0u32, |m, (_, (at, top))| m | top << at);
        return u32_const(fb, mask);
    }
    let word = cx.push_field(fb, PushField::StorageKeptChannels)?;
    let shift = u32_const(fb, 4 * color)?;
    let draw = fb.binary(BinOp::ShiftRightLogical, word, shift)?;
    let zero = u32_const(fb, 0)?;
    let mut mask = zero;
    for (lane, (at, top)) in fields(sc.view.format).into_iter().enumerate() {
        let bit = u32_const(fb, 1 << lane)?;
        let set = fb.binary(BinOp::BitwiseAnd, draw, bit)?;
        let set = fb.binary(BinOp::INotEqual, set, zero)?;
        let field = u32_const(fb, top << at)?;
        let field = fb.select(set, field, zero)?;
        mask = fb.binary(BinOp::BitwiseOr, mask, field)?;
    }
    Ok(mask)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Each packed format's fields tile its texel exactly, red to alpha as Vulkan packs it.
    #[test]
    fn the_packed_formats_tile_their_texels() {
        for (format, bits, want) in [
            (
                vk::Format::A1R5G5B5_UNORM_PACK16,
                16,
                [(10, 31), (5, 31), (0, 31), (15, 1)],
            ),
            (
                vk::Format::R5G6B5_UNORM_PACK16,
                16,
                [(11, 31), (5, 63), (0, 31), (0, 0)],
            ),
            (
                vk::Format::A2R10G10B10_UNORM_PACK32,
                32,
                [(20, 1023), (10, 1023), (0, 1023), (30, 3)],
            ),
        ] {
            assert!(is_packed(format), "{format:?}");
            let view = if bits == 16 {
                vk::Format::R16_UINT
            } else {
                vk::Format::R32_UINT
            };
            assert_eq!(
                crate::pipeline::StorageAttachmentUse::uint_view_of(format),
                Some(view),
                "{format:?}"
            );
            assert_eq!(fields(format), want, "{format:?}");
            let used = want.iter().fold(0u64, |m, (at, top)| {
                assert_eq!(m & (u64::from(*top) << at), 0, "{format:?} overlaps");
                m | u64::from(*top) << at
            });
            assert_eq!(used, (1u64 << bits) - 1, "{format:?}");
        }
        assert!(!is_packed(vk::Format::A2B10G10R10_UNORM_PACK32));
        assert!(!is_packed(vk::Format::B8G8R8A8_SRGB));
    }

    use crate::pipeline::sym::{ResourceKind, ResourceSlot, Stage};
    use crate::spirv::builder::ModuleBuilder;
    use crate::spirv::ids::{GlobalId, TypeId};
    use crate::spirv::interp::{self, Buffer, Config, Resources};
    use crate::spirv::ir::{
        natural_layout, ExecMode, ExecModel, IoDir, LayoutRule, MemAccess, StorageClass,
        TypedModule,
    };

    /// The `f32` bits Metal reads of a `BGR5A1Unorm` channel of 5-bit code `c`: the `half` nearest
    /// `c / 31`, which an Apple M4 Max's fetch read of every 16-bit texel, as `float4` and as
    /// `half4` alike (storage-forms bgr5a1_probe, arms fetch_float and fetch_half). Each `c / 31`
    /// past 0 is a normal `half`, so its nearest is the `f32`'s 13 low fraction bits rounded off to
    /// nearest even.
    fn metal_5bit(c: u32) -> u32 {
        metal_half(c, 31)
    }

    /// The `f32` bits of the `half` nearest `c / m`, as the 16-bit formats' tile holds it.
    fn metal_half(c: u32, m: u32) -> u32 {
        let b = ((f64::from(c) / f64::from(m)) as f32).to_bits();
        let (kept, dropped) = (b >> 13, b & 0x1FFF);
        let up = dropped > 0x1000 || (dropped == 0x1000 && kept & 1 == 1);
        (kept + u32::from(up)) << 13
    }

    /// `decode`'s rounding off of an `f32`'s 13 low fraction bits gives the `half` nearest the
    /// exact `c / m` only where the correctly rounded `f32` cannot cross a `half` rounding
    /// boundary: each `c / m` below 1 lies at least 65 `f32` steps from one (132 for `m` 31), in
    /// integers. With `c / m` in `[2^e, 2^(e+1))`, a `half` step is `2^(e-10)`, an `f32` step
    /// `2^(e-23)`, and the distance to the boundary in `f32` steps is `|2r - m| 2^12 / m` for
    /// `r = c 2^(10-e) mod m`.
    #[test]
    fn every_5_and_6_bit_quotient_is_far_from_a_half_boundary() {
        for (m, least) in [(31u64, 132u64), (63, 65)] {
            let mut nearest = u64::MAX;
            for c in 1..m {
                let e = 31 - i64::from(((c << 32) / m).leading_zeros());
                let scale = u64::try_from(10 - e).expect("c / m is at least 2^-6");
                let r = (c << scale) % m;
                assert_ne!(2 * r, m, "{c}/{m} on a boundary");
                nearest = nearest.min((2 * r).abs_diff(m) * 4096 / m);
            }
            assert_eq!(nearest, least, "m {m}");
        }
    }

    fn slot(index: u32) -> ResourceSlot {
        ResourceSlot {
            stage: Stage::Kernel,
            kind: ResourceKind::Buffer,
            index,
        }
    }

    fn storage_buffer(b: &mut ModuleBuilder, index: u32, elem: TypeId) -> BResult<GlobalId> {
        let rta = b.ty(SType::RuntimeArray {
            elem,
            stride: Some(4),
        })?;
        let layout = natural_layout(b.types(), LayoutRule::Std430, true, &[rta])?;
        let block = b.ty(SType::Struct {
            members: [rta].into(),
            layout: Some(layout),
        })?;
        b.resource(slot(index), StorageClass::StorageBuffer, block)
    }

    fn element(
        f: &mut FunctionBuilder<'_>,
        buf: GlobalId,
        (i, n): (SValue, u32),
        k: u32,
    ) -> BResult<SValue> {
        let n = u32_const(f, n)?;
        let at = f.binary(BinOp::IMul, i, n)?;
        let k = u32_const(f, k)?;
        let at = f.binary(BinOp::IAdd, at, k)?;
        let zero = u32_const(f, 0)?;
        let base = f.module().global_value(buf)?;
        f.access_chain(base, &[zero, at])
    }

    /// A kernel whose invocation `i` reads word `i` of buffer 0 as a `format` texel and stores the
    /// bits of its four lanes' values at words `4i` to `4i + 3` of buffer 1.
    fn kernel(format: vk::Format, half_tile: bool) -> BResult<TypedModule> {
        let mut m = ModuleBuilder::new();
        let u32_ = m.ty(u32_type())?;
        let uvec3 = m.ty(SType::Vector { elem: u32_, len: 3 })?;
        let uvec4 = m.ty(SType::Vector { elem: u32_, len: 4 })?;
        let input = storage_buffer(&mut m, 0, u32_)?;
        let output = storage_buffer(&mut m, 1, u32_)?;
        let id = m.builtin(crate::spv::BuiltIn::GlobalInvocationId, IoDir::Input, uvec3)?;
        let void = m.ty(SType::Void)?;
        let mut f = m.function(void)?;
        let gid = f.module().global_value(id)?;
        let gid = f.load(gid, MemAccess::default())?;
        let i = f.composite_extract(gid, &[0])?;
        let at = element(&mut f, input, (i, 1), 0)?;
        let word = f.load(at, MemAccess::default())?;
        let values = channel_values(&mut f, format, word, half_tile)?;
        let values = f.convert(ConvOp::Bitcast, values, uvec4)?;
        for k in 0..4u32 {
            let x = f.composite_extract(values, &[k])?;
            let at = element(&mut f, output, (i, 4), k)?;
            f.store(at, x, MemAccess::default())?;
        }
        f.ret(None)?;
        let main = f.finish()?;
        m.entry(
            ExecModel::GLCompute,
            main,
            vec![ExecMode::LocalSize { x: 1, y: 1, z: 1 }],
        )?;
        m.finalize()
    }

    fn decoded(module: &TypedModule, words: &[u32]) -> Vec<[u32; 4]> {
        let res = Resources {
            buffers: vec![
                Buffer {
                    slot: Some(slot(0)),
                    address: None,
                    bytes: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
                },
                Buffer {
                    slot: Some(slot(1)),
                    address: None,
                    bytes: vec![0xAB; words.len() * 16],
                },
            ],
            ..Resources::default()
        };
        let cfg = Config {
            workgroups: [u32::try_from(words.len()).expect("words"), 1, 1],
            ..Config::default()
        };
        let (out, _) = interp::run_with_images(module, &cfg, res, Vec::new()).expect("run");
        out.buffers[1]
            .as_chunks::<16>()
            .0
            .iter()
            .map(|c| {
                std::array::from_fn(|k| {
                    u32::from_le_bytes(c[4 * k..4 * k + 4].try_into().expect("word"))
                })
            })
            .collect()
    }

    /// The `f32` nearest `c / m`, which the exact read gives.
    fn exact(c: u32, m: u32) -> u32 {
        ((f64::from(c) / f64::from(m)) as f32).to_bits()
    }

    /// Every 5-bit code in each channel of a `BGR5A1Unorm` texel, with alpha 0 and 1.
    fn texels() -> Vec<u32> {
        (0..32u32)
            .flat_map(|c| [0, 1].map(|a| c << 10 | (31 - c) << 5 | (c * 7 % 32) | a << 15))
            .collect()
    }

    /// A `BGR5A1Unorm` texel reads as Metal's tile holds a loaded one, each channel the `half`
    /// nearest its value. The exact quotient, which the read gave before, is another `f32` for 30
    /// of the 32 codes.
    #[test]
    fn a_bgr5a1_texel_reads_the_half_metal_reads() {
        assert_eq!(
            [1, 15, 16].map(metal_5bit),
            [0x3d04_2000, 0x3ef7_c000, 0x3f04_2000]
        );
        let words = texels();
        let module = kernel(vk::Format::A1R5G5B5_UNORM_PACK16, true).expect("kernel");
        for (w, got) in words.iter().zip(decoded(&module, &words)) {
            let code = |at: u32| metal_5bit(w >> at & 31);
            let alpha = if w >> 15 != 0 { 1.0f32.to_bits() } else { 0 };
            assert_eq!(got, [code(10), code(5), code(0), alpha], "texel {w:#06x}");
        }
    }

    /// Every 6-bit green code with every 5-bit red and blue code of a `B5G6R5Unorm` texel.
    fn texels_565() -> Vec<u32> {
        (0..64u32)
            .map(|g| (g % 32) << 11 | g << 5 | (g * 7 % 32))
            .collect()
    }

    /// A `B5G6R5Unorm` texel reads as Metal's tile holds a loaded one: each channel the `half`
    /// nearest its value, alpha 1. Unrounded, each channel is its exact quotient.
    #[test]
    fn a_b5g6r5_texel_reads_the_half_metal_reads_and_alpha_one() {
        let one = 1.0f32.to_bits();
        let words = texels_565();
        let rounded = kernel(vk::Format::R5G6B5_UNORM_PACK16, true).expect("kernel");
        for (w, got) in words.iter().zip(decoded(&rounded, &words)) {
            let want = [
                metal_half(w >> 11 & 31, 31),
                metal_half(w >> 5 & 63, 63),
                metal_half(w & 31, 31),
                one,
            ];
            assert_eq!(got, want, "texel {w:#06x}");
        }
        let unrounded = kernel(vk::Format::R5G6B5_UNORM_PACK16, false).expect("kernel");
        for (w, got) in words.iter().zip(decoded(&unrounded, &words)) {
            let want = [exact(w >> 11 & 31, 31), exact(w >> 5 & 63, 63), exact(w & 31, 31), one];
            assert_eq!(got, want, "texel {w:#06x}");
        }
    }

    /// Without the `half` rounding (the m2v runtime's Vulkan compile, a `half` reader, the BGR10A2
    /// view) the read is each channel's exact quotient: code 1 is `0x3d04_2108`, not the `half`.
    #[test]
    fn an_unrounded_read_keeps_the_exact_quotient() {
        assert_eq!(exact(1, 31), 0x3d04_2108);
        let words = texels();
        let module = kernel(vk::Format::A1R5G5B5_UNORM_PACK16, false).expect("kernel");
        for (w, got) in words.iter().zip(decoded(&module, &words)) {
            let code = |at: u32| exact(w >> at & 31, 31);
            assert_eq!(
                got,
                [code(10), code(5), code(0), exact(w >> 15, 1)],
                "texel {w:#06x}"
            );
        }
    }
}
