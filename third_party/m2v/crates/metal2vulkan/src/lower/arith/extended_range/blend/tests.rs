//! Metal's 16-bit float blend ([`Blend::half_float`]) run through the interpreter against the
//! module docs' rule, computed here apart from the shader.

use super::{blend_half_float, Blend};
use crate::lower::arith::extended_range::tests::{slot, storage_buffer, word};
use crate::metal::{BlendFactor as F, BlendOperation as O, ColorWriteMask};
use crate::pipeline::lower_inputs::BlendEquation;
use crate::spirv::builder::ModuleBuilder;
use crate::spirv::error::BResult;
use crate::spirv::interp::{self, Buffer, Config, Resources};
use crate::spirv::ir::{ConvOp, ExecMode, ExecModel, IoDir, MemAccess, Type, TypedModule};

/// Each invocation's input words: the source's four `f32`s, the texel's, and the blend color's.
const WORDS: u32 = 12;

/// The factors the blend computes (every one but the second source's).
const FACTORS: [F; 15] = [
    F::Zero,
    F::One,
    F::SourceColor,
    F::OneMinusSourceColor,
    F::SourceAlpha,
    F::OneMinusSourceAlpha,
    F::DestinationColor,
    F::OneMinusDestinationColor,
    F::DestinationAlpha,
    F::OneMinusDestinationAlpha,
    F::SourceAlphaSaturated,
    F::BlendColor,
    F::OneMinusBlendColor,
    F::BlendAlpha,
    F::OneMinusBlendAlpha,
];

/// Every channel, two color channels and alpha, alpha alone, and two color channels alone.
const MASKS: [ColorWriteMask; 4] = [
    ColorWriteMask::ALL,
    ColorWriteMask::RED
        .union(ColorWriteMask::GREEN)
        .union(ColorWriteMask::ALPHA),
    ColorWriteMask::ALPHA,
    ColorWriteMask::RED.union(ColorWriteMask::BLUE),
];

/// `half` bits the sources and texels start with: zeros, one and its neighbours, a half, the
/// smallest normal and denormal, and the largest value below 4.
const SPECIAL: [u16; 12] = [
    0x0000, 0x8000, 0x3C00, 0xBC00, 0x3BFF, 0x3C01, 0x3800, 0x0400, 0x0001, 0x8001, 0x43FF, 0xC3FF,
];

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 32) as u32
    }

    /// A finite `half` below 4 in magnitude, denormals among them.
    fn half(&mut self) -> u16 {
        let r = self.next();
        let exponent = r % 17;
        ((r >> 16 & 0x8000) | exponent << 10 | (r >> 5 & 0x3FF)) as u16
    }

    /// An `f32` in `[-2, 2]`, most of them no `half`.
    fn color(&mut self) -> f32 {
        (f64::from(self.next()) / f64::from(u32::MAX) * 4.0 - 2.0) as f32
    }
}

/// A kernel whose invocation `i` reads [`WORDS`] words from buffer 0 and writes the blend's four
/// lanes to buffer 1.
fn kernel(blend: &Blend) -> BResult<TypedModule> {
    let mut b = ModuleBuilder::new();
    let f32_ = b.ty(Type::Float { width: 32 })?;
    let u32_ = b.ty(Type::Int {
        width: 32,
        signed: false,
    })?;
    let uvec3 = b.ty(Type::Vector { elem: u32_, len: 3 })?;
    let uvec4 = b.ty(Type::Vector { elem: u32_, len: 4 })?;
    let vec4 = b.ty(Type::Vector { elem: f32_, len: 4 })?;
    let input = storage_buffer(&mut b, 0, u32_)?;
    let output = storage_buffer(&mut b, 1, u32_)?;
    let id = b.builtin(crate::spv::BuiltIn::GlobalInvocationId, IoDir::Input, uvec3)?;
    let void = b.ty(Type::Void)?;
    let mut f = b.function(void)?;
    let gid = f.module().global_value(id)?;
    let gid = f.load(gid, MemAccess::default())?;
    let i = f.composite_extract(gid, &[0])?;
    let mut words = Vec::with_capacity(WORDS as usize);
    for k in 0..WORDS {
        let at = word(&mut f, input, (i, WORDS), k)?;
        words.push(f.load(at, MemAccess::default())?);
    }
    let s = f.composite_construct(uvec4, &words[0..4])?;
    let s = f.convert(ConvOp::Bitcast, s, vec4)?;
    let d = f.composite_construct(uvec4, &words[4..8])?;
    let d = f.convert(ConvOp::Bitcast, d, vec4)?;
    let color = match blend.reads_constant() {
        true => Some(f.composite_construct(uvec4, &words[8..12])?),
        false => None,
    };
    let out = blend_half_float(&mut f, blend, (s, d), color)?;
    let out = f.convert(ConvOp::Bitcast, out, uvec4)?;
    for k in 0..4u32 {
        let c = f.composite_extract(out, &[k])?;
        let at = word(&mut f, output, (i, 4), k)?;
        f.store(at, c, MemAccess::default())?;
    }
    f.ret(None)?;
    let main = f.finish()?;
    b.entry(
        ExecModel::GLCompute,
        main,
        vec![ExecMode::LocalSize { x: 1, y: 1, z: 1 }],
    )?;
    b.finalize()
}

/// The lanes `blend` computes of each invocation's words.
fn run(blend: &Blend, lanes: &[[u32; WORDS as usize]]) -> Vec<[f32; 4]> {
    let m = kernel(blend).expect("kernel");
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
                f32::from_le_bytes(c[4 * k..4 * k + 4].try_into().expect("word"))
            })
        })
        .collect()
}

