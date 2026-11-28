// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! SM50/SM53 interlock backoff and Op::SrcBar end-to-end backend tests:
//! Op::SrcBar survives opt_dce, lowers to NOP with wt_bar_mask waiting on
//! S2R SR_CLOCKLO's wr_bar, emits S2R (not CS2R) on SM53, keeps zero clock
//! reads before the fast-path LDG.E.CG break, and leaves SM75 CS2R/NANOSLEEP
//! intact.

use super::*;
use crate::sm70::ShaderModel70;
use crate::sm70_encode::encode_sm70_shader;
use compiler::cfg::CFGBuilder;
use rustc_hash::FxBuildHasher;

const SR_CLOCKLO: u8 = 0x50;
const SR_CLOCKHI: u8 = 0x51;

fn bits(w: u64, at: u32, n: u32) -> u64 {
    (w >> at) & ((1 << n) - 1)
}

fn sm50_word(code: &[u32], instr_idx: usize) -> u64 {
    let byte = instr_address(instr_idx);
    u64::from(code[byte / 4]) | (u64::from(code[byte / 4 + 1]) << 32)
}

fn sm50_sched(code: &[u32], instr_idx: usize) -> u32 {
    let group = instr_idx / 3;
    let slot = instr_idx % 3;
    let sw =
        u64::from(code[group * 8]) | (u64::from(code[group * 8 + 1]) << 32);
    ((sw >> (21 * slot)) & 0x1f_ffff) as u32
}

fn sm50_instr_count(code: &[u32]) -> usize {
    (code.len() / 8) * 3
}

fn make_shader<'a>(
    sm: &'a ShaderModelInfo,
    ssa_alloc: SSAValueAllocator,
    phi_alloc: PhiAllocator,
    blocks: Vec<BasicBlock>,
    edges: &[(usize, usize)],
) -> Shader<'a> {
    let mut cfg = CFGBuilder::<_, _, FxBuildHasher>::new();
    for (i, b) in blocks.into_iter().enumerate() {
        cfg.add_node(i, b);
    }
    for &(from, to) in edges {
        cfg.add_edge(from, to);
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
            uses_global_mem: true,
            writes_global_mem: false,
            uses_fp64: false,
            stage: ShaderStageInfo::Fragment(FragmentShaderInfo {
                uses_kill: false,
                uses_sample_shading: false,
                post_depth_coverage: false,
                early_fragment_tests: false,
                reads_interlock_ticket: true,
                interlock_mode: FragmentInterlockMode::Pixel,
                interlock_unordered: false,
            }),
            io: ShaderIoInfo::None,
        },
        functions: vec![Function {
            ssa_alloc,
            phi_alloc,
            blocks: cfg.as_cfg(false),
        }],
    }
}

fn run_backend(s: &mut Shader<'_>) {
    s.opt_bar_prop();
    s.opt_uniform_instrs();
    s.opt_copy_prop();
    s.opt_prmt();
    s.opt_lop();
    s.opt_copy_prop();
    s.opt_dce();
    s.opt_out();
    s.legalize();
    s.opt_dce();
    s.opt_instr_sched_prepass();
    s.assign_regs();
    s.lower_par_copies();
    s.lower_copy_swap();
    if s.sm.sm() >= 70 {
        s.opt_jump_thread();
    } else {
        s.opt_crs();
    }
    s.remove_annotations();
    s.opt_instr_sched_postpass();
    s.calc_instr_deps();
    s.gather_info();
}

fn count_src_bars(s: &Shader<'_>) -> usize {
    s.functions
        .iter()
        .flat_map(|f| f.blocks.iter())
        .flat_map(|b| b.instrs.iter())
        .filter(|i| matches!(i.op, Op::SrcBar(_)))
        .count()
}

