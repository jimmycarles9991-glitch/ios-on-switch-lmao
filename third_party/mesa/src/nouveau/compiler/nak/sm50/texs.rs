// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! Maxwell's scalar texture forms (TEXS, TLDS, TLD4S) and bound texture
//! indices. Layouts are nv50_ir's (nv50_ir_emit_gm107.cpp emitTEXS,
//! getTEXSMask, getTEXSTarget, getTLDSTarget; MIT); the precision bits and
//! the register order of each target, the Switch emulators' decoders.

use super::*;
use crate::ir::*;

/// The word index a bound texture instruction reads its handle at, in the
/// constant buffer SET_BINDLESS_TEXTURE names (the shader's only one:
/// nak_nir_lower_tex_sm50.c)
pub fn bound_index(tex: &TexRef) -> u16 {
    match tex {
        TexRef::Bound(idx) => *idx,
        TexRef::CBuf(TexCBufRef { offset, .. }) => {
            assert!(offset % 4 == 0);
            offset / 4
        }
        TexRef::Bindless => panic!("A scalar texture form is bound"),
    }
}

/// TEXS and TLDS's mask: the components written, in one register pair or two
fn scalar_mask(mask: ChannelMask, pair: bool) -> u8 {
    match (mask.to_bits(), pair) {
        (0x1, false) => 0,
        (0x2, false) => 1,
        (0x4, false) => 2,
        (0x8, false) => 3,
        (0x3, false) => 4,
        (0x9, false) => 5,
        (0xa, false) => 6,
        (0xc, false) => 7,
        (0x7, true) => 0,
        (0xb, true) => 1,
        (0xd, true) => 2,
        (0xe, true) => 3,
        (0xf, true) => 4,
        (m, _) => panic!("No scalar texture mask for {m:#x}"),
    }
}

fn texs_target(dim: TexDim, lod: TexLodMode, z_cmpr: bool) -> u8 {
    use TexLodMode::*;
    match (dim, lod, z_cmpr) {
        (TexDim::_1D, Zero, false) => 0x0,
        (TexDim::_2D, Auto, false) => 0x1,
        (TexDim::_2D, Zero, false) => 0x2,
        (TexDim::_2D, Lod, false) => 0x3,
        (TexDim::_2D, Auto, true) => 0x4,
        (TexDim::_2D, Lod, true) => 0x5,
        (TexDim::_2D, Zero, true) => 0x6,
        (TexDim::Array2D, Auto, false) => 0x7,
        (TexDim::Array2D, Zero, false) => 0x8,
        (TexDim::Array2D, Zero, true) => 0x9,
        (TexDim::_3D, Auto, false) => 0xa,
        (TexDim::_3D, Zero, false) => 0xb,
        (TexDim::Cube, Auto, false) => 0xc,
        (TexDim::Cube, Lod, false) => 0xd,
        _ => panic!("No TEXS target for {dim}{lod}"),
    }
}

fn tlds_target(dim: TexDim, lod: TexLodMode, offset: bool, ms: bool) -> u8 {
    use TexLodMode::*;
    match (dim, lod, offset, ms) {
        (TexDim::_1D, Zero, false, false) => 0x0,
        (TexDim::_1D, Lod, false, false) => 0x1,
        (TexDim::_2D, Zero, false, false) => 0x2,
        (TexDim::_2D, Zero, true, false) => 0x4,
        (TexDim::_2D, Lod, false, false) => 0x5,
        (TexDim::_2D, Zero, false, true) => 0x6,
        (TexDim::_3D, Zero, false, false) => 0x7,
        (TexDim::Array2D, Zero, false, false) => 0x8,
        (TexDim::_2D, Lod, true, false) => 0xc,
        _ => panic!("No TLDS target for {dim}{lod}"),
    }
}

/// A Maxwell shader's bound texture constant buffer + 1, SET_BINDLESS_TEXTURE's
/// slot: the buffer every texture handle at a constant buffer offset is read
/// from (they are all in one); 0: it has none
pub fn bound_texture_cbuf(s: &Shader<'_>) -> u8 {
    let mut found = 0_u8;
    for instr in s
        .functions
        .iter()
        .flat_map(|f| f.blocks.iter())
        .flat_map(|b| b.instrs.iter())
    {
        let tex = match &instr.op {
            Op::Tex(op) => &op.tex,
            Op::Tld(op) => &op.tex,
            Op::Tld4(op) => &op.tex,
            Op::Tmml(op) => &op.tex,
            Op::Txd(op) => &op.tex,
            Op::Txq(op) => &op.tex,
            _ => continue,
        };
        if let TexRef::CBuf(TexCBufRef { idx, .. }) = tex {
            assert!(
                found == 0 || found == idx + 1,
                "bound textures in two constant buffers"
            );
            found = idx + 1;
        }
    }
    found
}

