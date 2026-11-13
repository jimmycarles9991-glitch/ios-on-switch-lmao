// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! Integer multiplication on SM50 from XMAD, Maxwell's 16x16-bit
//! multiply-add, instead of IMUL, which issues at 1/130 of FFMA's rate on
//! GM20B. The 32-bit product is NVIDIA's three-XMAD sequence (nv50_ir's
//! LateAlgebraicOpt::handleMULMAD, MIT); the 64-bit product and the high
//! word add the four 16-bit partial products with plain XMADs, so only the
//! XMAD forms that sequence uses are emitted. Encodings: nv50_ir's
//! emitXMAD (MIT).

use super::*;
use crate::builder::SSABuilder;

impl SM50Op for OpXmad {
    fn legalize(&mut self, b: &mut LegalizeBuilder) {
        use RegFile::GPR;
        let [src0, src1, src2] = &mut self.srcs;
        b.copy_alu_src_if_not_reg(src0, GPR, SrcType::ALU);
        // Without .MRG or .CBCC only the selected half of an immediate counts
        if let SrcRef::Imm32(imm) = src1.src_ref {
            if !self.mrg && !self.cbcc && src1.is_unmodified() {
                let half = if self.hi[1] { imm >> 16 } else { imm & 0xffff };
                src1.src_ref = SrcRef::Imm32(half);
                self.hi[1] = false;
            }
        }
        let short_imm = matches!(src1.src_ref, SrcRef::Imm32(imm)
            if imm <= 0xffff && !self.hi[1]);
        if !short_imm {
            b.copy_alu_src_if_not_reg(src1, GPR, SrcType::ALU);
        }
        b.copy_alu_src_if_not_reg(src2, GPR, SrcType::ALU);
    }

