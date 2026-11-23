// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! Maxwell's three-source integer forms: LOP3.LUT, IADD3 and ISCADD (NAK's
//! OpLea of 32 bits). Field layouts are Ryujinx's InstDecoders.cs (InstLop3R/
//! I/C, InstIadd3R/I/C, InstIscaddR/I/C; MIT), which decodes NVIDIA's shipped
//! Maxwell code; nv50_ir's emitISCADD agrees on ISCADD.

use super::*;

/// A LUT's bits for a source that is a constant or inverted, and the source
/// left a plain register or immediate.
fn fold_lop_src(src: &mut Src, op: &mut LogicOp3, idx: usize) {
    if src.src_mod.is_bnot() {
        op.invert_src(idx);
        src.src_mod = SrcMod::None;
    }
    let constant = match src.src_ref {
        SrcRef::Zero => Some(false),
        SrcRef::Imm32(0) => Some(false),
        SrcRef::Imm32(u32::MAX) => Some(true),
        _ => None,
    };
    if let Some(value) = constant {
        op.fix_src(idx, value);
        src.src_ref = SrcRef::Zero;
    }
}

/// LOP32I's operation and source inversions that compute `op`, where `op`
/// ignores its third source: LOP3 has no 32-bit immediate form.
fn lop32i_form(op: LogicOp3) -> Option<(u8, bool, bool)> {
    if op.src_used(2) {
        return None;
    }
    let [a, b, _] = LogicOp3::SRC_MASKS;
    for (code, f) in [
        (0_u8, (|x, y| x & y) as fn(u8, u8) -> u8),
        (1, |x, y| x | y),
        (2, |x, y| x ^ y),
    ] {
        for (inv_a, inv_b) in [(false, false), (true, false), (false, true)] {
            let x = if inv_a { !a } else { a };
            let y = if inv_b { !b } else { b };
            if f(x, y) == op.lut {
                return Some((code, inv_a, inv_b));
            }
        }
    }
    None
}

impl SM50Op for OpLop3 {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        for (idx, src) in self.srcs.iter_mut().enumerate() {
            fold_lop_src(src, &mut self.op, idx);
        }
        let [src0, src1, src2] = &mut self.srcs;
        if !src_is_reg(src0, GPR) && src_is_reg(src1, GPR) {
            std::mem::swap(src0, src1);
            let op = self.op;
            self.op = LogicOp3::new_lut(&|x, y, z| op.eval(y, x, z));
        }
        if !src_is_reg(src2, GPR) && src_is_reg(src1, GPR) {
            std::mem::swap(src2, src1);
            let op = self.op;
            self.op = LogicOp3::new_lut(&|x, y, z| op.eval(x, z, y));
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        b.copy_alu_src_if_not_reg(src2, GPR, SrcType::ALU);
        if lop32i_form(self.op).is_none() {
            b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::ALU);
        }
    }

    fn encode(&self, e: &mut SM50Encoder<'_>) {
        if let Some(imm32) = self.srcs[1].as_imm_not_i20() {
            let (code, inv_a, inv_b) =
                lop32i_form(self.op).expect("LOP3 with a 32-bit immediate");
            e.set_opcode(0x0400);
            e.set_dst(&self.dst);
            e.set_reg_src(8..16, &self.srcs[0]);
            e.set_bit(55, inv_a);
            e.set_src_imm32(20..52, imm32);
            e.set_field(53..55, code);
            e.set_bit(56, inv_b);
            return;
        }
        match &self.srcs[1].src_ref {
            SrcRef::Zero | SrcRef::Reg(_) => {
                e.set_opcode(0x5be0);
                e.set_reg_src(20..28, &self.srcs[1]);
                e.set_field(28..36, self.op.lut);
                e.set_field(36..38, 0_u8); // no predicate result
                e.set_bit(38, false); // .X
                e.set_pred_dst(48..51, &Dst::None);
            }
            SrcRef::Imm32(imm32) => {
                e.set_opcode(0x3c00);
                e.set_src_imm_i20(20..39, 56, *imm32);
                e.set_field(48..56, self.op.lut);
                e.set_bit(57, false); // .X
            }
            SrcRef::CBuf(cb) => {
                e.set_opcode(0x0200);
                e.set_src_cb(20..39, cb);
                e.set_field(48..56, self.op.lut);
                e.set_bit(56, false); // .X
            }
            src => panic!("Invalid lop3 src1: {src}"),
        }
        e.set_dst(&self.dst);
        e.set_reg_src(8..16, &self.srcs[0]);
        e.set_reg_src(39..47, &self.srcs[2]);
        e.set_bit(47, false); // .CC
    }
}

