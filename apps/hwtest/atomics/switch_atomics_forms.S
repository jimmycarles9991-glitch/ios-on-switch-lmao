/* cpu.atomics' forms (switch_atomics_list.h). Built with LSE, as an iOS 26+ deployment target builds (an A12), each
 * form's site is its one LSE instruction, which the a64x loader rewrites into a branch to a stub. Built without, as
 * iOS 17's arm64 builds, the site is the ARMv8.0 sequence a64x's stub for that instruction runs (a64x_rewrite.c
 * stub_ldop_swp, stub_cas, stub_casp, and their spills below the red zone).
 *
 * Around the site NZCV holds 0b1011 and x9-x15 hold sentinels; a spill form keeps every register but its operands live
 * (x3-x17, x19-x28), so a stub has none to spare. A form returns the old value in x0 and, in x1, whether NZCV or a
 * sentinel changed:
 *   form(void *address, uint64_t a, uint64_t b)   LD<op>, SWP: *address op= a; CAS: if *address == a, *address = b
 *   form(void *pair, const uint64_t spec[4], uint64_t old[2])   CASP: if *pair == spec[0..1], *pair = spec[2..3]
 * _switch_atomic_sites holds each form's site address, in the list's order, so the harness can read the word this
 * process runs there; the sites are local labels, not symbols that would read as function starts. */

#define PLAIN_LIVE 9, 10, 11, 12, 13, 14, 15
#define SPILL_LIVE_LD 3, 4, 5, 6, 7, 8, PLAIN_LIVE, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28
#define SPILL_LIVE_CAS 4, 5, 6, 7, 8, PLAIN_LIVE, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28
#define SPILL_LIVE_CASP 8, PLAIN_LIVE, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28

.macro sentinels_set regs:vararg
    mov x16, #0xb0000000
    msr nzcv, x16
    .irp r, \regs
    mov x\r, #(0x300 + \r)
    .endr
.endm

/* The old value is in x2: return it in x0 and the clobber flag in x1. */
.macro sentinels_check regs:vararg
    mrs x0, nzcv
    lsr x0, x0, #28
    cmp x0, #0xb
    cset w0, ne
    .irp r, \regs
    cmp x\r, #(0x300 + \r)
    cset w\r, ne
    orr w0, w0, w\r
    .endr
    mov x1, x0
    mov x0, x2
.endm

.macro live_set shape, spill_regs:vararg
    .ifc \shape, spill
    sentinels_set \spill_regs
    .else
    sentinels_set PLAIN_LIVE
    .endif
.endm

.macro live_check shape, spill_regs:vararg
    .ifc \shape, spill
    sentinels_check \spill_regs
    .else
    sentinels_check PLAIN_LIVE
    .endif
.endm

