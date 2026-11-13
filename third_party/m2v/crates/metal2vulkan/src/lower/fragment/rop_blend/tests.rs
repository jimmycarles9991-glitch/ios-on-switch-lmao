//! The storage form's fixed-function blend run through the interpreter against a plain `f32`
//! reference of the module docs' model, bit for bit (any NaN for a NaN).

use super::{blend, Channels, RopBlend};
use crate::metal::{BlendFactor, BlendOperation, PixelFormat};
use crate::pipeline::lower_inputs::BlendEquation;
use crate::pipeline::sym::{ResourceKind, ResourceSlot, Stage};
use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
use crate::spirv::error::BResult;
use crate::spirv::ids::{GlobalId, TypeId, Value};
use crate::spirv::interp::{self, Buffer, Config, Resources};
use crate::spirv::ir::{
    natural_layout, BinOp, ConvOp, ExecMode, ExecModel, IoDir, LayoutRule, MemAccess, StorageClass,
    Type, TypedModule,
};
use crate::vk;

const FACTORS: [BlendFactor; 19] = [
    BlendFactor::Zero,
    BlendFactor::One,
    BlendFactor::SourceColor,
    BlendFactor::OneMinusSourceColor,
    BlendFactor::SourceAlpha,
    BlendFactor::OneMinusSourceAlpha,
    BlendFactor::DestinationColor,
    BlendFactor::OneMinusDestinationColor,
    BlendFactor::DestinationAlpha,
    BlendFactor::OneMinusDestinationAlpha,
    BlendFactor::SourceAlphaSaturated,
    BlendFactor::BlendColor,
    BlendFactor::OneMinusBlendColor,
    BlendFactor::BlendAlpha,
    BlendFactor::OneMinusBlendAlpha,
    BlendFactor::Source1Color,
    BlendFactor::OneMinusSource1Color,
    BlendFactor::Source1Alpha,
    BlendFactor::OneMinusSource1Alpha,
];

const OPS: [BlendOperation; 5] = [
    BlendOperation::Add,
    BlendOperation::Subtract,
    BlendOperation::ReverseSubtract,
    BlendOperation::Min,
    BlendOperation::Max,
];

const SPECIALS: [u32; 12] = [
    0x7FC0_0000,
    0xFFC1_2345,
    0x7F80_0000,
    0xFF80_0000,
    0x8000_0000,
    0x0000_0001,
    0x807F_FFFF,
    0x477F_E000,
    0x3F80_0000,
    0x0080_0000,
    0x3F00_0000,
    0xBF00_0000,
];

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 16) as u32
    }

    fn word(&mut self) -> u32 {
        match self.next() % 8 {
            0 => SPECIALS[self.next() as usize % SPECIALS.len()],
            1 => self.next(),
            _ => ((f64::from(self.next() % 3001) / 2000.0 - 0.25) as f32).to_bits(),
        }
    }
}

/// `n / m` correctly rounded to `f32`.
fn quotient(n: u32, m: u32) -> f32 {
    if n == 0 {
        return 0.0;
    }
    let q = f64::from(n) / f64::from(m);
    let y = q as f32;
    let err = |c: f32| (f64::from(c) * f64::from(m) - f64::from(n)).abs();
    [
        y,
        f32::from_bits(y.to_bits().wrapping_sub(1)),
        f32::from_bits(y.to_bits() + 1),
    ]
    .into_iter()
    .min_by(|a, b| err(*a).partial_cmp(&err(*b)).expect("finite"))
    .expect("a candidate")
}

fn unit(x: f32) -> f32 {
    if x > 0.0 {
        x.min(1.0)
    } else {
        0.0
    }
}

fn number_min(a: f32, b: f32, max: bool) -> f32 {
    match (a.is_nan(), b.is_nan()) {
        (true, _) => b,
        (_, true) => a,
        _ if max => a.max(b),
        _ => a.min(b),
    }
}

struct Inputs {
    s: [f32; 4],
    s1: [f32; 4],
    d: [f32; 4],
    c: [f32; 4],
}