impl SM50Op for OpIAdd3 {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        assert!(self.overflow[0].is_none() && self.overflow[1].is_none());
        let [src0, src1, src2] = &mut self.srcs;
        swap_srcs_if_not_reg(src0, src1, GPR);
        swap_srcs_if_not_reg(src2, src1, GPR);
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::I32);
        b.copy_alu_src_if_not_reg(src2, GPR, SrcType::I32);
        // GM20B traps (ILLEGAL_INSTR_ENCODING) on the constant-buffer form with
        // A and B negated. IADD gives that pair of bits to .PO and Volta's
        // IADD3 refuses it, so A and B are never both negated and a constant
        // buffer is never negated.
        if matches!(src1.src_ref, SrcRef::CBuf(_)) && src1.src_mod.is_ineg() {
            b.copy_alu_src(src1, GPR, SrcType::I32);
        }
        b.copy_alu_src_if_ineg_imm(src1, GPR, SrcType::I32);
        if src0.src_mod.is_ineg() && src1.src_mod.is_ineg() {
            if src2.src_mod.is_ineg() {
                b.copy_alu_src_and_lower_ineg(src1, GPR, SrcType::I32);
            } else {
                std::mem::swap(src1, src2);
            }
        }
        b.copy_alu_src_if_i20_overflow(src1, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM50Encoder<'_>) {
        assert!(self.srcs[0].is_unmodified() || self.srcs[1].is_unmodified());
        match &self.srcs[1].src_ref {
            SrcRef::Zero | SrcRef::Reg(_) => {
                e.set_opcode(0x5cc0);
                e.set_reg_src_ref(20..28, &self.srcs[1].src_ref);
                e.set_field(31..39, 0_u8); // parts, no shift
            }
            SrcRef::Imm32(imm32) => {
                assert!(self.srcs[1].is_unmodified());
                e.set_opcode(0x38c0);
                e.set_src_imm_i20(20..39, 56, *imm32);
            }
            SrcRef::CBuf(cb) => {
                assert!(self.srcs[1].is_unmodified());
                e.set_opcode(0x4cc0);
                e.set_src_cb(20..39, cb);
            }
            src => panic!("Invalid iadd3 src1: {src}"),
        }
        e.set_dst(&self.dst);
        e.set_reg_src_ref(8..16, &self.srcs[0].src_ref);
        e.set_reg_src_ref(39..47, &self.srcs[2].src_ref);
        e.set_bit(47, false); // .CC
        e.set_bit(48, false); // .X
        e.set_bit(49, self.srcs[2].src_mod.is_ineg());
        e.set_bit(50, self.srcs[1].src_mod.is_ineg());
        e.set_bit(51, self.srcs[0].src_mod.is_ineg());
    }
}

