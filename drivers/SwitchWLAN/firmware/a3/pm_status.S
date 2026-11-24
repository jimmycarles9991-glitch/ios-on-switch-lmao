/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_a3_pm_status_entry,"ax",%progbits
.global switch_a3_pm_status_entry
.type switch_a3_pm_status_entry,%function
.thumb_func
switch_a3_pm_status_entry:
    push {r0-r3, r12, lr}
    sub sp, sp, #8
    ldr r0, [sp, #12]
    str r0, [sp]
    ldr r0, [sp, #132]
    str r0, [sp, #4]
    ldr r1, [sp, #8]
    ldr r0, [r1]
    ldr r2, [sp, #68]
    ldr r3, [sp, #96]
    bl switch_a3_channel_pm_status
    add sp, sp, #8
    pop {r0-r3, r12, lr}
    b.w switch_a3_pm_status_original
.size switch_a3_pm_status_entry, .-switch_a3_pm_status_entry