fn reference_lane(
    i: &Inputs,
    (fs, fd, op): (BlendFactor, BlendFactor, BlendOperation),
    k: usize,
) -> f32 {
    use BlendFactor as F;
    let alpha = k == 3;
    let f = |f: BlendFactor| -> Option<f32> {
        Some(match f {
            F::Zero => return None,
            F::One => return Some(1.0),
            F::SourceAlphaSaturated if alpha => return Some(1.0),
            F::SourceColor => i.s[k],
            F::OneMinusSourceColor => 1.0 - i.s[k],
            F::SourceAlpha => i.s[3],
            F::OneMinusSourceAlpha => 1.0 - i.s[3],
            F::DestinationColor => i.d[k],
            F::OneMinusDestinationColor => 1.0 - i.d[k],
            F::DestinationAlpha => i.d[3],
            F::OneMinusDestinationAlpha => 1.0 - i.d[3],
            F::SourceAlphaSaturated => i.s[3].min(1.0 - i.d[3]),
            F::BlendColor => i.c[k],
            F::OneMinusBlendColor => 1.0 - i.c[k],
            F::BlendAlpha => i.c[3],
            F::OneMinusBlendAlpha => 1.0 - i.c[3],
            F::Source1Color => i.s1[k],
            F::OneMinusSource1Color => 1.0 - i.s1[k],
            F::Source1Alpha => i.s1[3],
            F::OneMinusSource1Alpha => 1.0 - i.s1[3],
            F::Unspecialized => unreachable!(),
        })
    };
    let term = |x: f32, factor: BlendFactor| match f(factor) {
        None => None,
        Some(_) if factor == F::One || (alpha && factor == F::SourceAlphaSaturated) => Some(x),
        Some(k) => Some(x * k),
    };
    let (first, second, add) = match op {
        BlendOperation::Min => return number_min(i.s[k], i.d[k], false),
        BlendOperation::Max => return number_min(i.s[k], i.d[k], true),
        BlendOperation::ReverseSubtract => (term(i.d[k], fd), term(i.s[k], fs), false),
        BlendOperation::Subtract => (term(i.s[k], fs), term(i.d[k], fd), false),
        _ => (term(i.s[k], fs), term(i.d[k], fd), true),
    };
    match (first, second) {
        (Some(a), Some(b)) if add => a + b,
        (Some(a), Some(b)) => a - b,
        (Some(a), None) => a,
        (None, Some(b)) if add => b,
        (None, Some(b)) => -b,
        (None, None) => 0.0,
    }
}

/// The model's result of `eq` over raw words: normalized operands clamped and the destination
/// code `code` decoded exactly; a float format's taken as they are.
fn reference(b: &RopBlend, w: &[u32; 16], code: [u32; 4]) -> [f32; 4] {
    let v = |k: usize| std::array::from_fn::<f32, 4, _>(|j| f32::from_bits(w[k + j]));
    let mut i = Inputs {
        s: v(0),
        s1: v(4),
        d: v(8),
        c: v(12),
    };
    if let Channels::Unorm { steps, .. } = b.channels {
        i.s = i.s.map(unit);
        i.s1 = i.s1.map(unit);
        i.c = i.c.map(unit);
        i.d = std::array::from_fn(|k| quotient(code[k], steps[k]));
    }
    if !b.alpha {
        i.d[3] = 1.0;
    }
    let eq = b.eq;
    std::array::from_fn(|k| match k {
        3 => reference_lane(
            &i,
            (eq.source_alpha, eq.destination_alpha, eq.alpha_operation),
            k,
        ),
        _ => reference_lane(&i, (eq.source_rgb, eq.destination_rgb, eq.rgb_operation), k),
    })
}

fn slot(index: u32) -> ResourceSlot {
    ResourceSlot {
        stage: Stage::Kernel,
        kind: ResourceKind::Buffer,
        index,
    }
}

fn storage_buffer(b: &mut ModuleBuilder, index: u32, elem: TypeId) -> BResult<GlobalId> {
    let rta = b.ty(Type::RuntimeArray {
        elem,
        stride: Some(4),
    })?;
    let layout = natural_layout(b.types(), LayoutRule::Std430, true, &[rta])?;
    let block = b.ty(Type::Struct {
        members: [rta].into(),
        layout: Some(layout),
    })?;
    b.resource(slot(index), StorageClass::StorageBuffer, block)
}

fn word(
    f: &mut FunctionBuilder<'_>,
    buf: GlobalId,
    (i, n): (Value, u32),
    k: u32,
) -> BResult<Value> {
    let n = f.u32_const(n)?;
    let n = f.module().const_value(n)?;
    let at = f.binary(BinOp::IMul, i, n)?;
    let k = f.u32_const(k)?;
    let k = f.module().const_value(k)?;
    let at = f.binary(BinOp::IAdd, at, k)?;
    let zero = f.u32_const(0)?;
    let zero = f.module().const_value(zero)?;
    let base = f.module().global_value(buf)?;
    f.access_chain(base, &[zero, at])
}

