/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.cpu cortex-r4
.text
.global switch_test_start
.global switch_test_heap_return
.global switch_test_heap
.global switch_test_register
.thumb_func
switch_test_start:
    cpsid if
    ldr sp, =0x23f000
    ldr r0, =0x20e670
    ldr r1, =0x110000
    ldr r1, [r1]
    mov r2, r1
    ldr r4, =0x44444444
    b.w 0x1e006c
.thumb_func
switch_test_heap:
    ldr r3, =0x110100
    stmia r3, {r0-r2}
    str r4, [r3, #12]
    str lr, [r3, #24]
    movw r0, #0x777
    bx lr
.thumb_func
switch_test_heap_return:
    ldr r3, =0x110100
    str r4, [r3, #16]
    mov r2, sp
    str r2, [r3, #20]
    sub sp, #104
    add r1, sp, #68
    movw r0, #0x4404
    movs r2, #8
1:
    str r0, [r1], #4
    adds r0, #1
    subs r2, #1
    bne 1b
    ldr r0, =switch_test_attach_return
    str r0, [sp, #100]
    ldr r4, =0x110000
    ldr r4, [r4, #4]
    b.w 0x1e9b8e
.thumb_func
switch_test_register:
    ldr r12, =0x110100
    ldr r1, [r12, #68]
    adds r1, #1
    str r1, [r12, #68]
    str r0, [r12, #72]
    str r10, [r12, #76]
    str r9, [r12, #80]
    str r8, [r12, #84]
    add r0, sp, #32
    add r1, r12, #88
    movs r2, #6
2:
    ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, #1
    bne 2b
    ldr r0, =0x110000
    ldr r0, [r0, #8]
    ldmia sp!, {r4-r10, pc}
.thumb_func
switch_test_attach_return:
    ldr r3, =0x110100
    str r0, [r3, #28]
    mov r0, sp
    str r0, [r3, #32]
    add r0, r3, #36
    stmia r0, {r4-r11}
    ldr r0, =0x1df9ac
    ldr r1, =0x20e670
    movs r2, #0
6:
    str r2, [r0], #4
    cmp r0, r1
    blo 6b
    ldr r4, =0x110000
    ldr r2, [r4, #12]
    cmn r2, #1
    beq 3f
    ldr r12, [r3, #88]
    cmp r12, #0
    beq 3f
    sub sp, #24
    movs r0, #0
    str r0, [sp]
    str r0, [sp, #4]
    ldr r0, [r4, #20]
    str r0, [sp, #8]
    ldr r0, [r4, #16]
    str r0, [sp, #12]
    movs r0, #0
    str r0, [sp, #16]
    str r0, [sp, #20]
    ldr r0, [r3, #84]
    ldr r1, [r4, #24]
    cbz r1, 4f
    mov r0, r1
4:
    ldr r1, [r3, #76]
    ldr r3, [r3, #80]
    blx r12
    add sp, #24
    ldr r3, =0x110100
    str r0, [r3, #112]
3:
    ldr r3, =0x110100
    movs r0, #1
    str r0, [r3, #116]
5:
    dsb sy
    wfi
    b 5b
    .ltorg
