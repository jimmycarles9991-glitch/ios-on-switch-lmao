/* What cpu.atomics runs besides the forms: the harness's own ARMv8.0 atomics, and the store-buffering litmus pairs.
 * Built with LSE, the two translated litmus rows hold an AL instruction the a64x loader rewrites; built without, they
 * hold the sequence a64x's stub for it runs today. */

/* The harness's own ARMv8.0 atomics: the inverse operations of the bit forms, counters, locks and barriers, never
 * stubbed on any build. orr and bic return the old value, add the new one. */
.macro v80_helper name, ldx, stx, r, op, result
    .text
    .p2align 2
    .globl _switch_atomics_v80_\name
_switch_atomics_v80_\name:
1:
    \ldx \r\()2, [x0]
    \op \r\()3, \r\()2, \r\()1
    \stx w4, \r\()3, [x0]
    cbnz w4, 1b
    mov x0, x\result
    ret
.endm

v80_helper orr_b, ldaxrb, stlxrb, w, orr, 2
v80_helper orr_h, ldaxrh, stlxrh, w, orr, 2
v80_helper orr_w, ldaxr, stlxr, w, orr, 2
v80_helper orr_x, ldaxr, stlxr, x, orr, 2
v80_helper bic_b, ldaxrb, stlxrb, w, bic, 2
v80_helper bic_h, ldaxrh, stlxrh, w, bic, 2
v80_helper bic_w, ldaxr, stlxr, w, bic, 2
v80_helper bic_x, ldaxr, stlxr, x, bic, 2
v80_helper add_x, ldaxr, stlxr, x, add, 3

    .text
    .p2align 2
    .globl _switch_atomics_v80_swap
_switch_atomics_v80_swap:
1:
    ldaxr x2, [x0]
    stlxr w3, x1, [x0]
    cbnz w3, 1b
    mov x0, x2
    ret

    .p2align 2
    .globl _switch_atomics_v80_cas
_switch_atomics_v80_cas:
1:
    ldaxr x3, [x0]
    cmp x3, x1
    b.ne 2f
    stlxr w4, x2, [x0]
    cbnz w4, 1b
    b 3f
2:
    clrex
3:
    mov x0, x3
    ret

    .p2align 2
    .globl _switch_atomics_v80_load
_switch_atomics_v80_load:
    ldar x0, [x0]
    ret

    .p2align 2
    .globl _switch_atomics_v80_store
_switch_atomics_v80_store:
    stlr x1, [x0]
    ret

/* Store buffering (switch_atomics_litmus.c): store 1 to *mine (x0) by an exchange or a compare-and-swap from 0, then
 * load *other (x1) plainly and return it. Two threads, each on the other's pair, both reading 0 is the outcome an
 * acquire-release atomic forbids and a LDAXR/STLXR pair without a barrier after it allows. */
    .text
    .p2align 2
    .globl _switch_sb_llsc_plain
_switch_sb_llsc_plain:
    mov x2, #1
1:
    ldaxr x3, [x0]
    stlxr w4, x2, [x0]
    cbnz w4, 1b
    ldr x0, [x1]
    ret

    .p2align 2
    .globl _switch_sb_llsc_dmb
_switch_sb_llsc_dmb:
    mov x2, #1
1:
    ldaxr x3, [x0]
    stlxr w4, x2, [x0]
    cbnz w4, 1b
    dmb ish
    ldr x0, [x1]
    ret

    .p2align 2
    .globl _switch_sb_swpal
_switch_sb_swpal:
    mov x2, #1
L_sb_site_swpal:
#if defined(__ARM_FEATURE_ATOMICS)
    swpal x2, x3, [x0]
#else
1:
    ldaxr x5, [x0]
    stlxr w16, x2, [x0]
    cbnz w16, 1b
    mov x3, x5
#endif
    ldr x0, [x1]
    ret

    .p2align 2
    .globl _switch_sb_cas_llsc_plain
_switch_sb_cas_llsc_plain:
    mov x2, #1
1:
    ldaxr x3, [x0]
    cbnz x3, 2f
    stlxr w4, x2, [x0]
    cbnz w4, 1b
    b 3f
2:
    clrex
3:
    ldr x0, [x1]
    ret

    .p2align 2
    .globl _switch_sb_cas_llsc_dmb
_switch_sb_cas_llsc_dmb:
    mov x2, #1
1:
    ldaxr x3, [x0]
    cbnz x3, 2f
    stlxr w4, x2, [x0]
    cbnz w4, 1b
    b 3f
2:
    clrex
3:
    dmb ish
    ldr x0, [x1]
    ret

    .p2align 2
    .globl _switch_sb_casal
_switch_sb_casal:
    mov x2, #1
    mov x3, #0
L_sb_site_casal:
#if defined(__ARM_FEATURE_ATOMICS)
    casal x3, x2, [x0]
#else
1:
    ldaxr x5, [x0]
    eor x16, x5, x3
    cbnz x16, 2f
    stlxr w16, x2, [x0]
    cbnz w16, 1b
    b 3f
2:
    clrex
3:
    mov x3, x5
#endif
    ldr x0, [x1]
    ret

    .section __DATA,__const
    .p2align 3
    .globl _switch_sb_sites
_switch_sb_sites:
    .quad L_sb_site_swpal
    .quad L_sb_site_casal
