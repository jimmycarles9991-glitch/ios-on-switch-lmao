/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_a3_channel_gate,"ax",%progbits
.global switch_a3_channel_gate
.type switch_a3_channel_gate,%function
.thumb_func
switch_a3_channel_gate:
    push {r0-r3, r12, lr}
    bl switch_a3_channel_may_leave
    cbnz r0, 1f
    /* The cleanup path skips the native enable after the blocked retune. */
    ldr r0, [sp]
    bl switch_a3_channel_mac_enable
    pop.w {r0-r3, r12, lr}
    mov r6, r0
    b.w switch_a3_channel_cleanup
1:
    pop.w {r0-r3, r12, lr}
    b.w switch_a3_channel_original
.size switch_a3_channel_gate, .-switch_a3_channel_gate
