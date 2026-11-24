/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_a3_pm_prepare_entry,"ax",%progbits
.global switch_a3_pm_prepare_entry
.type switch_a3_pm_prepare_entry,%function
.thumb_func
switch_a3_pm_prepare_entry:
    push {r0-r3, r12, lr}
    mov r0, r5
    bl switch_a3_channel_pm_prepare
    cbnz r0, 1f
    pop {r0-r3, r12, lr}
    ldrb r3, [r3, #8]
    cbnz r3, 2f
    b.w switch_a3_pm_prepare_request
1:
    pop {r0-r3, r12, lr}
    b.w switch_a3_pm_prepare_done
2:
    b.w switch_a3_pm_prepare_skip
.size switch_a3_pm_prepare_entry, .-switch_a3_pm_prepare_entry

.section .text.switch_a3_pm_submit_entry,"ax",%progbits
.global switch_a3_pm_submit_entry
.type switch_a3_pm_submit_entry,%function
.thumb_func
switch_a3_pm_submit_entry:
    push {r0-r7, r12, lr}
    sub sp, sp, #16
    mov r2, r3
    bl switch_a3_channel_pm_capture
    mov r4, r0
    add r3, sp, #56
    ldm r3, {r0-r2}
    stm sp, {r0-r2}
    add r3, sp, #16
    ldm r3, {r0-r3}
    bl switch_a3_pm_submit_original
    mov r5, r0
    mov r1, r0
    mov r0, r4
    bl switch_a3_channel_pm_submitted
    str r5, [sp, #16]
    add sp, sp, #16
    pop {r0-r7, r12, pc}
.size switch_a3_pm_submit_entry, .-switch_a3_pm_submit_entry

.section .text.switch_a3_pm_set_entry,"ax",%progbits
.global switch_a3_pm_set_entry
.type switch_a3_pm_set_entry,%function
.thumb_func
switch_a3_pm_set_entry:
    push {r0-r3, r12, lr}
    bl switch_a3_channel_pm_set
    pop {r0-r3, r12, lr}
    ldr.w r3, [r0, #0x120]
    b.w switch_a3_pm_set_continue
.size switch_a3_pm_set_entry, .-switch_a3_pm_set_entry
