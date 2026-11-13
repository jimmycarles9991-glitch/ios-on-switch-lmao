// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! SM53's HSETP2: two half compares into two predicates. Field layout from
//! Ryujinx's InstDecoders.cs (InstHsetp2R; MIT): the second destination
//! predicate 0..3, the first 3..6, ftz 6, A 8..16, B 20..28, B's swizzle
//! 28..30, |B| 30, -B 31, the comparison 35..39 (FSETP's codes), the
//! accumulator 39..42 and its inversion 42, -A 43, |A| 44, the combining
//! operation 45..47, A's swizzle 47..49, .H_AND 49.

use super::*;

impl SM50Op for OpHSetP2 {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1] = &mut self.srcs;
        if swap_srcs_if_not_reg(src0, src1, GPR) {
            self.cmp_op = self.cmp_op.flip();
        }
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::F16v2);
        b.copy_alu_src_if_not_reg(src1, GPR, SrcType::F16v2);
    }

    fn encode(&self, e: &mut SM50Encoder<'_>) {
        assert!(e.sm.sm() >= 53);
        let [a, b] = &self.srcs;
        e.set_opcode(0x5d20);
        e.set_pred_dst(3..6, &self.dsts[0]);
        e.set_pred_dst(0..3, &self.dsts[1]);
        e.set_bit(6, self.ftz);
        e.set_reg_src_ref(8..16, &a.src_ref);
        e.set_reg_src_ref(20..28, &b.src_ref);
        e.set_field(28..30, sm53_half_swizzle(b));
        e.set_bit(30, b.src_mod.has_fabs());
        e.set_bit(31, b.src_mod.has_fneg());
        e.set_float_cmp_op(35..39, self.cmp_op);
        e.set_pred_src(39..42, 42, &self.accum);
        e.set_bit(43, a.src_mod.has_fneg());
        e.set_bit(44, a.src_mod.has_fabs());
        e.set_pred_set_op(45..47, self.set_op);
        e.set_field(47..49, sm53_half_swizzle(a));
        e.set_bit(49, self.horizontal);
    }
}

#[cfg(test)]
mod sm53_hsetp2_tests {
    use super::*;

    fn bits(w: u64, at: u32, n: u32) -> u64 {
        (w >> at) & ((1 << n) - 1)
    }

    #[test]
    fn fields() {
        let reg = |i| RegRef::new(RegFile::GPR, i, 1);
        let src = |i, m, s| Src {
            src_ref: SrcRef::Reg(reg(i)),
            src_mod: m,
            src_swizzle: s,
        };
        let op = OpHSetP2 {
            dsts: [RegRef::new(RegFile::Pred, 2, 1).into(), Dst::None],
            set_op: PredSetOp::And,
            cmp_op: FloatCmpOp::OrdLt,
            srcs: [
                src(1, SrcMod::FNeg, SrcSwizzle::Yy),
                src(4, SrcMod::FAbs, SrcSwizzle::Xx),
            ],
            accum: SrcRef::True.into(),
            ftz: false,
            horizontal: false,
        };
        let sm = ShaderModel50::new(53);
        let labels = FxHashMap::default();
        let (mut ip, mut sched) = (0, [0_u32; 2]);
        let w = encode_instr(
            0,
            Some(&Instr::new(op)),
            &sm,
            &labels,
            &mut ip,
            &mut sched,
        );
        let w = u64::from(w[0]) | (u64::from(w[1]) << 32);
        // maxwell.inc: HSETP2 (reg) "0101 1101 0010 0---"
        assert_eq!(bits(w, 51, 13), 0b0101_1101_0010_0);
        assert_eq!((bits(w, 3, 3), bits(w, 0, 3)), (2, 7));
        assert_eq!((bits(w, 8, 8), bits(w, 20, 8)), (1, 4));
        assert_eq!((bits(w, 47, 2), bits(w, 28, 2)), (3, 2)); // H1H1, H0H0
        assert_eq!((bits(w, 43, 1), bits(w, 44, 1)), (1, 0));
        assert_eq!((bits(w, 30, 1), bits(w, 31, 1)), (1, 0));
        assert_eq!((bits(w, 35, 4), bits(w, 39, 3), bits(w, 45, 2)), (1, 7, 0));
    }
}