/// A kernel whose invocation `i` reads the source, second source, destination and blend color
/// (sixteen words) and writes the blend's four lanes.
fn kernel(b: &RopBlend) -> BResult<TypedModule> {
    let mut m = ModuleBuilder::new();
    let f32_ = m.ty(Type::Float { width: 32 })?;
    let u32_ = m.ty(Type::Int {
        width: 32,
        signed: false,
    })?;
    let uvec3 = m.ty(Type::Vector { elem: u32_, len: 3 })?;
    let uvec4 = m.ty(Type::Vector { elem: u32_, len: 4 })?;
    let vec4 = m.ty(Type::Vector { elem: f32_, len: 4 })?;
    let input = storage_buffer(&mut m, 0, u32_)?;
    let output = storage_buffer(&mut m, 1, u32_)?;
    let id = m.builtin(crate::spv::BuiltIn::GlobalInvocationId, IoDir::Input, uvec3)?;
    let void = m.ty(Type::Void)?;
    let mut f = m.function(void)?;
    let gid = f.module().global_value(id)?;
    let gid = f.load(gid, MemAccess::default())?;
    let i = f.composite_extract(gid, &[0])?;
    let mut words = Vec::with_capacity(16);
    for k in 0..16 {
        let at = word(&mut f, input, (i, 16), k)?;
        words.push(f.load(at, MemAccess::default())?);
    }
    let part = |f: &mut FunctionBuilder<'_>, k: usize| -> BResult<Value> {
        let x = f.composite_construct(uvec4, &words[k..k + 4])?;
        f.convert(ConvOp::Bitcast, x, vec4)
    };
    let s = part(&mut f, 0)?;
    let s1 = part(&mut f, 4)?;
    let d = part(&mut f, 8)?;
    let c = f.composite_construct(uvec4, &words[12..16])?;
    let out = blend(
        &mut f,
        b,
        (s, b.reads_second().then_some(s1), d),
        b.reads_constant().then_some(c),
    )?;
    let out = f.convert(ConvOp::Bitcast, out, uvec4)?;
    for k in 0..4u32 {
        let x = f.composite_extract(out, &[k])?;
        let at = word(&mut f, output, (i, 4), k)?;
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

fn run(b: &RopBlend, lanes: &[[u32; 16]]) -> Vec<[u32; 4]> {
    let m = kernel(b).expect("kernel");
    let bytes: Vec<u8> = lanes
        .iter()
        .flatten()
        .flat_map(|w| w.to_le_bytes())
        .collect();
    let res = Resources {
        buffers: vec![
            Buffer {
                slot: Some(slot(0)),
                address: None,
                bytes,
            },
            Buffer {
                slot: Some(slot(1)),
                address: None,
                bytes: vec![0xAB; lanes.len() * 16],
            },
        ],
        ..Resources::default()
    };
    let cfg = Config {
        workgroups: [u32::try_from(lanes.len()).expect("lanes"), 1, 1],
        ..Config::default()
    };
    let (out, _) = interp::run_with_images(&m, &cfg, res, Vec::new()).expect("run");
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

fn same(got: u32, want: f32) -> bool {
    got == want.to_bits() || (f32::from_bits(got).is_nan() && want.is_nan())
}

/// Random operands for `b`, the destination as a decode that multiplies by the reciprocal
/// would give it for a normalized format (a code per lane, returned).
fn operands(rng: &mut Rng, b: &RopBlend) -> ([u32; 16], [u32; 4]) {
    let mut w: [u32; 16] = std::array::from_fn(|_| rng.word());
    let mut code = [0; 4];
    if let Channels::Unorm { steps, .. } = b.channels {
        for k in 0..4 {
            code[k] = rng.next() % (steps[k] + 1);
            w[8 + k] = (code[k] as f32 * (1.0 / steps[k] as f32)).to_bits();
        }
    }
    (w, code)
}

fn check(pf: PixelFormat, format: vk::Format, pairs: &[(BlendFactor, BlendFactor)], seed: u64) {
    let mut rng = Rng(seed);
    for &(fs, fd) in pairs {
        for op in OPS {
            let (afs, afd) = (
                FACTORS[(rng.next() % 19) as usize],
                FACTORS[(rng.next() % 19) as usize],
            );
            let aop = OPS[(rng.next() % 5) as usize];
            for (fa, fb_, oa) in [(fs, fd, op), (afs, afd, aop)] {
                let eq = BlendEquation {
                    source_rgb: fs,
                    destination_rgb: fd,
                    rgb_operation: op,
                    source_alpha: fa,
                    destination_alpha: fb_,
                    alpha_operation: oa,
                };
                let b = RopBlend::of(&eq, format).expect("a modelled blend");
                let cases: Vec<([u32; 16], [u32; 4])> =
                    (0..24).map(|_| operands(&mut rng, &b)).collect();
                let lanes: Vec<[u32; 16]> = cases.iter().map(|c| c.0).collect();
                for ((w, code), got) in cases.iter().zip(run(&b, &lanes)) {
                    let want = reference(&b, w, *code);
                    let lanes = if b.alpha { 4 } else { 3 };
                    for k in 0..lanes {
                        assert!(
                            same(got[k], want[k]),
                            "{pf:?} {eq:?} lane {k}: {:#x} vs {:#x} of {w:x?}",
                            got[k],
                            want[k].to_bits()
                        );
                    }
                }
            }
        }
    }
}

fn pairs(stride: usize, offset: usize) -> Vec<(BlendFactor, BlendFactor)> {
    let mut out = Vec::new();
    for (i, &fs) in FACTORS.iter().enumerate() {
        for (j, &fd) in FACTORS.iter().enumerate() {
            if (i * 19 + j) % stride == offset {
                out.push((fs, fd));
            }
        }
    }
    out
}

#[test]
fn unorm8_blends_are_the_models() {
    check(
        PixelFormat::BGRA8Unorm,
        vk::Format::B8G8R8A8_UNORM,
        &pairs(3, 0),
        1,
    );
}

#[test]
fn half_float_blends_are_the_models() {
    check(
        PixelFormat::RGBA16Float,
        vk::Format::R16G16B16A16_SFLOAT,
        &pairs(3, 1),
        2,
    );
}

#[test]
fn float_blends_are_the_models() {
    check(
        PixelFormat::R32Float,
        vk::Format::R32_SFLOAT,
        &pairs(3, 2),
        3,
    );
    check(
        PixelFormat::RG16Float,
        vk::Format::R16G16_SFLOAT,
        &pairs(7, 3),
        4,
    );
}

#[test]
fn other_unorm_blends_are_the_models() {
    check(PixelFormat::R8Unorm, vk::Format::R8_UNORM, &pairs(5, 0), 5);
    check(
        PixelFormat::RGB10A2Unorm,
        vk::Format::A2B10G10R10_UNORM_PACK32,
        &pairs(7, 1),
        6,
    );
    check(
        PixelFormat::RGBA16Unorm,
        vk::Format::R16G16B16A16_UNORM,
        &pairs(11, 2),
        7,
    );
}

/// Every code of 8, 10 and 16 bits read through a reciprocal's product blends as its exact value.
#[test]
fn every_destination_code_is_exact() {
    let eq = BlendEquation {
        source_rgb: BlendFactor::Zero,
        destination_rgb: BlendFactor::One,
        rgb_operation: BlendOperation::Add,
        source_alpha: BlendFactor::Zero,
        destination_alpha: BlendFactor::One,
        alpha_operation: BlendOperation::Add,
    };
    for (pf, format, m) in [
        (PixelFormat::RGBA8Unorm, vk::Format::R8G8B8A8_UNORM, 255u32),
        (
            PixelFormat::RGBA16Unorm,
            vk::Format::R16G16B16A16_UNORM,
            65535,
        ),
    ] {
        let b = RopBlend::of(&eq, format).expect("a modelled blend");
        let lanes: Vec<[u32; 16]> = (0..=m)
            .step_by(4)
            .map(|c| {
                let mut w = [0u32; 16];
                for k in 0..4 {
                    let code = (c + k as u32).min(m);
                    w[8 + k] = (code as f32 * (1.0 / m as f32)).to_bits();
                }
                w
            })
            .collect();
        for (w, got) in lanes.iter().zip(run(&b, &lanes)) {
            for k in 0..4 {
                let code = (f32::from_bits(w[8 + k]) * m as f32).round() as u32;
                assert_eq!(got[k], quotient(code, m).to_bits(), "{pf:?} code {code}");
            }
        }
    }
}

#[test]
fn formats_outside_the_model_are_refused() {
    let eq = BlendEquation {
        source_rgb: BlendFactor::One,
        destination_rgb: BlendFactor::OneMinusSourceAlpha,
        rgb_operation: BlendOperation::Add,
        source_alpha: BlendFactor::One,
        destination_alpha: BlendFactor::OneMinusSourceAlpha,
        alpha_operation: BlendOperation::Add,
    };
    for (pf, format) in [
        (PixelFormat::RGBA8Snorm, vk::Format::R8G8B8A8_SNORM),
        (PixelFormat::RGBA8Uint, vk::Format::R8G8B8A8_UINT),
        (
            PixelFormat::RG11B10Float,
            vk::Format::B10G11R11_UFLOAT_PACK32,
        ),
    ] {
        assert!(RopBlend::of(&eq, format).is_err(), "{pf:?}");
    }
}
