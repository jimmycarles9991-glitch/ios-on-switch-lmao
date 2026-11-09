// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! ios-on-switch-lmao: SM53's half ALU with an f32 result (OFMT F32), in the
//! two readings the device has to tell apart, for SwitchHWTest's
//! gpu.sm53_probe (never in release builds). They apply only to a shader
//! carrying SM53_PROBE_MARKER, which the probe's Metal source compares its
//! thread index with:
//!
//! - the f32 widening of a half sum, product or fma is the op with an f32
//!   result, if the op rounds to half before it widens;
//! - an f32 sum, product or fma of widened halves is the half op with an f32
//!   result, if the op rounds the exact result once to f32.
//!
//! At most one of them holds. Its kernels match the CPU and the other's do
//! not; the one that holds becomes a rule of its own.
//!
//! opt_sm53_f32_source_probe asks the same of HADD2's F32 swizzle: an f32
//! narrowed to half, and its round trip through half, as one HADD2 each.

use crate::ir::*;

use rustc_hash::FxHashMap;

/// hw_test_app/shaders/Scenes.metal SM53_PROBE_MARKER: a signalling NaN's
/// bits, a value no other shader has a use for
const SM53_PROBE_MARKER: u32 = 0x7fa5_5353;

#[derive(Clone, Copy, PartialEq)]
enum Half {
    Add,
    Mul,
    Fma,
}

enum Def {
    /// F2F.F32.F16 of a half, unmodified: the half (its lane in the swizzle)
    Widened(Src),
    /// A half op with a half result, no flushes
    Half {
        kind: Half,
        srcs: Vec<Src>,
        saturate: bool,
    },
}

fn ssa(src: &Src) -> Option<SSAValue> {
    let ssa = src.src_ref.as_ssa()?;
    if ssa.comps() == 1 { Some(ssa[0]) } else { None }
}

fn widening(op: &OpF2F) -> bool {
    op.src_type == FloatType::F16
        && op.dst_type == FloatType::F32
        && !op.integer_rnd
        && op.src.src_mod.is_none()
}

/// The half a widened source of an f32 op reads, with the op's modifiers
fn half_of(defs: &FxHashMap<SSAValue, Def>, src: &Src) -> Option<Src> {
    let Some(Def::Widened(h)) = defs.get(&ssa(src)?) else {
        return None;
    };
    let swizzle = if matches!(h.src_swizzle, SrcSwizzle::Yy) {
        SrcSwizzle::Yy
    } else {
        SrcSwizzle::Xx
    };
    Some(Src {
        src_ref: h.src_ref.clone(),
        src_mod: src.src_mod,
        src_swizzle: swizzle,
    })
}

/// The half op `kind` of `srcs` with an f32 result: a product as an fma
/// adding -0, which leaves every product as it is
fn with_f32_result(dst: Dst, kind: Half, srcs: &[Src], saturate: bool) -> Option<Op> {
    if kind != Half::Add && srcs.iter().any(|s| s.src_mod.has_fabs()) {
        return None;
    }
    Some(match (kind, srcs) {
        (Half::Add, [a, b]) => OpHAdd2 {
            dst,
            srcs: [a.clone(), b.clone()],
            saturate,
            ftz: false,
            f32: true,
            a_f32: false,
        }
        .into(),
        (Half::Mul, [a, b]) => OpHFma2 {
            dst,
            srcs: [a.clone(), b.clone(), Src::ZERO.fneg()],
            saturate,
            ftz: false,
            dnz: false,
            f32: true,
        }
        .into(),
        (Half::Fma, [a, b, c]) => OpHFma2 {
            dst,
            srcs: [a.clone(), b.clone(), c.clone()],
            saturate,
            ftz: false,
            dnz: false,
            f32: true,
        }
        .into(),
        _ => return None,
    })
}

fn halves(defs: &FxHashMap<SSAValue, Def>, srcs: &[Src]) -> Option<Vec<Src>> {
    srcs.iter().map(|s| half_of(defs, s)).collect()
}

