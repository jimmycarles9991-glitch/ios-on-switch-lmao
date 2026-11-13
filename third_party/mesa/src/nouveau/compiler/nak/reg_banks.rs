// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! Register banks for SM50 register allocation. On GM20B an instruction
//! reading two source registers of one bank (register % 4) issues a cycle
//! later for each extra read from that bank, and the reuse flags save
//! nothing (bench loop dual1 and tput1), so RA gives a value a register
//! whose bank the other sources of its next uses do not occupy, when one is
//! free.

use crate::ir::*;

use rustc_hash::{FxHashMap, FxHashSet};

/// How many of a value's next uses pick its bank.
const USES_CONSIDERED: usize = 3;

pub const NUM_BANKS: u32 = 4;

pub fn bank(reg: u32) -> u32 {
    reg % NUM_BANKS
}

fn reads_register_banks(op: &Op, sm: u8) -> bool {
    !op.is_virtual() && (op.has_fixed_latency(sm) || matches!(op, Op::Xmad(_)))
}

/// Whether the block defines a value a later block reads: one live out of
/// it, a phi source, or a value copied into one.
pub fn defines_values_out(
    b: &BasicBlock,
    live_out: impl Fn(&SSAValue) -> bool,
) -> bool {
    let mut leaving: FxHashSet<SSAValue> = FxHashSet::default();
    if let Some(phis) = b.phi_srcs() {
        for (_, src) in phis.srcs.iter() {
            leaving.extend(src.iter_ssa());
        }
    }
    for instr in b.instrs.iter().rev() {
        let defines = |ssa: &SSAValue| live_out(ssa) || leaving.contains(ssa);
        if instr.dsts().iter().any(|d| d.iter_ssa().any(defines)) {
            return true;
        }
        if let Op::Copy(copy) = &instr.op {
            if copy.dst.iter_ssa().any(|ssa| leaving.contains(ssa)) {
                leaving.extend(copy.src.iter_ssa());
            }
        }
    }
    false
}

/// For each scalar GPR value of a block, the instructions that read it
/// beside other GPR values, and those other values.
pub struct BankPartners {
    uses: FxHashMap<SSAValue, Vec<(usize, Vec<SSAValue>)>>,
}

impl BankPartners {
    /// The block's values that die in it: a value live out of the block
    /// keeps its register into the next blocks, whose parallel copies a
    /// scattered choice multiplies.
    pub fn for_block(
        b: &BasicBlock,
        sm: u8,
        live_out: impl Fn(&SSAValue) -> bool,
    ) -> Option<BankPartners> {
        if sm >= 70 {
            return None;
        }
        let mut uses: FxHashMap<SSAValue, Vec<(usize, Vec<SSAValue>)>> =
            Default::default();
        for (ip, instr) in b.instrs.iter().enumerate() {
            if !reads_register_banks(&instr.op, sm) {
                continue;
            }
            let mut read: Vec<SSAValue> = Vec::new();
            for src in instr.srcs() {
                if let SrcRef::SSA(ssa) = &src.src_ref {
                    if ssa.comps() == 1
                        && ssa[0].file() == RegFile::GPR
                        && !read.contains(&ssa[0])
                    {
                        read.push(ssa[0]);
                    }
                }
            }
            if read.len() < 2 {
                continue;
            }
            for &ssa in read.iter().filter(|ssa| !live_out(ssa)) {
                let others = read.iter().copied().filter(|&o| o != ssa);
                uses.entry(ssa).or_default().push((ip, others.collect()));
            }
        }
        Some(BankPartners { uses })
    }

    /// The banks the allocated partners of `ssa`'s next uses after `ip`
    /// occupy, one bit each.
    pub fn taken_banks(
        &self,
        ssa: SSAValue,
        ip: usize,
        reg_of: impl Fn(SSAValue) -> Option<u32>,
    ) -> u32 {
        let Some(uses) = self.uses.get(&ssa) else {
            return 0;
        };
        let first = uses.partition_point(|(uip, _)| *uip <= ip);
        let mut taken = 0;
        for (_, partners) in uses[first..].iter().take(USES_CONSIDERED) {
            for &p in partners {
                if let Some(reg) = reg_of(p) {
                    taken |= 1 << bank(reg);
                }
            }
        }
        taken
    }
}

/// Whether register allocation left a parallel copy that moves a value
pub fn moves_values(s: &Shader<'_>) -> bool {
    s.functions.iter().flat_map(|f| f.blocks.iter()).any(|b| {
        b.instrs.iter().any(|i| match &i.op {
            Op::ParCopy(pc) => pc
                .dsts_srcs
                .iter()
                .any(|(d, src)| d.as_reg() != src.src_ref.as_reg()),
            _ => false,
        })
    })
}

/// The extra register-file reads a program's ALU instructions pay for
/// reading one bank twice
pub fn extra_reads(s: &Shader<'_>) -> usize {
    let mut extra = 0;
    for b in s.functions.iter().flat_map(|f| f.blocks.iter()) {
        for i in &b.instrs {
            if !reads_register_banks(&i.op, s.sm.sm()) {
                continue;
            }
            let mut regs: Vec<u32> = Vec::new();
            for src in i.srcs() {
                if let SrcRef::Reg(r) = &src.src_ref {
                    if r.file() == RegFile::GPR && !regs.contains(&r.base_idx())
                    {
                        regs.push(r.base_idx());
                    }
                }
            }
            let mut per_bank = [0_usize; NUM_BANKS as usize];
            for r in regs {
                per_bank[bank(r) as usize] += 1;
            }
            extra +=
                per_bank.iter().map(|n| n.saturating_sub(1)).sum::<usize>();
        }
    }
    extra
}