/// Builds an SM53 ticket-wait loop with exponential S2R SR_CLOCKLO backoff:
///
///   B0 (entry):   addr = LDC c[0][0x260]; expected = S2R SR_ORDERING_TICKET;
///                 delay0 = 32; PBK B4
///   B1 (poll):    delay = phi(B0: delay0, B3: next_delay)
///                 tkt = LDG.E.CG [addr]; hi = SHR tkt, 16;
///                 done = ISETP.EQ hi, expected; @done BRK B4
///   B2 (backoff): start = S2R SR_CLOCKLO; SrcBar(start)
///   B3 (spin):    now = S2R SR_CLOCKLO; SrcBar(now);
///                 diff = IADD now, -start;
///                 expired = ISETP.GE.U32 diff, delay;
///                 @!expired BRA B3
///   B4 (step):    doubled = SHL delay, 1; next_delay = IMNMX.U32.MIN doubled, 256;
///                 BRA B1
///   B5 (exit):    EXIT
fn build_sm53_interlock_backoff(sm: &ShaderModelInfo) -> Shader<'_> {
    let mut ssa = SSAValueAllocator::new();
    let mut phis = PhiAllocator::new();
    let mut labels = LabelAllocator::new();
    let (l0, l1, l2, l3, l4, l5) = (
        labels.alloc(),
        labels.alloc(),
        labels.alloc(),
        labels.alloc(),
        labels.alloc(),
        labels.alloc(),
    );

    let addr = ssa.alloc_vec(RegFile::GPR, 2);
    let ticket = ssa.alloc(RegFile::GPR);
    let expected = ssa.alloc(RegFile::GPR);
    let delay0 = ssa.alloc(RegFile::GPR);
    let delay = ssa.alloc(RegFile::GPR);
    let tkt = ssa.alloc(RegFile::GPR);
    let hi = ssa.alloc(RegFile::GPR);
    let done = ssa.alloc(RegFile::Pred);
    let start = ssa.alloc(RegFile::GPR);
    let now = ssa.alloc(RegFile::GPR);
    let diff = ssa.alloc(RegFile::GPR);
    let expired = ssa.alloc(RegFile::Pred);
    let doubled = ssa.alloc(RegFile::GPR);
    let next_delay = ssa.alloc(RegFile::GPR);
    let delay_phi = phis.alloc();

    let mut b0_phi = OpPhiSrcs::new();
    b0_phi.srcs.push(delay_phi, delay0.into());
    let mut b1_phi = OpPhiDsts::new();
    b1_phi.dsts.push(delay_phi, delay.into());
    let mut b4_phi = OpPhiSrcs::new();
    b4_phi.srcs.push(delay_phi, next_delay.into());

    let mut brk = Instr::new(OpBrk { target: l5 });
    brk.pred = Pred::from(done);

    let mut spin_bra = Instr::new(OpBra {
        target: l3,
        cond: SrcRef::True.into(),
    });
    spin_bra.pred = Pred::from(expired).bnot();

    let blocks = vec![
        BasicBlock {
            label: l0,
            uniform: true,
            instrs: vec![
                Instr::new(OpLdc {
                    dst: addr.clone().into(),
                    cb: CBufRef {
                        buf: CBuf::Binding(0),
                        offset: 0x260,
                    }
                    .into(),
                    offset: Src::ZERO,
                    mode: LdcMode::Indexed,
                    mem_type: MemType::B64,
                }),
                Instr::new(OpS2R {
                    dst: ticket.into(),
                    idx: 0x0f,
                }),
                Instr::new(OpShr {
                    dst: expected.into(),
                    src: ticket.into(),
                    shift: 16_u32.into(),
                    wrap: false,
                    signed: false,
                }),
                Instr::new(OpCopy {
                    dst: delay0.into(),
                    src: 32_u32.into(),
                }),
                Instr::new(OpPBk { target: l5 }),
                Instr::new(b0_phi),
            ],
        },
        BasicBlock {
            label: l1,
            uniform: false,
            instrs: vec![
                Instr::new(b1_phi),
                Instr::new(OpLd {
                    dst: tkt.into(),
                    addr: addr.into(),
                    uniform_addr: SrcRef::Zero.into(),
                    pred: SrcRef::True.into(),
                    offset: 0,
                    stride: OffsetStride::X1,
                    access: MemAccess {
                        mem_type: MemType::B32,
                        space: MemSpace::Global(MemAddrType::A64),
                        order: MemOrder::Strong(MemScope::System),
                        eviction_priority: MemEvictionPriority::NoAllocate,
                    },
                }),
                Instr::new(OpShr {
                    dst: hi.into(),
                    src: tkt.into(),
                    shift: 16_u32.into(),
                    wrap: false,
                    signed: false,
                }),
                Instr::new(OpISetP {
                    dst: done.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Eq,
                    cmp_type: IntCmpType::U32,
                    ex: false,
                    srcs: [hi.into(), expected.into()],
                    accum: SrcRef::True.into(),
                    low_cmp: SrcRef::True.into(),
                }),
                brk,
            ],
        },
        BasicBlock {
            label: l2,
            uniform: false,
            instrs: vec![
                Instr::new(OpS2R {
                    dst: start.into(),
                    idx: SR_CLOCKLO,
                }),
                Instr::new(OpSrcBar { src: start.into() }),
            ],
        },
        BasicBlock {
            label: l3,
            uniform: false,
            instrs: vec![
                Instr::new(OpS2R {
                    dst: now.into(),
                    idx: SR_CLOCKLO,
                }),
                Instr::new(OpSrcBar { src: now.into() }),
                Instr::new(OpIAdd2 {
                    dst: diff.into(),
                    carry_out: Dst::None,
                    srcs: [now.into(), Src::from(start).ineg()],
                }),
                Instr::new(OpISetP {
                    dst: expired.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Ge,
                    cmp_type: IntCmpType::U32,
                    ex: false,
                    srcs: [diff.into(), delay.into()],
                    accum: SrcRef::True.into(),
                    low_cmp: SrcRef::True.into(),
                }),
                spin_bra,
            ],
        },
        BasicBlock {
            label: l4,
            uniform: false,
            instrs: vec![
                Instr::new(OpShl {
                    dst: doubled.into(),
                    src: delay.into(),
                    shift: 1_u32.into(),
                    wrap: false,
                }),
                Instr::new(OpIMnMx {
                    dst: next_delay.into(),
                    cmp_type: IntCmpType::U32,
                    srcs: [doubled.into(), 256_u32.into()],
                    min: SrcRef::True.into(),
                }),
                Instr::new(b4_phi),
                Instr::new(OpBra {
                    target: l1,
                    cond: SrcRef::True.into(),
                }),
            ],
        },
        BasicBlock {
            label: l5,
            uniform: true,
            instrs: vec![Instr::new(OpExit {})],
        },
    ];

    make_shader(
        sm,
        ssa,
        phis,
        blocks,
        &[(0, 1), (1, 2), (1, 5), (2, 3), (3, 3), (3, 4), (4, 1)],
    )
}

