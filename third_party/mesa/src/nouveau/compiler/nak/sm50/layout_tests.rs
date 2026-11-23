// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! SM50 code layout: a block runs on into the scheduling group its
//! predecessor ends in, and a branch-class instruction stalls in place of a
//! NOP behind it.

use super::*;
use compiler::cfg::CFGBuilder;
use rustc_hash::FxBuildHasher;

fn shader(sm: &ShaderModelInfo, blocks: Vec<BasicBlock>) -> Shader<'_> {
    let mut cfg = CFGBuilder::<_, _, FxBuildHasher>::new();
    let n = blocks.len();
    for (i, b) in blocks.into_iter().enumerate() {
        cfg.add_node(i, b);
    }
    for i in 1..n {
        cfg.add_edge(i - 1, i);
    }
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
            uses_global_mem: false,
            writes_global_mem: false,
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
            blocks: cfg.as_cfg(false),
        }],
    }
}

fn word(code: &[u32], byte: usize) -> u64 {
    u64::from(code[byte / 4]) | (u64::from(code[byte / 4 + 1]) << 32)
}

fn stall(code: &[u32], byte: usize) -> u64 {
    let slot = (byte % 32) / 8 - 1;
    (word(code, byte / 32 * 32) >> (21 * slot)) & 0xf
}

/// A BRA ending one block and the EXIT that is the next block: one group,
/// the EXIT its second instruction and the BRA's target
#[test]
fn a_block_starts_in_the_group_its_predecessor_ends_in() {
    let sm_info = ShaderModelInfo::new(53, 64);
    let mut labels = LabelAllocator::new();
    let (first, second) = (labels.alloc(), labels.alloc());
    let bra = Instr::new(OpBra {
        target: second,
        cond: SrcRef::True.into(),
    });
    let mut s = shader(
        &sm_info,
        vec![
            BasicBlock {
                label: first,
                uniform: true,
                instrs: vec![bra],
            },
            BasicBlock {
                label: second,
                uniform: true,
                instrs: vec![Instr::new(OpExit {})],
            },
        ],
    );
    s.calc_instr_deps();
    let code = encode_sm50_shader(&ShaderModel50::new(53), &s);

    assert_eq!(code.len(), 8);
    let (bra, exit) = (word(&code, 0x08), word(&code, 0x10));
    assert_eq!(bra >> 48, 0xe240);
    assert_eq!((bra >> 20) & 0xff_ffff, 0, "the branch lands on 0x10");
    assert_eq!(exit >> 48, 0xe300);
    assert_eq!((stall(&code, 0x08), stall(&code, 0x10)), (13, 15));
}

#[test]
fn no_instruction_address_is_a_scheduling_word() {
    for n in 0..96 {
        let step = if n % 3 == 2 { 16 } else { 8 };
        assert_ne!(instr_address(n) % 32, 0);
        assert_eq!(instr_address(n + 1) - instr_address(n), step);
    }
}