/// `x` (exact in `f64`) rounded to `half`, nearest even. No value here reaches the infinities.
fn h(x: f64) -> f64 {
    if x == 0.0 {
        return x;
    }
    let e = ((x.abs().to_bits() >> 52) & 0x7FF) as i32 - 1023;
    // A `half`'s spacing at `x`: `2^(e - 10)`, at least the denormals' `2^-24`.
    let ulp = 2f64.powi(e.max(-14) - 10);
    let y = (x / ulp).round_ties_even() * ulp;
    assert!(y.abs() <= 65504.0, "{x} rounds past the largest half");
    y
}

/// The value factor `f` takes in lane `k` (alpha's where `k` is 3) of source `s`, texel `d` and
/// blend color `c`, each a `half`.
fn factor(f: F, k: usize, (s, d, c): ([f64; 4], [f64; 4], [f64; 4])) -> f64 {
    let one_minus = |x: f64| h(1.0 - x);
    match f {
        F::Zero => 0.0,
        F::One => 1.0,
        F::SourceColor => s[k],
        F::OneMinusSourceColor => one_minus(s[k]),
        F::SourceAlpha => s[3],
        F::OneMinusSourceAlpha => one_minus(s[3]),
        F::DestinationColor => d[k],
        F::OneMinusDestinationColor => one_minus(d[k]),
        F::DestinationAlpha => d[3],
        F::OneMinusDestinationAlpha => one_minus(d[3]),
        F::SourceAlphaSaturated if k == 3 => 1.0,
        F::SourceAlphaSaturated => s[3].min(one_minus(d[3])),
        F::BlendColor => c[k],
        F::OneMinusBlendColor => one_minus(c[k]),
        F::BlendAlpha => c[3],
        F::OneMinusBlendAlpha => one_minus(c[3]),
        _ => unreachable!("{f:?}"),
    }
}

/// Lane `k` of blend `eq` (the module docs' rule): each product rounded to `half`, then their sum.
/// Every product of two `half`s and every sum of two is exact in `f64`.
fn lane(eq: &BlendEquation, k: usize, v: ([f64; 4], [f64; 4], [f64; 4])) -> f64 {
    let (fs, fd, op) = match k {
        3 => (eq.source_alpha, eq.destination_alpha, eq.alpha_operation),
        _ => (eq.source_rgb, eq.destination_rgb, eq.rgb_operation),
    };
    let (s, d) = (v.0[k], v.1[k]);
    let term = |x: f64, f: F| h(x * factor(f, k, v));
    match op {
        O::Add => h(term(s, fs) + term(d, fd)),
        O::Subtract => h(term(s, fs) - term(d, fd)),
        O::ReverseSubtract => h(term(d, fd) - term(s, fs)),
        O::Min => s.min(d),
        O::Max => s.max(d),
        _ => unreachable!("{op:?}"),
    }
}

/// Every factor pair under each operation that reads them, then `Min` and `Max`.
fn keys() -> Vec<(F, F, O)> {
    let mut out = Vec::new();
    for op in [O::Add, O::Subtract, O::ReverseSubtract] {
        for fs in FACTORS {
            for fd in FACTORS {
                out.push((fs, fd, op));
            }
        }
    }
    out.push((F::One, F::One, O::Min));
    out.push((F::One, F::One, O::Max));
    out
}

/// The shader computes each blend's rule for every color key (each paired with an alpha key in
/// turn) under several masks, over random sources, texels and blend colors; a lane the write does
/// not store keeps the texel's.
#[test]
fn every_half_blend_computes_metals_rule() {
    let keys = keys();
    let mut r = Rng(0x4842_4C44);
    for (i, &(fs, fd, op)) in keys.iter().enumerate() {
        let (sa, da, opa) = keys[(i * 31 + 7) % keys.len()];
        let eq = BlendEquation {
            source_rgb: fs,
            destination_rgb: fd,
            rgb_operation: op,
            source_alpha: sa,
            destination_alpha: da,
            alpha_operation: opa,
        };
        let written = MASKS[i % MASKS.len()];
        let blend = Blend::half_float(&eq, written).expect("a blend");
        let mut lanes = Vec::new();
        let mut values = Vec::new();
        for n in 0..32 {
            let mut half = |j: usize| match SPECIAL.get((n * 8 + j + i) % 24) {
                Some(&bits) => bits,
                None => r.half(),
            };
            let s: [f32; 4] = std::array::from_fn(|j| crate::float16::f16_bits_to_f32(half(j)));
            let d: [f32; 4] = std::array::from_fn(|j| crate::float16::f16_bits_to_f32(half(4 + j)));
            let c: [f32; 4] = std::array::from_fn(|_| r.color());
            let mut words = [0u32; WORDS as usize];
            for (w, x) in words.iter_mut().zip(s.iter().chain(&d).chain(&c)) {
                *w = x.to_bits();
            }
            lanes.push(words);
            values.push((
                s.map(f64::from),
                d.map(f64::from),
                c.map(|x| h(f64::from(x))),
            ));
        }
        let got = run(&blend, &lanes);
        let colors = [
            ColorWriteMask::RED,
            ColorWriteMask::GREEN,
            ColorWriteMask::BLUE,
        ]
        .into_iter()
        .any(|m| written.contains(m));
        for (n, (out, v)) in got.iter().zip(&values).enumerate() {
            for (k, &x) in out.iter().enumerate() {
                let stored = match k {
                    3 => written.contains(ColorWriteMask::ALPHA),
                    _ => colors,
                };
                let want = match stored {
                    true => lane(&eq, k, *v),
                    false => v.1[k],
                };
                assert!(
                    f64::from(x) == want,
                    "{eq:?} {written:?} invocation {n} lane {k}: {x} where {want} ({v:?})"
                );
            }
        }
    }
}