fn rewrite(defs: &FxHashMap<SSAValue, Def>, op: &Op) -> Option<Op> {
    match op {
        Op::F2F(f)
            if widening(f)
                && matches!(f.src.src_swizzle, SrcSwizzle::None | SrcSwizzle::Xx) =>
        {
            let Def::Half { kind, srcs, saturate } = defs.get(&ssa(&f.src)?)? else {
                return None;
            };
            with_f32_result(f.dst.clone(), *kind, srcs, *saturate)
        }
        Op::FAdd(o) if o.rnd_mode == FRndMode::NearestEven => {
            with_f32_result(o.dst.clone(), Half::Add, &halves(defs, &o.srcs)?, o.saturate)
        }
        Op::FMul(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(o.dst.clone(), Half::Mul, &halves(defs, &o.srcs)?, o.saturate)
        }
        Op::FFma(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(o.dst.clone(), Half::Fma, &halves(defs, &o.srcs)?, o.saturate)
        }
        _ => None,
    }
}

fn record(defs: &mut FxHashMap<SSAValue, Def>, op: &Op) {
    let (dst, def) = match op {
        Op::F2F(f) if widening(f) && f.src.src_ref.as_ssa().is_some() => {
            (&f.dst, Def::Widened(f.src.clone()))
        }
        Op::HAdd2(h) if !h.f32 && !h.ftz => (
            &h.dst,
            Def::Half { kind: Half::Add, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        Op::HMul2(h) if !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half { kind: Half::Mul, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        Op::HFma2(h) if !h.f32 && !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half { kind: Half::Fma, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        _ => return,
    };
    if let Some(ssa) = dst.as_ssa() {
        if ssa.comps() == 1 {
            defs.insert(ssa[0], def);
        }
    }
}

impl Shader<'_> {
    pub fn carries_sm53_probe_marker(&self) -> bool {
        self.functions
            .iter()
            .flat_map(|f| f.blocks.iter())
            .flat_map(|b| b.instrs.iter())
            .flat_map(|i| i.op.srcs_as_slice())
            .any(|s| matches!(s.src_ref, SrcRef::Imm32(SM53_PROBE_MARKER)))
    }

    pub fn opt_sm53_half_probe(&mut self) {
        for f in &mut self.functions {
            let mut defs = FxHashMap::default();
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    if let Some(op) = rewrite(&defs, &instr.op) {
                        instr.op = op;
                    }
                    record(&mut defs, &instr.op);
                }
            }
        }
    }
}

fn narrowing(op: &OpF2F) -> bool {
    op.src_type == FloatType::F32
        && op.dst_type == FloatType::F16
        && op.rnd_mode == FRndMode::NearestEven
        && !op.integer_rnd
        && op.src.src_swizzle.is_none()
        // legalize copies a constant as a half pair, not as an f32
        && matches!(op.src.src_ref, SrcRef::SSA(_))
        && matches!(
            op.src.src_mod,
            SrcMod::None | SrcMod::FAbs | SrcMod::FNeg | SrcMod::FNegAbs
        )
}

fn f32_source(dst: Dst, x: Src, f32: bool) -> Op {
    OpHAdd2 {
        dst,
        srcs: [x, Src::ZERO.fneg()],
        saturate: false,
        ftz: false,
        f32,
        a_f32: true,
    }
    .into()
}

/// The half a widening of a low lane reads
fn widened_low(op: &OpF2F) -> Option<SSAValue> {
    let low = matches!(op.src.src_swizzle, SrcSwizzle::None | SrcSwizzle::Xx);
    if widening(op) && !op.ftz && low {
        ssa(&op.src)
    } else {
        None
    }
}

impl Shader<'_> {
    /// SM53's F32 swizzle, after copy-prop. HADD2 of an f32 and -0 with a half
    /// result is the f32 rounded to half if the ALU rounds to nearest even,
    /// whether it converts the f32 first or rounds the exact sum; with an f32
    /// result it is the round trip through half only if it converts the f32 to
    /// half first.
    pub fn opt_sm53_f32_source_probe(&mut self) {
        for f in &mut self.functions {
            let mut narrowed: FxHashMap<SSAValue, Src> = FxHashMap::default();
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    let op = match &instr.op {
                        Op::F2F(n) if narrowing(n) => {
                            if let Some(dst) = n.dst.as_ssa() {
                                narrowed.insert(dst[0], n.src.clone());
                            }
                            f32_source(n.dst.clone(), n.src.clone(), false)
                        }
                        Op::F2F(w) => {
                            let Some(x) =
                                widened_low(w).and_then(|h| narrowed.get(&h))
                            else {
                                continue;
                            };
                            f32_source(w.dst.clone(), x.clone(), true)
                        }
                        _ => continue,
                    };
                    instr.op = op;
                }
            }
        }
    }
}
