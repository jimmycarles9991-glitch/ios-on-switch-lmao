// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! Debug-only instruction probes selected by a shader marker. These rewrites
//! deliberately change shader semantics; their outputs distinguish rounding,
//! conversion, and denormal hypotheses rather than establish equivalence.

use crate::ir::*;

use rustc_hash::FxHashMap;

/// hw_test_app/shaders/Scenes.metal SM53_PROBE_MARKER: a signalling NaN's
/// bits, a value no other shader has a use for
const SM53_PROBE_MARKER: u32 = 0x7fa5_5353;
const SM53_PROOF_MARKER: u32 = 0x7fa5_5354;

fn proof_word(half_rewrites: usize, source_rewrites: usize) -> u32 {
    0x5353_0000
        | ((half_rewrites.min(255) as u32) << 8)
        | source_rewrites.min(255) as u32
}

fn stamp_proof(instr: &mut Instr, proof: u32) {
    for src in instr.op.srcs_as_mut_slice() {
        if matches!(src.src_ref, SrcRef::Imm32(SM53_PROOF_MARKER)) {
            src.src_ref = SrcRef::Imm32(proof);
        }
    }
}

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
    if ssa.comps() == 1 {
        Some(ssa[0])
    } else {
        None
    }
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
fn with_f32_result(
    dst: Dst,
    kind: Half,
    srcs: &[Src],
    saturate: bool,
) -> Option<Op> {
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
                && matches!(
                    f.src.src_swizzle,
                    SrcSwizzle::None | SrcSwizzle::Xx
                ) =>
        {
            let Def::Half {
                kind,
                srcs,
                saturate,
            } = defs.get(&ssa(&f.src)?)?
            else {
                return None;
            };
            with_f32_result(f.dst.clone(), *kind, srcs, *saturate)
        }
        Op::FAdd(o) if o.rnd_mode == FRndMode::NearestEven => with_f32_result(
            o.dst.clone(),
            Half::Add,
            &halves(defs, &o.srcs)?,
            o.saturate,
        ),
        Op::FMul(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(
                o.dst.clone(),
                Half::Mul,
                &halves(defs, &o.srcs)?,
                o.saturate,
            )
        }
        Op::FFma(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(
                o.dst.clone(),
                Half::Fma,
                &halves(defs, &o.srcs)?,
                o.saturate,
            )
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
            Def::Half {
                kind: Half::Add,
                srcs: h.srcs.to_vec(),
                saturate: h.saturate,
            },
        ),
        Op::HMul2(h) if !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half {
                kind: Half::Mul,
                srcs: h.srcs.to_vec(),
                saturate: h.saturate,
            },
        ),
        Op::HFma2(h) if !h.f32 && !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half {
                kind: Half::Fma,
                srcs: h.srcs.to_vec(),
                saturate: h.saturate,
            },
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

    pub fn opt_sm53_half_probe(&mut self) -> usize {
        let mut rewritten = 0;
        for f in &mut self.functions {
            let mut defs = FxHashMap::default();
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    if let Some(op) = rewrite(&defs, &instr.op) {
                        instr.op = op;
                        rewritten += 1;
                    }
                    record(&mut defs, &instr.op);
                }
            }
        }
        rewritten
    }

    pub fn stamp_sm53_probe(
        &mut self,
        half_rewrites: usize,
        source_rewrites: usize,
    ) {
        let proof = proof_word(half_rewrites, source_rewrites);
        for f in &mut self.functions {
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    stamp_proof(instr, proof);
                }
            }
        }
    }
}

#[cfg(test)]
mod sm53_half_tests {
    use super::*;
    use compiler::cfg::CFGBuilder;
    use rustc_hash::FxBuildHasher;

