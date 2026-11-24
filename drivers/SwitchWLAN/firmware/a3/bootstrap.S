/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "abi.h"
.syntax unified
.thumb
.cpu cortex-r4
.section .text.switch_a3_hooks,"ax",%progbits
.global switch_a3_heap_init
.global switch_a3_attach
.extern switch_a3_state
.extern switch_a3_initialize
.extern switch_a3_island_end

.thumb_func
switch_a3_heap_init:
    push {r4, lr}
    ldr r3, =switch_a3_state
    str r1, [r3, #SWITCH_A3_HEAP_END_OFFSET]
#if defined(SWITCH_A3_CANDIDATE) && SWITCH_A3_CANDIDATE
    ldr r4, =SWITCH_A3_ORIGINAL_HEAP_BEGIN
    cmp r0, r4
    bne .Lbounds_failed
    ldr r4, =SWITCH_A3_ORIGINAL_HEAP_END
    cmp r1, r4
    bne .Lbounds_failed
    cmp r2, r1
    bne .Lbounds_failed
#endif
    ldr r4, =switch_a3_island_end
    str r4, [r3, #SWITCH_A3_HEAP_BEGIN_OFFSET]
    ldr r0, =SWITCH_A3_HEAP_MIN_BYTES
    adds r0, r4
    bcs .Lbounds_failed
    cmp r1, r0
    blo .Lbounds_failed
    movs r0, #SWITCH_A3_HEAP_READY
    str r0, [r3, #SWITCH_A3_STATUS_OFFSET]
    mov r0, r4
    ldr r3, =0x1dfb9d
    blx r3
    pop {r4, pc}
.Lbounds_failed:
    movs r0, #SWITCH_A3_BOUNDS_FAILED
    str r0, [r3, #SWITCH_A3_STATUS_OFFSET]
    pop {r4, lr}
.Lpark:
    dsb sy
    wfi
    b .Lpark

.thumb_func
switch_a3_attach:
    push {r0-r3, r12, lr}
    mov r0, r4
    bl switch_a3_initialize
    pop {r0-r3, r12, lr}
    mov r0, r4
    add sp, #68
    ldr pc, =0x1e9b93
    .ltorg
