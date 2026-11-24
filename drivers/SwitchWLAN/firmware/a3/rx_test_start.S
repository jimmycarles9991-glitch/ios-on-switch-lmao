/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_rx_test_start,"ax",%progbits
.global switch_rx_test_start
.type switch_rx_test_start,%function
.thumb_func
switch_rx_test_start:
    ldr sp, =0xfff000
    bl switch_rx_test_run
1:  wfi
    b 1b

.global switch_rx_test_registers
.type switch_rx_test_registers,%function
.thumb_func
switch_rx_test_registers:
    push.w {r4-r11, lr}
    sub sp, #12
    movs r4, #4
    movs r5, #5
    movs r6, #6
    movs r7, #7
    mov.w r8, #8
    mov.w r9, #9
    mov.w r10, #10
    mov.w r11, #11
    bl switch_a3_rx_entry
    movs r0, #0
    cmp r4, #4
    bne 2f
    cmp r5, #5
    bne 2f
    cmp r6, #6
    bne 2f
    cmp r7, #7
    bne 2f
    cmp r8, #8
    bne 2f
    cmp r9, #9
    bne 2f
    cmp r10, #10
    bne 2f
    cmp r11, #11
    bne 2f
    movs r0, #1
2:  add sp, #12
    pop.w {r4-r11, pc}
.size switch_rx_test_registers, .-switch_rx_test_registers