impl SM50Encoder<'_> {
    fn set_scalar_operands(&mut self, dsts: &[Dst; 2], srcs: &[Src; 2]) {
        self.set_dst(&dsts[0]);
        match &dsts[1] {
            Dst::None => self.set_reg(28..36, zero_reg()),
            Dst::Reg(reg) => self.set_reg(28..36, *reg),
            dst => panic!("invalid dst {dst}"),
        }
        self.set_reg_src(8..16, &srcs[0]);
        self.set_reg_src(20..28, &srcs[1]);
    }

    pub fn encode_texs(&mut self, op: &OpTex, f16: bool) {
        assert!(op.fault.is_none());
        assert!(op.offset_mode == TexOffsetMode::None);
        self.set_field(60..64, 0xd_u8);
        self.set_bit(59, !f16);
        self.set_field(57..59, 0_u8);
        self.set_field(53..57, texs_target(op.dim, op.lod_mode, op.z_cmpr));
        self.set_field(
            50..53,
            scalar_mask(op.channel_mask, !op.dsts[1].is_none()),
        );
        self.set_bit(49, op.nodep);
        self.set_field(36..49, bound_index(&op.tex));
        self.set_scalar_operands(&op.dsts, &op.srcs);
    }

    pub fn encode_tlds(&mut self, op: &OpTld, f16: bool) {
        assert!(op.fault.is_none());
        let offset = match op.offset_mode {
            TexOffsetMode::None => false,
            TexOffsetMode::AddOffI => true,
            TexOffsetMode::PerPx => panic!("TLDS has no per-pixel offsets"),
        };
        self.set_field(60..64, 0xd_u8);
        self.set_bit(59, !f16);
        self.set_field(57..59, 1_u8);
        self.set_field(
            53..57,
            tlds_target(op.dim, op.lod_mode, offset, op.is_ms),
        );
        self.set_field(
            50..53,
            scalar_mask(op.channel_mask, !op.dsts[1].is_none()),
        );
        self.set_bit(49, op.nodep);
        self.set_field(36..49, bound_index(&op.tex));
        self.set_scalar_operands(&op.dsts, &op.srcs);
    }

    pub fn encode_tld4s(&mut self, op: &OpTld4, f16: bool) {
        assert!(op.fault.is_none());
        assert!(op.dim == TexDim::_2D);
        assert!(op.channel_mask.to_bits() == 0xf);
        let offset = match op.offset_mode {
            TexOffsetMode::None => false,
            TexOffsetMode::AddOffI => true,
            TexOffsetMode::PerPx => panic!("TLD4S has no per-pixel offsets"),
        };
        // TLD4S's precision bit is the other way round from TEXS's and TLDS's
        self.set_field(56..64, 0xdf_u8);
        self.set_bit(55, f16);
        self.set_field(52..54, op.comp);
        self.set_bit(51, offset);
        self.set_bit(50, op.z_cmpr);
        self.set_bit(49, op.nodep);
        self.set_field(36..49, bound_index(&op.tex));
        self.set_scalar_operands(&op.dsts, &op.srcs);
    }
}

// The scalar forms' encodings against the Switch emulators' decoder
// (texture_fetch_swizzled.cpp, texture_load_swizzled.cpp,
// texture_gather_swizzled.cpp) and maxwell.inc's patterns
#[cfg(test)]
mod sm53_texs_tests {
    use super::*;

    fn bits(w: u64, at: u32, n: u32) -> u64 {
        (w >> at) & ((1 << n) - 1)
    }

