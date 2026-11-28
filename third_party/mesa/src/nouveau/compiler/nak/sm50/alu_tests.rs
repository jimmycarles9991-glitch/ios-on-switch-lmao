// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! SM50 immediates: what legalize leaves for the encoder and the fields the
//! encoder writes (maxwell.inc: FADD32I has no .SAT; F2F's imm20 is a float's
//! top bits, as nv50_ir's emitF2F writes it), and the register copies of
//! 32-bit immediates legalize shares within a block.

use super::*;
use crate::const_tracker::ConstTracker;
use crate::legalize::HeldImms;
use crate::ssa_value::SSAValueAllocator;

fn bits(w: u64, at: u32, n: u32) -> u64 {
    (w >> at) & ((1 << n) - 1)
}

fn encode(op: impl Into<Op>) -> u64 {
    let sm = ShaderModel50::new(53);
    let labels = FxHashMap::default();
    let (mut ip, mut sched) = (0, [0_u32; 2]);
    let i = Instr::new(op);
    let w = encode_instr(0, Some(&i), &sm, &labels, &mut ip, &mut sched);
    u64::from(w[0]) | (u64::from(w[1]) << 32)
}

fn legalized_fadd(saturate: bool) -> OpFAdd {
    let sm = ShaderModelInfo::new(53, 64);
    let mut alloc = SSAValueAllocator::new();
    let mut consts = ConstTracker::new();
    let x = alloc.alloc(RegFile::GPR);
    let d = alloc.alloc(RegFile::GPR);
    let mut op = OpFAdd {
        dst: d.into(),
        srcs: [x.into(), 0x3f80_0001_u32.into()],
        saturate,
        rnd_mode: FRndMode::NearestEven,
        ftz: false,
    };
    let mut b = LegalizeBuilder::new(&sm, &mut alloc, &mut consts);
    op.legalize(&mut b);
    op
}

fn legalized<T: SM50Op>(mut op: T, alloc: &mut SSAValueAllocator) -> T {
    let sm = ShaderModelInfo::new(53, 64);
    let mut consts = ConstTracker::new();
    let mut b = LegalizeBuilder::new(&sm, alloc, &mut consts);
    op.legalize(&mut b);
    op
}

/// IADD's immediate forms have no negation bit for the immediate, so none is
/// left negated for the encoder, with a carry out or without
#[test]
fn iadd_keeps_no_negated_immediate() {
    for value in [5_u32, 0x1234_5678] {
        for carry in [false, true] {
            let mut alloc = SSAValueAllocator::new();
            let x = alloc.alloc(RegFile::GPR);
            let d = alloc.alloc(RegFile::GPR);
            let op = legalized(
                OpIAdd2 {
                    dst: d.into(),
                    carry_out: if carry {
                        RegRef::new(RegFile::Carry, 0, 1).into()
                    } else {
                        Dst::None
                    },
                    srcs: [x.into(), Src::from(value).ineg()],
                },
                &mut alloc,
            );
            let src1 = &op.srcs[1];
            assert!(
                !(matches!(src1.src_ref, SrcRef::Imm32(_))
                    && src1.src_mod.is_ineg()),
                "a negated immediate {value:#x} left for IADD (carry {carry})"
            );
            if !carry {
                assert!(
                    src1.src_ref == SrcRef::Imm32(value.wrapping_neg())
                        && src1.src_mod.is_none()
                );
            }
        }
    }
}

/// LOP encodes only an inversion of a source: an integer or float negation or
/// an absolute value on one goes before the encoder sees it
#[test]
fn lop_keeps_no_arithmetic_modifier() {
    for modify in [Src::ineg, Src::fneg, Src::fabs] {
        let mut alloc = SSAValueAllocator::new();
        let [x, y, d] = std::array::from_fn(|_| alloc.alloc(RegFile::GPR));
        let op = legalized(
            OpLop2 {
                dst: d.into(),
                srcs: [x.into(), modify(Src::from(y))],
                op: LogicOp2::And,
            },
            &mut alloc,
        );
        for src in &op.srcs {
            assert!(src.src_mod.is_none() || src.src_mod.is_bnot(), "{src}");
        }
    }
}

#[test]
fn saturated_fadd_takes_no_long_immediate() {
    assert!(legalized_fadd(true).srcs[1].as_ssa().is_some());
    assert!(legalized_fadd(false).srcs[1].as_imm_not_f20().is_some());
}

#[test]
fn f2f_immediates_are_float_bits() {
    for (imm, field, sign) in
        [(0x3f80_0000_u32, 0x3f800, 0), (0xc000_0000, 0x40000, 1)]
    {
        let w = encode(OpF2F {
            dst: RegRef::new(RegFile::GPR, 2, 1).into(),
            src: imm.into(),
            src_type: FloatType::F32,
            dst_type: FloatType::F16,
            rnd_mode: FRndMode::NearestEven,
            ftz: false,
            integer_rnd: false,
        });
        assert_eq!(bits(w, 48, 16) & 0xfef8, 0x38a8);
        assert_eq!((bits(w, 20, 19), bits(w, 56, 1)), (field, sign));
    }
}

