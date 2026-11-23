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