    fn encode(&self, e: &mut SM50Encoder<'_>) {
        match &self.srcs[1].src_ref {
            SrcRef::Imm32(imm) => {
                assert!(*imm <= 0xffff && !self.hi[1]);
                e.set_opcode(0x3600);
                e.set_field(20..36, *imm);
            }
            SrcRef::Zero | SrcRef::Reg(_) => {
                e.set_opcode(0x5b00);
                e.set_reg_src(20..28, &self.srcs[1]);
                e.set_bit(35, self.hi[1]);
            }
            src => panic!("Invalid xmad src1: {src}"),
        }

        e.set_dst(&self.dst);
        e.set_reg_src(8..16, &self.srcs[0]);
        e.set_reg_src(39..47, &self.srcs[2]);
        e.set_bit(36, self.psl);
        e.set_bit(37, self.mrg);
        e.set_bit(38, false); // .X
        e.set_bit(47, false); // .CC
        e.set_bit(48, false); // src0 signed
        e.set_bit(49, false); // src1 signed
        e.set_field(50..53, if self.cbcc { 4_u8 } else { 0 });
        e.set_bit(53, self.hi[0]);
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Form {
    hi: [bool; 2],
    psl: bool,
    mrg: bool,
    cbcc: bool,
}

const fn form(hi: [bool; 2], psl: bool, mrg: bool, cbcc: bool) -> Form {
    Form { hi, psl, mrg, cbcc }
}

const LO: Form = form([false, false], false, false, false);
const A_HI: Form = form([true, false], false, false, false);
const B_HI: Form = form([false, true], false, false, false);
const BOTH_HI: Form = form([true, true], false, false, false);
const PSL: Form = form([false, false], true, false, false);
const PSL_A_HI: Form = form([true, false], true, false, false);
// NVIDIA's 32-bit product: lo = y.l*x.l; merged = (y.l*x.h & 0xffff) |
// x.l << 16; y.h*merged.h << 16 + lo + merged << 16
const MERGE: Form = form([false, true], false, true, false);
const MERGED_SUM: Form = form([true, true], true, false, true);

fn xmad<B: SSABuilder + ?Sized>(
    b: &mut B,
    a: Src,
    x: Src,
    c: Src,
    form: Form,
) -> SSAValue {
    let dst = b.alloc_ssa(RegFile::GPR);
    b.push_op(OpXmad {
        dst: dst.into(),
        srcs: [a, x, c],
        hi: form.hi,
        psl: form.psl,
        mrg: form.mrg,
        cbcc: form.cbcc,
    });
    dst
}

/// The low 32 bits of x * y.
pub fn imul<B: SSABuilder + ?Sized>(b: &mut B, x: Src, y: Src) -> SSAValue {
    if let Some(k) = y.as_u32(SrcType::ALU) {
        return imul_imm(b, x, k);
    }
    if let Some(k) = x.as_u32(SrcType::ALU) {
        return imul_imm(b, y, k);
    }
    let lo = xmad(b, y.clone(), x.clone(), 0.into(), LO);
    let merged = xmad(b, y.clone(), x, 0.into(), MERGE);
    xmad(b, y, merged.into(), lo.into(), MERGED_SUM)
}

/// The low 32 bits of x * k.
pub fn imul_imm<B: SSABuilder + ?Sized>(b: &mut B, x: Src, k: u32) -> SSAValue {
    if k == 0 {
        return b.copy(0.into());
    }
    if k.is_power_of_two() {
        return b.shl(x, k.trailing_zeros().into());
    }
    let (k_lo, k_hi) = (k & 0xffff, k >> 16);
    if k_lo == 0 {
        return xmad(b, x, k_hi.into(), 0.into(), PSL);
    }
    let lo = xmad(b, x.clone(), k_lo.into(), 0.into(), LO);
    let low_product = xmad(b, x.clone(), k_lo.into(), lo.into(), PSL_A_HI);
    if k_hi == 0 {
        return low_product;
    }
    xmad(b, x, k_hi.into(), low_product.into(), PSL)
}

/// x * y as two 32-bit words, low first. With a = ah:al and b = bh:bl in
/// 16-bit halves, every partial sum below fits 32 bits:
/// t = ah*bl + (al*bl >> 16), u = al*bh + (t & 0xffff),
/// v = ah*bh + (t >> 16); high = v + (u >> 16), low = al*bl's low half
/// under u's.
pub fn imul_2x32_64<B: SSABuilder + ?Sized>(
    b: &mut B,
    x: Src,
    y: Src,
    signed: bool,
) -> SSARef {
    let k = y.as_u32(SrcType::ALU);
    let (x, y, k) = match (k, x.as_u32(SrcType::ALU)) {
        (None, Some(kx)) => (y, x, Some(kx)),
        _ => (x, y, k),
    };
    let [lo, hi_unsigned] = match k {
        Some(k) if k >> 16 == 0 => umul_2x32_64_short(b, x.clone(), k),
        _ => umul_2x32_64(b, x.clone(), y.clone()),
    };
    if !signed {
        return [lo, hi_unsigned].into();
    }
    // The signed product's high word: the unsigned one less y where x is
    // negative and less x where y is.
    let x_sign = b.shr(x.clone(), 31.into(), true);
    let less_y = b.lop2(LogicOp2::And, x_sign.into(), y.clone());
    let mut high =
        b.iadd(hi_unsigned.into(), Src::from(less_y).ineg(), 0.into());
    let less_x = match k {
        Some(k) if k >> 31 == 0 => None,
        Some(_) => Some(x),
        None => {
            let y_sign = b.shr(y, 31.into(), true);
            Some(b.lop2(LogicOp2::And, y_sign.into(), x).into())
        }
    };
    if let Some(less_x) = less_x {
        high = b.iadd(high.into(), less_x.ineg(), 0.into());
    }
    [lo, high].into()
}

fn umul_2x32_64_short<B: SSABuilder + ?Sized>(
    b: &mut B,
    x: Src,
    k: u32,
) -> [SSAValue; 2] {
    let ll = xmad(b, x.clone(), k.into(), 0.into(), LO);
    let ll_hi = b.shr(ll.into(), 16.into(), false);
    let t = xmad(b, x.clone(), k.into(), ll_hi.into(), A_HI);
    let lo = xmad(b, x, k.into(), ll.into(), PSL_A_HI);
    [lo, b.shr(t.into(), 16.into(), false)]
}

fn umul_2x32_64<B: SSABuilder + ?Sized>(
    b: &mut B,
    x: Src,
    y: Src,
) -> [SSAValue; 2] {
    let ll = xmad(b, x.clone(), y.clone(), 0.into(), LO);
    let ll_hi = b.shr(ll.into(), 16.into(), false);
    let t = xmad(b, x.clone(), y.clone(), ll_hi.into(), A_HI);
    let t_lo = b.lop2(LogicOp2::And, t.into(), 0xffff.into());
    let u = xmad(b, x.clone(), y.clone(), t_lo.into(), B_HI);
    let t_hi = b.shr(t.into(), 16.into(), false);
    let v = xmad(b, x, y, t_hi.into(), BOTH_HI);
    let u_hi = b.shr(u.into(), 16.into(), false);
    let high = b.iadd(v.into(), u_hi.into(), 0.into());
    [b.prmt(ll.into(), u.into(), [0, 1, 4, 5]), high]
}

// The encoder's words run through a decoder written from yuzu's XMAD
// translator (half_a 53, half_b 35, psl 36, mrg 37, x 38, cc 47, signs 48 and
// 49, select mode 50..53, src_c 39) on the sequences above
#[cfg(test)]
mod sm53_xmad_tests {
    use super::*;

    const A: u32 = 1;
    const X: u32 = 2;
    const C: u32 = 3;

    fn bits(w: u64, at: u32, n: u32) -> u64 {
        (w >> at) & ((1 << n) - 1)
    }

    fn encode(form: Form, imm: Option<u32>) -> u64 {
        let gpr = |i| Src::from(SrcRef::Reg(RegRef::new(RegFile::GPR, i, 1)));
        let op = OpXmad {
            dst: RegRef::new(RegFile::GPR, 4, 1).into(),
            srcs: [gpr(A), imm.map_or(gpr(X), Src::from), gpr(C)],
            hi: form.hi,
            psl: form.psl,
            mrg: form.mrg,
            cbcc: form.cbcc,
        };
        let sm = ShaderModel50::new(53);
        let labels = FxHashMap::default();
        let (mut ip, mut sched) = (0, [0_u32; 2]);
        let i = Instr::new(op);
        let w = encode_instr(0, Some(&i), &sm, &labels, &mut ip, &mut sched);
        u64::from(w[0]) | (u64::from(w[1]) << 32)
    }

    fn run(w: u64, a: u32, x: u32, c: u32) -> u32 {
        // maxwell.inc: XMAD (imm) "0011 011- 00--", XMAD (reg) "0101 1011 00--"
        let imm = bits(w, 57, 7) == 0b001_1011 && bits(w, 54, 2) == 0;
        assert!(imm || bits(w, 54, 10) == 0b01_0110_1100);
        assert_eq!((bits(w, 0, 8), bits(w, 8, 8), bits(w, 39, 8)), (4, 1, 3));
        assert_eq!((bits(w, 38, 1), bits(w, 47, 3)), (0, 0));
        let b = match (imm, bits(w, 20, 8)) {
            (true, _) => bits(w, 20, 16) as u32,
            (false, 255) => 0,
            (false, r) => {
                assert_eq!(r, 2);
                x
            }
        };
        let half = |v: u32, hi: u64| if hi != 0 { v >> 16 } else { v & 0xffff };
        let b_hi = if imm { 0 } else { bits(w, 35, 1) };
        let mut p = half(a, bits(w, 53, 1)).wrapping_mul(half(b, b_hi));
        if bits(w, 36, 1) != 0 {
            p <<= 16;
        }
        let c = match bits(w, 50, 3) {
            0 => c,
            4 => c.wrapping_add(b << 16),
            mode => panic!("select mode {mode}"),
        };
        let r = p.wrapping_add(c);
        if bits(w, 37, 1) != 0 {
            (r & 0xffff) | (b << 16)
        } else {
            r
        }
    }

    fn xmad(form: Form, a: u32, x: u32, c: u32) -> u32 {
        run(encode(form, None), a, x, c)
    }

    fn xmad_imm(form: Form, a: u32, k: u32, c: u32) -> u32 {
        run(encode(form, Some(k)), a, 0, c)
    }

    fn values() -> Vec<u32> {
        let mut v = vec![0, 1, 0xffff, 0x10000, 0x7fff_ffff, 0x8000_0000];
        v.extend([0xffff_ffff, 0x8080_8081, 0x00ff_ffff, 510, 1020]);
        let mut s = 0x2545_f491_u32;
        for _ in 0..64 {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            v.push(s);
        }
        v
    }

    #[test]
    fn imul_is_three_xmads() {
        for &x in &values() {
            for &y in &values() {
                let lo = xmad(LO, y, x, 0);
                let merged = xmad(MERGE, y, x, 0);
                assert_eq!(xmad(MERGED_SUM, y, merged, lo), x.wrapping_mul(y));
                let low = xmad_imm(
                    PSL_A_HI,
                    x,
                    y & 0xffff,
                    xmad_imm(LO, x, y & 0xffff, 0),
                );
                let k_hi = y >> 16;
                let prod = if k_hi == 0 {
                    low
                } else {
                    xmad_imm(PSL, x, k_hi, low)
                };
                assert_eq!(prod, x.wrapping_mul(y));
            }
        }
    }

    #[test]
    fn wide_products() {
        for &x in &values() {
            for &y in &values() {
                let want = u64::from(x) * u64::from(y);
                let ll = xmad(LO, x, y, 0);
                let t = xmad(A_HI, x, y, ll >> 16);
                let u = xmad(B_HI, x, y, t & 0xffff);
                let v = xmad(BOTH_HI, x, y, t >> 16);
                let lo = (ll & 0xffff) | (u << 16);
                let hi = v.wrapping_add(u >> 16);
                assert_eq!(u64::from(hi) << 32 | u64::from(lo), want);
                if y <= 0xffff {
                    let ll = xmad_imm(LO, x, y, 0);
                    let t = xmad_imm(A_HI, x, y, ll >> 16);
                    let lo = xmad_imm(PSL_A_HI, x, y, ll);
                    assert_eq!(u64::from(t >> 16) << 32 | u64::from(lo), want);
                }
            }
        }
    }
}
