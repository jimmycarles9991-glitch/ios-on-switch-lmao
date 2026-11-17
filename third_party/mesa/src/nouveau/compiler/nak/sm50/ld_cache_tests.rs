// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! SM50 LDG's cache op (bits 46-47: 1 .CG, 3 .CV): an atomic load, the pixel
//! interlock's ticket wait (nak_nir.c), reads L2 alone; a coherent load keeps
//! .CV.

use super::*;

fn bits(w: u64, at: u32, n: u32) -> u64 {
    (w >> at) & ((1 << n) - 1)
}

fn ldg(order: MemOrder, eviction_priority: MemEvictionPriority) -> u64 {
    let sm = ShaderModel50::new(53);
    let labels = FxHashMap::default();
    let (mut ip, mut sched) = (0, [0_u32; 2]);
    let i = Instr::new(OpLd {
        dst: RegRef::new(RegFile::GPR, 2, 1).into(),
        addr: RegRef::new(RegFile::GPR, 4, 2).into(),
        uniform_addr: SrcRef::Zero.into(),
        pred: SrcRef::True.into(),
        offset: 0,
        stride: OffsetStride::X1,
        access: MemAccess {
            mem_type: MemType::B32,
            space: MemSpace::Global(MemAddrType::A64),
            order,
            eviction_priority,
        },
    });
    let w = encode_instr(0, Some(&i), &sm, &labels, &mut ip, &mut sched);
    u64::from(w[0]) | (u64::from(w[1]) << 32)
}

#[test]
fn an_atomic_load_reads_l2_alone() {
    let w = ldg(
        MemOrder::Strong(MemScope::System),
        MemEvictionPriority::NoAllocate,
    );
    assert_eq!(bits(w, 48, 16) & 0xfff8, 0xeed0);
    assert_eq!(bits(w, 46, 2), 1);
}

#[test]
fn a_coherent_load_stays_uncached() {
    let w = ldg(
        MemOrder::Strong(MemScope::System),
        MemEvictionPriority::Normal,
    );
    assert_eq!(bits(w, 46, 2), 3);
}