.macro frame_push shape
    .ifc \shape, spill
    stp x19, x20, [sp, #-80]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    .endif
    .ifc \shape, lr_base
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    .endif
    .ifc \shape, lr_source
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    .endif
    .ifc \shape, lr_result
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    .endif
.endm

.macro frame_pop shape
    .ifc \shape, spill
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x19, x20, [sp], #80
    .endif
    .ifc \shape, lr_base
    ldp x29, x30, [sp], #16
    .endif
    .ifc \shape, lr_source
    ldp x29, x30, [sp], #16
    .endif
    .ifc \shape, lr_result
    ldp x29, x30, [sp], #16
    .endif
.endm

/* An ARMv8.0 sequence that needs scratch registers saves them below the red zone in a spill form, as a stub does. */
.macro v80_spill shape, frame, a, b, c, d
    .ifc \shape, spill
    stp x\a, x\b, [sp, #-(128 + \frame)]!
    .ifnb \c
    stp x\c, x\d, [sp, #16]
    .endif
    .endif
.endm

.macro v80_unspill shape, frame, a, b, c, d
    .ifc \shape, spill
    .ifnb \c
    ldp x\c, x\d, [sp, #16]
    .endif
    ldp x\a, x\b, [sp], #(128 + \frame)
    .endif
.endm

/* Narrow min/max compares in the operand's own width: the operand extended into w4 once, before the loop. */
.macro v80_minmax_enter op, size, rs
    .ifc \op, smax
    v80_extend \size, \rs, sxt
    .endif
    .ifc \op, smin
    v80_extend \size, \rs, sxt
    .endif
    .ifc \op, umax
    v80_extend \size, \rs, uxt
    .endif
    .ifc \op, umin
    v80_extend \size, \rs, uxt
    .endif
.endm

.macro v80_extend size, rs, kind
    mrs x3, nzcv
    .ifc \size, b
    \kind\()b w4, w\rs
    .endif
    .ifc \size, h
    \kind\()h w4, w\rs
    .endif
.endm

.macro v80_minmax_leave op
    .ifc \op, smax
    msr nzcv, x3
    .endif
    .ifc \op, smin
    msr nzcv, x3
    .endif
    .ifc \op, umax
    msr nzcv, x3
    .endif
    .ifc \op, umin
    msr nzcv, x3
    .endif
.endm

.macro v80_select size, r, rs, cond, sign
    .ifc \size, b
    .ifc \sign, s
    sxtb w17, w5
    cmp w17, w4
    .else
    cmp w5, w4
    .endif
    csel w17, w5, w4, \cond
    .endif
    .ifc \size, h
    .ifc \sign, s
    sxth w17, w5
    cmp w17, w4
    .else
    cmp w5, w4
    .endif
    csel w17, w5, w4, \cond
    .endif
    .ifc \size, w
    cmp w5, w\rs
    csel w17, w5, w\rs, \cond
    .endif
    .ifc \size, x
    cmp x5, x\rs
    csel x17, x5, x\rs, \cond
    .endif
.endm

.macro v80_compute op, size, r, rs
    .ifc \op, add
    add \r\()17, \r\()5, \r\()\rs
    .endif
    .ifc \op, clr
    bic \r\()17, \r\()5, \r\()\rs
    .endif
    .ifc \op, eor
    eor \r\()17, \r\()5, \r\()\rs
    .endif
    .ifc \op, set
    orr \r\()17, \r\()5, \r\()\rs
    .endif
    .ifc \op, smax
    v80_select \size, \r, \rs, gt, s
    .endif
    .ifc \op, smin
    v80_select \size, \r, \rs, lt, s
    .endif
    .ifc \op, umax
    v80_select \size, \r, \rs, hi, u
    .endif
    .ifc \op, umin
    v80_select \size, \r, \rs, lo, u
    .endif
.endm

/* stub_ldop_swp: nzcv in x3, the extended operand in w4, old in x5, status in w16, new in x17. */
.macro v80_ldop op, size, ldx, stx, r, rs, rt, rn
    v80_minmax_enter \op, \size, \rs
1:
    \ldx \r\()5, [x\rn]
    .ifc \op, swp
    \stx w16, \r\()\rs, [x\rn]
    .else
    v80_compute \op, \size, \r, \rs
    \stx w16, \r\()17, [x\rn]
    .endif
    cbnz w16, 1b
    v80_minmax_leave \op
    .ifnc \rt, zr
    mov \r\()\rt, \r\()5
    .endif
.endm

/* stub_cas: old in x5, the compare and status in w16/x16. */
.macro v80_cas size, ldx, stx, r, rs, rt
1:
    \ldx \r\()5, [x0]
    eor \r\()16, \r\()5, \r\()\rs
    .ifc \size, b
    and w16, w16, #0xff
    .endif
    .ifc \size, h
    and w16, w16, #0xffff
    .endif
    cbnz \r\()16, 2f
    \stx w16, \r\()\rt, [x0]
    cbnz w16, 1b
    b 3f
2:
    clrex
3:
    .ifnc \rs, zr
    mov \r\()\rs, \r\()5
    .endif
.endm

/* stub_casp: the old pair in x16/x17, the compare and status in x3/x8; a failed compare stores the old pair back. */
.macro v80_casp ldx, stx, r
1:
    \ldx \r\()16, \r\()17, [x0]
    eor \r\()3, \r\()16, \r\()4
    eor \r\()8, \r\()17, \r\()5
    orr \r\()3, \r\()3, \r\()8
    cbnz \r\()3, 2f
    \stx w3, \r\()6, \r\()7, [x0]
    cbnz w3, 1b
    b 3f
2:
    \stx w3, \r\()16, \r\()17, [x0]
    cbnz w3, 1b
3:
    mov \r\()4, \r\()16
    mov \r\()5, \r\()17
.endm

/* LD<op> and SWP: operand x1, result x2, base x0, unless the shape moves one of them. */
.macro ldop_site name, op, size, shape, insn, ldx, stx, r, rs, rt, rn
L_site_\name:
#if defined(__ARM_FEATURE_ATOMICS)
    \insn \r\()\rs, \r\()\rt, [x\rn]
#else
    v80_spill \shape, 48, 3, 4, 5, 8
    .ifc \shape, spill
    stp x16, x17, [sp, #32]
    .endif
    v80_ldop \op, \size, \ldx, \stx, \r, \rs, \rt, \rn
    .ifc \shape, spill
    ldp x16, x17, [sp, #32]
    .endif
    v80_unspill \shape, 48, 3, 4, 5, 8
#endif
.endm

.macro ldop_form name, op, size, shape, insn, ldx, stx, r
    frame_push \shape
    live_set \shape, SPILL_LIVE_LD
    .ifc \shape, lr_base
    mov x30, x0
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 2, 30
    .endif
    .ifc \shape, lr_source
    mov x30, x1
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 30, 2, 0
    .endif
    .ifc \shape, lr_result
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 30, 0
    mov x2, x30
    .endif
    .ifc \shape, rt_zr
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, zr, 0
    mov x2, #0
    .endif
    .ifc \shape, rt_rs
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 1, 0
    mov x2, x1
    .endif
    .ifc \shape, rt_rn
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 0, 0
    mov x2, x0
    .endif
    .ifc \shape, plain
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 2, 0
    .endif
    .ifc \shape, spill
    ldop_site \name, \op, \size, \shape, \insn, \ldx, \stx, \r, 1, 2, 0
    .endif
    live_check \shape, SPILL_LIVE_LD
    frame_pop \shape
    ret
.endm

/* CAS: the compare value copied into x3 (Rs, which receives the old value), the new value in x2. */
.macro cas_site name, size, shape, insn, ldx, stx, r, rs, rt
L_site_\name:
#if defined(__ARM_FEATURE_ATOMICS)
    \insn \r\()\rs, \r\()\rt, [x0]
#else
    v80_spill \shape, 16, 5, 16
    v80_cas \size, \ldx, \stx, \r, \rs, \rt
    v80_unspill \shape, 16, 5, 16
#endif
.endm

.macro cas_form name, size, shape, insn, ldx, stx, r
    frame_push \shape
    live_set \shape, SPILL_LIVE_CAS
    .ifc \shape, rs_zr
    cas_site \name, \size, \shape, \insn, \ldx, \stx, \r, zr, 2
    mov x2, #0
    .else
    mov x3, x1
    .ifc \shape, rt_zr
    cas_site \name, \size, \shape, \insn, \ldx, \stx, \r, 3, zr
    .endif
    .ifc \shape, rt_rs
    cas_site \name, \size, \shape, \insn, \ldx, \stx, \r, 3, 3
    .endif
    .ifc \shape, plain
    cas_site \name, \size, \shape, \insn, \ldx, \stx, \r, 3, 2
    .endif
    .ifc \shape, spill
    cas_site \name, \size, \shape, \insn, \ldx, \stx, \r, 3, 2
    .endif
    mov x2, x3
    .endif
    live_check \shape, SPILL_LIVE_CAS
    frame_pop \shape
    ret
.endm

/* CASP: the compare pair in x4/x5, the new pair in x6/x7; the old pair goes to old[] and x0 returns 0. */
.macro casp_form name, shape, insn, ldx, stx, r
    frame_push \shape
    ldp x4, x5, [x1]
    ldp x6, x7, [x1, #16]
    live_set \shape, SPILL_LIVE_CASP
L_site_\name:
#if defined(__ARM_FEATURE_ATOMICS)
    \insn \r\()4, \r\()5, \r\()6, \r\()7, [x0]
#else
    v80_spill \shape, 32, 3, 8, 16, 17
    v80_casp \ldx, \stx, \r
    v80_unspill \shape, 32, 3, 8, 16, 17
#endif
    stp x4, x5, [x2]
    mov x2, #0
    live_check \shape, SPILL_LIVE_CASP
    frame_pop \shape
    ret
.endm

.macro form name, op, size, order, shape, insn, ldx, stx, r
    .text
    .p2align 2
    .globl _switch_atomic_\name
_switch_atomic_\name:
    .ifc \op, casp
    casp_form \name, \shape, \insn, \ldx, \stx, \r
    .else
    .ifc \op, cas
    cas_form \name, \size, \shape, \insn, \ldx, \stx, \r
    .else
    ldop_form \name, \op, \size, \shape, \insn, \ldx, \stx, \r
    .endif
    .endif
.endm

#define SWITCH_ATOMIC(name, op, size, order, shape, insn, ldx, stx, r) \
    form name, op, size, order, shape, insn, ldx, stx, r
#include "switch_atomics_list.h"
#undef SWITCH_ATOMIC

    .section __DATA,__const
    .p2align 3
    .globl _switch_atomic_sites
_switch_atomic_sites:
#define SWITCH_ATOMIC(name, op, size, order, shape, insn, ldx, stx, r) .quad L_site_##name
#include "switch_atomics_list.h"
#undef SWITCH_ATOMIC