impl SM50Op for OpLea {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        assert!(self.overflow.is_none() && !self.dst_high);
        assert!(self.a_high.is_zero());
        assert!(self.intermediate_mod == SrcMod::None);
        if !self.a.src_mod.is_none() && !self.b.src_mod.is_none() {
            b.copy_alu_src_and_lower_ineg(&mut self.b, GPR, SrcType::I32);
        }
        b.copy_alu_src_if_not_reg(&mut self.a, GPR, SrcType::ALU);
        b.copy_alu_src_if_ineg_imm(&mut self.b, GPR, SrcType::I32);
        b.copy_alu_src_if_i20_overflow(&mut self.b, GPR, SrcType::I32);
    }

    fn encode(&self, e: &mut SM50Encoder<'_>) {
        match &self.b.src_ref {
            SrcRef::Zero | SrcRef::Reg(_) => {
                e.set_opcode(0x5c18);
                e.set_reg_src_ref(20..28, &self.b.src_ref);
            }
            SrcRef::Imm32(imm32) => {
                e.set_opcode(0x3818);
                e.set_src_imm_i20(20..39, 56, *imm32);
            }
            SrcRef::CBuf(cb) => {
                e.set_opcode(0x4c18);
                e.set_src_cb(20..39, cb);
            }
            src => panic!("Invalid iscadd src b: {src}"),
        }
        e.set_dst(&self.dst);
        e.set_reg_src_ref(8..16, &self.a.src_ref);
        e.set_field(39..44, self.shift);
        e.set_bit(47, false); // .CC
                              // AvgMode: 1 negates B, 2 negates A, 3 adds one (not both)
        e.set_bit(48, self.b.src_mod.is_ineg());
        e.set_bit(49, self.a.src_mod.is_ineg());
    }
}

#[cfg(test)]
mod sm53_int3_tests {
    use super::*;

    fn bits(w: u64, at: u32, n: u32) -> u64 {
        (w >> at) & ((1 << n) - 1)
    }

    fn gpr(i: u32) -> Src {
        SrcRef::Reg(RegRef::new(RegFile::GPR, i, 1)).into()
    }

    fn dst() -> Dst {
        RegRef::new(RegFile::GPR, 4, 1).into()
    }

    fn encode(op: impl Into<Op>) -> u64 {
        let sm = ShaderModel50::new(53);
        let labels = FxHashMap::default();
        let (mut ip, mut sched) = (0, [0_u32; 2]);
        let i = Instr::new(op);
        let w = encode_instr(0, Some(&i), &sm, &labels, &mut ip, &mut sched);
        u64::from(w[0]) | (u64::from(w[1]) << 32)
    }

    /// maxwell.inc: LOP3 (reg) "0101 1011 1110 0---", (imm) "0011 11--",
    /// IADD3 (reg) "0101 1100 1100", (imm) "0011 100- 1100",
    /// ISCADD (reg) "0101 1100 0001 1---", (imm) "0011 100- 0001 1---"
    #[test]
    fn opcodes_and_fields() {
        let lut = LogicOp3 { lut: 0x96 };
        let w = encode(OpLop3 {
            dst: dst(),
            srcs: [gpr(1), gpr(2), gpr(3)],
            op: lut,
        });
        assert_eq!(bits(w, 51, 13), 0b0101_1011_1110_0);
        assert_eq!((bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8)), (4, 1, 2));
        assert_eq!(
            (bits(w, 39, 8), bits(w, 28, 8), bits(w, 36, 2)),
            (3, 0x96, 0)
        );
        let w = encode(OpLop3 {
            dst: dst(),
            srcs: [gpr(1), 0x7fff_u32.into(), gpr(3)],
            op: lut,
        });
        assert_eq!(bits(w, 58, 6), 0b0011_11);
        assert_eq!((bits(w, 20, 19), bits(w, 48, 8)), (0x7fff, 0x96));

        let w = encode(OpIAdd3 {
            dst: dst(),
            overflow: [Dst::None, Dst::None],
            srcs: [gpr(1), Src::from(gpr(2)).ineg(), gpr(3)],
        });
        assert_eq!(bits(w, 52, 12), 0b0101_1100_1100);
        assert_eq!((bits(w, 51, 1), bits(w, 50, 1), bits(w, 49, 1)), (0, 1, 0));
        assert_eq!((bits(w, 39, 8), bits(w, 31, 8)), (3, 0));