    /// maxwell.inc: TEXS "1101 -00-", TLDS "1101 -01-", TLD4S "1101 1111 -0--",
    /// TEX "1100 0---"
    fn decode_name(w: u64) -> &'static str {
        match (
            bits(w, 60, 4),
            bits(w, 57, 2),
            bits(w, 56, 8),
            bits(w, 59, 5),
        ) {
            (_, _, 0xdf, _) if bits(w, 54, 1) == 0 => "TLD4S",
            (0xd, 0, _, _) => "TEXS",
            (0xd, 1, _, _) => "TLDS",
            (_, _, _, 0x18) => "TEX",
            _ => "unknown",
        }
    }

    fn reg(i: u32, comps: u8) -> RegRef {
        RegRef::new(RegFile::GPR, i, comps)
    }

    fn src(i: u32, comps: u8) -> Src {
        SrcRef::Reg(reg(i, comps)).into()
    }

    fn encode(op: impl Into<Op>) -> u64 {
        let sm = ShaderModel50::new(53);
        let labels = FxHashMap::default();
        let mut ip = 0;
        let mut sched = [0_u32; 2];
        let i = Instr::new(op);
        let w = encode_instr(0, Some(&i), &sm, &labels, &mut ip, &mut sched);
        u64::from(w[0]) | (u64::from(w[1]) << 32)
    }

    fn tex(lod_mode: TexLodMode, srcs: [Src; 2], scalar: bool) -> OpTex {
        OpTex {
            dsts: [reg(8, 2).into(), reg(10, 2).into()],
            fault: Dst::None,
            tex: TexRef::CBuf(TexCBufRef {
                idx: 2,
                offset: 0x40,
            }),
            srcs,
            dim: TexDim::_2D,
            lod_mode,
            deriv_mode: TexDerivMode::Auto,
            z_cmpr: false,
            offset_mode: TexOffsetMode::None,
            mem_eviction_priority: MemEvictionPriority::Normal,
            nodep: true,
            channel_mask: ChannelMask::new(0xf),
            scalar,
            f16: false,
        }
    }

    #[test]
    fn texs_2d_writes_a_pair_then_b_pair_in_f32() {
        let w = encode(tex(TexLodMode::Auto, [src(4, 1), src(5, 1)], true));
        assert_eq!(decode_name(w), "TEXS");
        assert_eq!(bits(w, 59, 1), 1); // precision: F32
        assert_eq!(bits(w, 53, 4), 1); // 2D
        assert_eq!(bits(w, 50, 3), 4); // RGBA, B in use
        assert_eq!(bits(w, 49, 1), 1); // nodep
        assert_eq!(bits(w, 36, 13), 0x10); // the handle's word
        assert_eq!(bits(w, 0, 8), 8); // dest A
        assert_eq!(bits(w, 28, 8), 10); // dest B
        assert_eq!(bits(w, 8, 8), 4); // x
        assert_eq!(bits(w, 20, 8), 5); // y
    }

    #[test]
    fn texs_2d_level_takes_the_coordinates_as_a_pair() {
        let w = encode(tex(TexLodMode::Lod, [src(4, 2), src(7, 1)], true));
        assert_eq!(bits(w, 53, 4), 3); // 2D.LL
        assert_eq!((bits(w, 8, 8), bits(w, 20, 8)), (4, 7));
    }

    #[test]
    fn texs_masks_of_two_registers_leave_b_unused() {
        let mut op = tex(TexLodMode::Zero, [src(4, 1), src(5, 1)], true);
        op.dsts = [reg(8, 2).into(), Dst::None];
        op.channel_mask = ChannelMask::new(0x9);
        let w = encode(op);
        assert_eq!(bits(w, 53, 4), 2); // 2D.LZ
        assert_eq!(bits(w, 50, 3), 5); // RA
        assert_eq!(bits(w, 28, 8), 255);
    }

    #[test]
    fn bound_tex_is_tex_with_its_word_index() {
        let mut op =
            tex(TexLodMode::Auto, [src(4, 2), SrcRef::Zero.into()], false);
        op.dsts = [reg(8, 4).into(), Dst::None];
        let w = encode(op);
        assert_eq!(decode_name(w), "TEX");
        assert_eq!(bits(w, 36, 13), 0x10);
        assert_eq!(bits(w, 51, 3), 7); // no sparse predicate
        assert_eq!(bits(w, 55, 2), 0);
    }

    #[test]
    fn tlds_and_tld4s() {
        let w = encode(OpTld {
            dsts: [reg(8, 2).into(), reg(10, 2).into()],
            fault: Dst::None,
            tex: TexRef::CBuf(TexCBufRef {
                idx: 2,
                offset: 0x44,
            }),
            srcs: [src(4, 2), src(6, 1)],
            dim: TexDim::_2D,
            is_ms: false,
            lod_mode: TexLodMode::Lod,
            offset_mode: TexOffsetMode::None,
            mem_eviction_priority: MemEvictionPriority::Normal,
            nodep: false,
            channel_mask: ChannelMask::new(0xf),
            scalar: true,
            f16: false,
        });
        assert_eq!(decode_name(w), "TLDS");
        assert_eq!(bits(w, 59, 1), 1);
        assert_eq!(bits(w, 53, 4), 5); // 2D.LL
        assert_eq!(bits(w, 36, 13), 0x11);
        let w = encode(OpTld4 {
            dsts: [reg(8, 2).into(), reg(10, 2).into()],
            fault: Dst::None,
            tex: TexRef::CBuf(TexCBufRef {
                idx: 2,
                offset: 0x48,
            }),
            srcs: [src(4, 1), src(5, 1)],
            dim: TexDim::_2D,
            comp: 2,
            offset_mode: TexOffsetMode::None,
            z_cmpr: false,
            mem_eviction_priority: MemEvictionPriority::Normal,
            nodep: false,
            channel_mask: ChannelMask::new(0xf),
            scalar: true,
            f16: false,
        });
        assert_eq!(decode_name(w), "TLD4S");
        assert_eq!(bits(w, 55, 1), 0); // precision: F32
        assert_eq!(bits(w, 52, 2), 2); // component
        assert_eq!(bits(w, 36, 13), 0x12);
        assert_eq!((bits(w, 8, 8), bits(w, 20, 8)), (4, 5));
    }
}
