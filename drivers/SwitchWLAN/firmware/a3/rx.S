/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_a3_rx_entry,"ax",%progbits
.global switch_a3_rx_entry
.type switch_a3_rx_entry,%function
.thumb_func
switch_a3_rx_entry:
    push {r0, r1, r2, lr}
    bl switch_a3_rx_consume
    cmp r0, #0
    pop.w {r0, r1, r2, lr}
    beq.w switch_a3_rx_original
    bx lr
.size switch_a3_rx_entry, .-switch_a3_rx_entry