    fn shader(sm: &ShaderModelInfo, instrs: Vec<Instr>) -> Shader<'_> {
        let mut labels = LabelAllocator::new();
        let mut cfg = CFGBuilder::<_, _, FxBuildHasher>::new();
        cfg.add_node(
            0,
            BasicBlock {
                label: labels.alloc(),
                uniform: true,
                instrs,
            },
        );
        Shader {
            sm,
            info: ShaderInfo {
                max_warps_per_sm: 0,
                num_gprs: 0,
                num_control_barriers: 0,
                num_instrs: 0,
                num_static_cycles: 0,
                num_spills_to_mem: 0,
                num_fills_from_mem: 0,
                num_spills_to_reg: 0,
                num_fills_from_reg: 0,
                slm_size: 0,
                max_crs_depth: 0,
                uses_global_mem: true,
                writes_global_mem: true,
                uses_fp64: false,
                stage: ShaderStageInfo::Compute(ComputeShaderInfo {
                    local_size: [32, 1, 1],
                    smem_size: 0,
                }),
                io: ShaderIoInfo::None,
            },
            functions: vec![Function {
                ssa_alloc: SSAValueAllocator::new(),
                phi_alloc: PhiAllocator::new(),
                blocks: cfg.as_cfg(true),
            }],
        }
    }

    fn copy(dst: SSAValue, value: u32) -> Instr {
        Instr::new(OpCopy {
            dst: dst.into(),
            src: value.into(),
        })
    }

    fn convert(dst: SSAValue, src: SSAValue, narrow: bool) -> Instr {
        Instr::new(OpF2F {
            dst: dst.into(),
            src: src.into(),
            src_type: if narrow {
                FloatType::F32
            } else {
                FloatType::F16
            },
            dst_type: if narrow {
                FloatType::F16
            } else {
                FloatType::F32
            },
            rnd_mode: FRndMode::NearestEven,
            ftz: false,
            integer_rnd: false,
        })
    }

    #[test]
    fn proof_counts_actual_source_rewrites_and_preserves_other_literals() {
        let sm = ShaderModelInfo::new(53, 64);
        let mut alloc = SSAValueAllocator::new();
        let values: Vec<_> =
            (0..6).map(|_| alloc.alloc(RegFile::GPR)).collect();
        let mut s = shader(
            &sm,
            vec![
                copy(values[0], SM53_PROBE_MARKER),
                copy(values[1], SM53_PROOF_MARKER),
                copy(values[2], 0x1234_5678),
                convert(values[3], values[2], true),
                convert(values[4], values[3], false),
            ],
        );
        assert!(s.carries_sm53_probe_marker());
        let half = s.opt_sm53_half_probe();
        let source = s.opt_sm53_f32_source_probe();
        assert_eq!((half, source), (0, 2));
        s.stamp_sm53_probe(half, source);
        let ops = &s.functions[0].blocks[0].instrs;
        for (i, word) in
            [(0, SM53_PROBE_MARKER), (1, 0x5353_0002), (2, 0x1234_5678)]
        {
            assert!(
                matches!(&ops[i].op, Op::Copy(c) if c.src.src_ref == SrcRef::Imm32(word))
            );
        }
        for (i, f32) in [(3, false), (4, true)] {
            let Op::HAdd2(h) = &ops[i].op else {
                panic!("missing source probe");
            };
            assert!(h.a_f32 && !h.ftz && !h.saturate);
            assert_eq!(h.f32, f32);
            assert_eq!(ssa(&h.srcs[0]), Some(values[2]));
            assert!(h.srcs[0].src_swizzle.is_none());
            assert!(h.srcs[1].src_mod.has_fneg());
        }
    }

    #[test]
    fn half_result_proof_counts_only_eligible_rewrite() {
        let sm = ShaderModelInfo::new(53, 64);
        let mut alloc = SSAValueAllocator::new();
        let v: Vec<_> = (0..6).map(|_| alloc.alloc(RegFile::GPR)).collect();
        let mut s = shader(
            &sm,
            vec![
                Instr::new(OpHAdd2 {
                    dst: v[2].into(),
                    srcs: [v[0].into(), v[1].into()],
                    saturate: false,
                    ftz: false,
                    f32: false,
                    a_f32: false,
                }),
                convert(v[3], v[2], false),
                Instr::new(OpHAdd2 {
                    dst: v[4].into(),
                    srcs: [v[0].into(), v[1].into()],
                    saturate: false,
                    ftz: true,
                    f32: false,
                    a_f32: false,
                }),
                convert(v[5], v[4], false),
            ],
        );
        assert!(!s.carries_sm53_probe_marker());
        assert_eq!(s.opt_sm53_half_probe(), 1);
        let ops = &s.functions[0].blocks[0].instrs;
        assert!(
            matches!(&ops[1].op, Op::HAdd2(h) if h.f32 && !h.a_f32 && !h.ftz)
        );
        assert!(matches!(&ops[3].op, Op::F2F(_)));
        assert_eq!(proof_word(1, 0), 0x5353_0100);
        assert_eq!(proof_word(256, usize::MAX), 0x5353_ffff);
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
    pub fn opt_sm53_f32_source_probe(&mut self) -> usize {
        let mut rewritten = 0;
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
                    rewritten += 1;
                }
            }
        }
        rewritten
    }
}