#[test]
fn sm53_interlock_backoff_preserves_src_bar_and_emits_s2r_clock() {
    let sm_info = ShaderModelInfo::new(53, 64);
    let mut s = build_sm53_interlock_backoff(&sm_info);

    s.opt_copy_prop();
    s.opt_dce();
    assert_eq!(count_src_bars(&s), 2, "opt_dce must not delete Op::SrcBar");
    s.legalize();
    s.opt_dce();
    assert_eq!(
        count_src_bars(&s),
        2,
        "post-legalize opt_dce must keep Op::SrcBar"
    );

    s.opt_instr_sched_prepass();
    s.assign_regs();
    s.lower_par_copies();
    s.lower_copy_swap();
    s.opt_crs();
    s.remove_annotations();
    s.opt_instr_sched_postpass();
    s.calc_instr_deps();
    assert_eq!(
        count_src_bars(&s),
        0,
        "calc_instr_deps must lower every Op::SrcBar to Op::Nop"
    );

    let code = encode_sm50_shader(&ShaderModel50::new(53), &s);
    let n = sm50_instr_count(&code);
    let mut first_ldg = None;
    let mut first_brk = None;
    let mut clock_s2r_indices = Vec::new();
    let mut has_iadd_sub = false;
    let mut has_isetp_ge_u32 = false;
    let mut has_bra = false;

    for i in 0..n {
        let w = sm50_word(&code, i);
        let sched = sm50_sched(&code, i);
        let op16 = bits(w, 48, 16);

        assert_ne!(op16, 0x50c8, "SM53 must not emit unverified CS2R");

        if (op16 & 0xfff8) == 0xeed0 && bits(w, 46, 2) == 1 {
            first_ldg.get_or_insert(i);
        }
        if op16 == 0xe340 {
            first_brk.get_or_insert(i);
        }
        if op16 == 0xf0c8 && bits(w, 20, 8) == u64::from(SR_CLOCKLO) {
            clock_s2r_indices.push((i, sched));
        }
        if (op16 & 0xfffc) == 0x5c10 && bits(w, 48, 2) != 0 {
            has_iadd_sub = true;
        }
        if ((op16 & 0xff00) == 0x5b00 || (op16 & 0xfe00) == 0x3600)
            && bits(w, 49, 3) == 6
            && bits(w, 48, 1) == 0
        {
            has_isetp_ge_u32 = true;
        }
        if op16 == 0xe240 {
            has_bra = true;
        }
    }

    let ldg_idx = first_ldg.expect("expected LDG.E.CG poll");
    let brk_idx = first_brk.expect("expected fast-path BRK after LDG.E.CG");
    assert!(ldg_idx < brk_idx);
    assert_eq!(clock_s2r_indices.len(), 2, "expected start and now S2R");
    assert!(
        clock_s2r_indices.iter().all(|&(idx, _)| idx > brk_idx),
        "zero clock reads may precede the fast-path LDG.E.CG break"
    );

    for (idx, sched) in clock_s2r_indices {
        let wr_bar = (sched >> 5) & 7;
        let delay = sched & 0xf;
        assert!(wr_bar < 6, "S2R SR_CLOCKLO must set a write barrier");
        assert!(delay >= 2, "S2R must hold >= 2 cycles before barrier wait");
        let next_w = sm50_word(&code, idx + 1);
        let next_sched = sm50_sched(&code, idx + 1);
        let wt_mask = (next_sched >> 11) & 0x3f;
        assert_eq!(
            bits(next_w, 48, 16),
            0x50b0,
            "Op::SrcBar must lower to NOP right after S2R SR_CLOCKLO"
        );
        assert_ne!(
            wt_mask & (1 << wr_bar),
            0,
            "NOP from Op::SrcBar must wait on S2R's wr_bar"
        );
    }

    assert!(has_iadd_sub, "expected IADD subtraction in backoff loop");
    assert!(has_isetp_ge_u32, "expected ISETP.GE.U32 in backoff loop");
    assert!(has_bra, "expected BRA in backoff loop");
}