/// Two SELs of 32-bit immediates in one block, the second `gap` instructions
/// after the first: the copies legalize put before them and their sources
/// RRO needs no scoreboard; the MUFU it feeds still does
#[test]
fn rro_is_fixed_latency() {
    let dst: Dst = RegRef::new(RegFile::GPR, 1, 1).into();
    let rro = Op::Rro(Box::new(OpRro {
        dst: dst.clone(),
        op: RroOp::Exp2,
        src: Src::ZERO,
    }));
    let mufu = Op::MuFu(Box::new(OpMuFu {
        dst,
        op: MuFuOp::Exp2,
        src: Src::ZERO,
        op_type: FloatType::F32,
    }));
    assert!(rro.has_fixed_latency(53) && !mufu.has_fixed_latency(53));
}

fn sels_of(imms: [u32; 2], gap: usize) -> (usize, [Src; 2]) {
    let sm = ShaderModelInfo::new(53, 64);
    let mut alloc = SSAValueAllocator::new();
    let mut consts = ConstTracker::new();
    let mut held = HeldImms::new();
    held.start_block();
    let mut copies = 0;
    let mut read = Vec::new();
    for (imm, ip) in imms.into_iter().zip([0, gap]) {
        held.set_ip(ip);
        let x = alloc.alloc(RegFile::GPR);
        let mut op = OpSel {
            dst: alloc.alloc(RegFile::GPR).into(),
            cond: alloc.alloc(RegFile::Pred).into(),
            srcs: [x.into(), imm.into()],
        };
        let mut b = LegalizeBuilder::sharing_imms(
            &sm,
            &mut alloc,
            &mut consts,
            &mut held,
        );
        op.legalize(&mut b);
        copies += b.into_vec().len();
        read.push(op.srcs[1].clone());
    }
    let second = read.pop().unwrap();
    (copies, [read.pop().unwrap(), second])
}

#[test]
fn a_nearby_immediate_reads_the_earlier_copy() {
    let (copies, [first, second]) =
        sels_of([0x3f80_0000; 2], HeldImms::REACH);
    assert_eq!(copies, 1);
    assert!(first.as_ssa().is_some() && first.src_ref == second.src_ref);
}

#[test]
fn a_far_or_different_immediate_gets_its_own_copy() {
    for (imms, gap) in [
        ([0x3f80_0000; 2], HeldImms::REACH + 1),
        ([0x3f80_0000, 0x7f80_0000], 1),
    ] {
        let (copies, [first, second]) = sels_of(imms, gap);
        assert_eq!(copies, 2);
        assert!(first.src_ref != second.src_ref);
    }
}

fn gpr(i: u32) -> Dst {
    RegRef::new(RegFile::GPR, i, 1).into()
}

fn ffma(dst: u32) -> Op {
    Op::FFma(Box::new(OpFFma {
        dst: gpr(dst),
        srcs: [Src::ZERO, Src::ZERO, Src::ZERO],
        saturate: false,
        rnd_mode: FRndMode::NearestEven,
        ftz: false,
        dnz: false,
    }))
}

fn mufu(op: MuFuOp) -> Op {
    Op::MuFu(Box::new(OpMuFu {
        dst: gpr(2),
        op,
        src: Src::ZERO,
        op_type: FloatType::F32,
    }))
}

fn ldc() -> Op {
    Op::Ldc(Box::new(OpLdc {
        dst: gpr(2),
        cb: SrcRef::CBuf(CBufRef {
            buf: CBuf::Binding(1),
            offset: 0,
        })
        .into(),
        offset: Src::ZERO,
        mode: LdcMode::Indexed,
        mem_type: MemType::B32,
    }))
}

/// A fixed-latency GPR write read by MUFU issues 2 cycles before the read,
/// by LDC 4 and by FFMA 6; an XMAD write keeps 6 for every reader, and MUFU's
/// own result its estimate
#[test]
fn a_variable_latency_reader_collects_its_operand_after_issue() {
    let sm = ShaderModel50::new(53);
    let w = ffma(1);
    assert_eq!(sm.raw_latency(&w, 0, &mufu(MuFuOp::Rcp), 0), 2);
    assert_eq!(sm.raw_latency(&w, 0, &ldc(), 0), 4);
    assert_eq!(sm.raw_latency(&w, 0, &ffma(3), 0), 6);
    let xmad = Op::Xmad(Box::new(OpXmad {
        dst: gpr(1),
        srcs: [Src::ZERO, Src::ZERO, Src::ZERO],
        hi: [false; 2],
        psl: false,
        mrg: false,
        cbcc: false,
    }));
    assert_eq!(sm.raw_latency(&xmad, 0, &mufu(MuFuOp::Rcp), 0), 6);
    assert_eq!(sm.raw_latency(&xmad, 0, &ldc(), 0), 6);
    let m = mufu(MuFuOp::Rcp);
    assert_eq!(
        sm.raw_latency(&m, 0, &mufu(MuFuOp::Rsq), 0),
        instr_latency(53, &m, 0)
    );
}