        let w = encode(OpLea {
            dst: dst(),
            overflow: Dst::None,
            a: gpr(1),
            b: 0x40_u32.into(),
            a_high: 0.into(),
            shift: 3,
            dst_high: false,
            intermediate_mod: SrcMod::None,
        });
        assert_eq!(
            (bits(w, 57, 7), bits(w, 48, 8) >> 3),
            (0b0011_100, 0b0_0011)
        );
        assert_eq!(
            (bits(w, 8, 8), bits(w, 20, 19), bits(w, 39, 5)),
            (1, 0x40, 3)
        );
    }

    #[test]
    fn lop32i_covers_two_source_luts() {
        let [a, b, _] = LogicOp3::SRC_MASKS;
        assert_eq!(
            lop32i_form(LogicOp3 { lut: a & b }),
            Some((0, false, false))
        );
        assert_eq!(
            lop32i_form(LogicOp3 { lut: !a | b }),
            Some((1, true, false))
        );
        assert_eq!(
            lop32i_form(LogicOp3 { lut: a ^ !b }),
            Some((2, true, false))
        );
        assert_eq!(lop32i_form(LogicOp3 { lut: 0x96 }), None);
    }

    fn cbuf(offset: u16) -> Src {
        SrcRef::CBuf(CBufRef {
            buf: CBuf::Binding(2),
            offset,
        })
        .into()
    }

    /// The op legalize leaves for srcs over three SSA values, and how many
    /// instructions it put before it
    fn legalized_iadd3(
        srcs: impl FnOnce(Src, Src, Src) -> [Src; 3],
    ) -> (OpIAdd3, usize) {
        use crate::const_tracker::ConstTracker;
        use crate::ssa_value::SSAValueAllocator;
        let sm = ShaderModelInfo::new(53, 64);
        let mut alloc = SSAValueAllocator::new();
        let mut consts = ConstTracker::new();
        let [x, y, z] = [(); 3].map(|_| alloc.alloc(RegFile::GPR));
        let mut op = OpIAdd3 {
            dst: alloc.alloc(RegFile::GPR).into(),
            overflow: [Dst::None, Dst::None],
            srcs: srcs(x.into(), y.into(), z.into()),
        };
        let mut b = LegalizeBuilder::new(&sm, &mut alloc, &mut consts);
        op.legalize(&mut b);
        (op, b.into_vec().len())
    }

    fn negs(op: &OpIAdd3) -> [bool; 3] {
        [0, 1, 2].map(|i| op.srcs[i].src_mod.is_ineg())
    }

    /// GM20B traps on 4ccc000800270707: IADD3 R7, -R7, -c[0x2][0x8], R0
    #[test]
    fn a_negated_cbuf_is_read_into_a_register() {
        for (op, copies) in [
            legalized_iadd3(|x, _, z| [x.ineg(), cbuf(8).ineg(), z]),
            legalized_iadd3(|x, _, z| [cbuf(8).ineg(), x.ineg(), z]),
        ] {
            assert!(op.srcs[1].as_ssa().is_some());
            assert_eq!(negs(&op), [true, false, true]);
            assert_eq!(copies, 1);
        }
    }

    #[test]
    fn negated_a_and_b_move_b_to_c() {
        let (op, copies) = legalized_iadd3(|x, y, z| [x.ineg(), y.ineg(), z]);
        assert_eq!(negs(&op), [true, false, true]);
        assert_eq!(copies, 0);
        let (op, copies) =
            legalized_iadd3(|x, y, z| [x.ineg(), y.ineg(), z.ineg()]);
        assert_eq!(negs(&op), [true, false, true]);
        assert_eq!(copies, 1);
    }

    #[test]
    fn an_unnegated_cbuf_stays_an_operand() {
        let (op, copies) = legalized_iadd3(|x, _, z| [x.ineg(), cbuf(8), z]);
        assert!(matches!(op.srcs[1].src_ref, SrcRef::CBuf(_)));
        assert_eq!(copies, 0);
    }

    #[test]
    #[should_panic]
    fn the_cbuf_form_never_encodes_a_negated_cbuf() {
        encode(OpIAdd3 {
            dst: dst(),
            overflow: [Dst::None, Dst::None],
            srcs: [gpr(7), cbuf(8).ineg(), gpr(0)],
        });
    }

    #[test]
    #[should_panic]
    fn a_and_b_are_never_encoded_both_negated() {
        encode(OpIAdd3 {
            dst: dst(),
            overflow: [Dst::None, Dst::None],
            srcs: [gpr(5).ineg(), gpr(5).ineg(), gpr(0)],
        });
    }
}