#[test]
fn sm75_cs2r_src_bar_and_nanosleep_compile_cleanly() {
    for sm_ver in [75, 80, 120] {
        let sm_info = ShaderModelInfo::new(sm_ver, 32);
        let mut ssa = SSAValueAllocator::new();
        let phis = PhiAllocator::new();
        let mut labels = LabelAllocator::new();
        let l0 = labels.alloc();

        let addr = ssa.alloc_vec(RegFile::GPR, 2);
        let hi = ssa.alloc(RegFile::GPR);
        let lo = ssa.alloc(RegFile::GPR);
        let sum = ssa.alloc(RegFile::GPR);
        let sleep = ssa.alloc(RegFile::GPR);

        let blocks = vec![BasicBlock {
            label: l0,
            uniform: true,
            instrs: vec![
                Instr::new(OpLdc {
                    dst: addr.clone().into(),
                    cb: CBufRef {
                        buf: CBuf::Binding(1),
                        offset: 0,
                    }
                    .into(),
                    offset: Src::ZERO,
                    mode: LdcMode::Indexed,
                    mem_type: MemType::B64,
                }),
                Instr::new(OpCS2R {
                    dst: hi.into(),
                    idx: SR_CLOCKHI,
                }),
                Instr::new(OpSrcBar { src: hi.into() }),
                Instr::new(OpCS2R {
                    dst: lo.into(),
                    idx: SR_CLOCKLO,
                }),
                Instr::new(OpSrcBar { src: lo.into() }),
                Instr::new(OpIAdd3 {
                    dst: sum.into(),
                    overflow: [Dst::None, Dst::None],
                    srcs: [hi.into(), lo.into(), Src::ZERO],
                }),
                Instr::new(OpCopy {
                    dst: sleep.into(),
                    src: 64_u32.into(),
                }),
                Instr::new(OpNanosleep { time: sleep.into() }),
                Instr::new(OpSt {
                    addr: addr.into(),
                    uniform_addr: SrcRef::Zero.into(),
                    data: sum.into(),
                    offset: 0,
                    stride: OffsetStride::X1,
                    access: MemAccess {
                        mem_type: MemType::B32,
                        space: MemSpace::Global(MemAddrType::A64),
                        order: MemOrder::Strong(MemScope::GPU),
                        eviction_priority: MemEvictionPriority::Normal,
                    },
                }),
                Instr::new(OpExit {}),
            ],
        }];

        let mut s = make_shader(&sm_info, ssa, phis, blocks, &[]);
        run_backend(&mut s);
        assert_eq!(count_src_bars(&s), 0);

        let code = encode_sm70_shader(&ShaderModel70::new(sm_ver), &s);
        let mut cs2r_count = 0;
        let mut nanosleep_count = 0;
        let mut nop_count = 0;
        for chunk in code.chunks_exact(4) {
            let op = chunk[0] & 0xfff;
            if op == 0x805 {
                cs2r_count += 1;
            } else if (op & 0x1ff) == 0x15d {
                nanosleep_count += 1;
            } else if op == 0x918 {
                nop_count += 1;
            }
        }
        assert_eq!(cs2r_count, 2, "sm{sm_ver} must keep CS2R for clock reads");
        assert_eq!(nanosleep_count, 1, "sm{sm_ver} must keep NANOSLEEP");
        assert!(nop_count >= 2, "sm{sm_ver} must lower both SrcBars to NOP");
    }
}

#[test]
fn sm53_assign_deps_serial_lowers_src_bar() {
    let sm_info = ShaderModelInfo::new(53, 64);
    let mut s = build_sm53_interlock_backoff(&sm_info);
    s.opt_copy_prop();
    s.opt_dce();
    s.legalize();
    s.opt_dce();
    s.assign_regs();
    s.lower_par_copies();
    s.lower_copy_swap();
    s.opt_crs();
    s.assign_deps_serial();
    assert_eq!(count_src_bars(&s), 0);
    let code = encode_sm50_shader(&ShaderModel50::new(53), &s);
    assert!(!code.is_empty());
}
