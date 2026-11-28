// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! ios-on-switch-lmao: a half widened to f32 for no use but a colour output,
//! where the target stores a half denormal as it stores zero (8- and 10-bit
//! unorm, sRGB and snorm formats: a half denormal is below 2^-14, under half a
//! code of any of them), takes SM53's half ALU, HADD2.F32 of the half and -0,
//! in place of F2F. HADD2 issues at full rate with a fixed latency; F2F is a
//! quarter-rate conversion through the scoreboard. HADD2.F32 gives a half
//! denormal as a zero of its sign (measured on GM20B, as from_nir.rs's
//! fquantize2f16 notes) and every other half exactly.

use crate::ir::*;
use rustc_hash::FxHashMap;

/// The colour target of each register output: four for each target the shader
/// writes, in target order, none for a target it leaves alone (from_nir's
/// copy_fs_outputs_nv); the sample mask and depth follow
fn output_targets(writes_color: u32) -> Vec<u8> {
    (0..8_u8)
        .filter(|t| (writes_color >> (4 * t)) & 0xf != 0)
        .flat_map(|t| [t; 4])
        .collect()
}

impl Function {
    fn opt_unorm_outputs(&mut self, targets: u8, slots: &[u8]) -> bool {
        let mut uses: FxHashMap<SSAValue, u32> = FxHashMap::default();
        let mut widenings: FxHashMap<SSAValue, (usize, usize)> =
            FxHashMap::default();
        let mut outputs = Vec::new();
        for (bi, b) in self.blocks.iter().enumerate() {
            for (ii, instr) in b.instrs.iter().enumerate() {
                for src in instr.srcs() {
                    for ssa in src.iter_ssa() {
                        *uses.entry(*ssa).or_default() += 1;
                    }
                }
                match &instr.op {
                    Op::F2F(op) if is_half_widening(op) => {
                        if let Dst::SSA(dst) = &op.dst {
                            widenings.insert(dst[0], (bi, ii));
                        }
                    }
                    Op::RegOut(out) => {
                        for (src, &target) in out.srcs.iter().zip(slots) {
                            if targets & (1 << target) != 0 {
                                if let Some(ssa) = src.as_ssa() {
                                    outputs.push(ssa[0]);
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
        }

        let mut progress = false;
        for out in outputs {
            if uses.get(&out) != Some(&1) {
                continue;
            }
            let Some(&(bi, ii)) = widenings.get(&out) else {
                continue;
            };
            let instr = &mut self.blocks[bi].instrs[ii];
            let Op::F2F(op) = &instr.op else {
                unreachable!("a widening is an F2F");
            };
            let swizzle = if op.src.src_swizzle == SrcSwizzle::Yy {
                SrcSwizzle::Yy
            } else {
                SrcSwizzle::Xx
            };
            instr.op = OpHAdd2 {
                dst: op.dst.clone(),
                srcs: [op.src.clone().swizzle(swizzle), Src::ZERO.fneg()],
                saturate: false,
                ftz: false,
                f32: true,
                a_f32: false,
            }
            .into();
            progress = true;
        }
        progress
    }
}

/// An F2F of a half in a register to a single f32
fn is_half_widening(op: &OpF2F) -> bool {
    op.src_type == FloatType::F16
        && op.dst_type == FloatType::F32
        && !op.integer_rnd
        && matches!(&op.src.src_ref, SrcRef::SSA(ssa) if ssa.comps() == 1)
        && matches!(&op.dst, Dst::SSA(dst) if dst.comps() == 1)
}

impl Shader<'_> {
    /// `targets`: bit j, colour attachment j stores a half denormal as zero
    pub fn opt_unorm_outputs(&mut self, targets: u8) -> bool {
        if targets == 0 || self.sm.sm() < 53 || self.sm.sm() >= 70 {
            return false;
        }
        let ShaderIoInfo::Fragment(io) = &self.info.io else {
            return false;
        };
        let slots = output_targets(io.writes_color);
        let mut progress = false;
        for f in &mut self.functions {
            progress |= f.opt_unorm_outputs(targets, &slots);
        }
        progress
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use compiler::cfg::CFGBuilder;
    use rustc_hash::FxBuildHasher;

    fn widen(dst: SSAValue, half: Src) -> Instr {
        Instr::new(OpF2F {
            dst: dst.into(),
            src: half,
            src_type: FloatType::F16,
            dst_type: FloatType::F32,
            rnd_mode: FRndMode::NearestEven,
            ftz: true,
            integer_rnd: false,
        })
    }

    fn function(ssa: SSAValueAllocator, instrs: Vec<Instr>) -> Function {
        let mut cfg = CFGBuilder::<_, _, FxBuildHasher>::new();
        cfg.add_node(
            0,
            BasicBlock {
                label: LabelAllocator::new().alloc(),
                uniform: true,
                instrs,
            },
        );
        Function {
            ssa_alloc: ssa,
            phi_alloc: PhiAllocator::new(),
            blocks: cfg.as_cfg(false),
        }
    }

    /// Target 0 (in the mask): its low and high half outputs take HADD2.F32,
    /// a widening something else also reads stays an F2F; target 1 (not in the
    /// mask) keeps its F2F
    #[test]
    fn only_a_widening_a_masked_target_alone_reads_takes_the_half_alu() {
        let sm = ShaderModelInfo::new(53, 64);
        let mut ssa = SSAValueAllocator::new();
        let halves = ssa.alloc(RegFile::GPR);
        let [low, high, shared, other] =
            std::array::from_fn(|_| ssa.alloc(RegFile::GPR));
        let sum = ssa.alloc(RegFile::GPR);
        let mut outputs = vec![Src::ZERO; 8];
        outputs[0] = low.into();
        outputs[1] = high.into();
        outputs[2] = shared.into();
        outputs[4] = other.into();
        outputs[5] = sum.into();
        let instrs = vec![
            Instr::new(OpCopy {
                dst: halves.into(),
                src: 0x4000_3c00.into(),
            }),
            widen(low, halves.into()),
            widen(high, Src::from(halves).swizzle(SrcSwizzle::Yy)),
            widen(shared, halves.into()),
            widen(other, halves.into()),
            Instr::new(OpFAdd {
                dst: sum.into(),
                srcs: [shared.into(), shared.into()],
                saturate: false,
                rnd_mode: FRndMode::NearestEven,
                ftz: false,
            }),
            Instr::new(OpRegOut { srcs: outputs }),
        ];
        let mut f = function(ssa, instrs);
        assert!(
            sm.sm() == 53 && f.opt_unorm_outputs(0b01, &output_targets(0xff))
        );
        let ops: Vec<&Op> = f.blocks[0].instrs.iter().map(|i| &i.op).collect();
        let Op::HAdd2(low_op) = ops[1] else {
            panic!("the low half widened by F2F: {}", ops[1]);
        };
        assert!(low_op.f32 && low_op.srcs[0].src_swizzle == SrcSwizzle::Xx);
        assert!(
            low_op.srcs[1].src_ref == SrcRef::Zero
                && low_op.srcs[1].src_mod.has_fneg()
        );
        let Op::HAdd2(high_op) = ops[2] else {
            panic!("the high half widened by F2F: {}", ops[2]);
        };
        assert!(high_op.f32 && high_op.srcs[0].src_swizzle == SrcSwizzle::Yy);
        assert!(matches!(ops[3], Op::F2F(_)), "a widening read twice taken");
        assert!(
            matches!(ops[4], Op::F2F(_)),
            "an unmasked target's widening taken"
        );
    }

    /// A shader writing target 1 alone gives it reg_out's first four outputs
    #[test]
    fn a_target_s_outputs_follow_the_targets_written() {
        assert_eq!(output_targets(0xf0), [1, 1, 1, 1]);
        assert_eq!(output_targets(0x0f01), [0, 0, 0, 0, 2, 2, 2, 2]);
        let mut ssa = SSAValueAllocator::new();
        let halves = ssa.alloc(RegFile::GPR);
        let wide = ssa.alloc(RegFile::GPR);
        let instrs = vec![
            Instr::new(OpCopy {
                dst: halves.into(),
                src: 0x3c00.into(),
            }),
            widen(wide, halves.into()),
            Instr::new(OpRegOut {
                srcs: vec![wide.into(), Src::ZERO, Src::ZERO, Src::ZERO],
            }),
        ];
        let mut f = function(ssa, instrs);
        assert!(!f.opt_unorm_outputs(0b01, &output_targets(0x30)));
        assert!(matches!(f.blocks[0].instrs[1].op, Op::F2F(_)));
        assert!(f.opt_unorm_outputs(0b10, &output_targets(0x30)));
        assert!(matches!(f.blocks[0].instrs[1].op, Op::HAdd2(_)));
    }
}
