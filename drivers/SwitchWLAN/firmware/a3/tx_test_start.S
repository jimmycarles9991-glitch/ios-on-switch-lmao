/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_tx_test_start,"ax",%progbits
.global switch_tx_test_start
.type switch_tx_test_start,%function
.thumb_func
switch_tx_test_start:
    ldr sp, =0xfff000
    bl switch_tx_test_run
1:  wfi
    b 1b
.size switch_tx_test_start, .-switch_tx_test_start
.global switch_pm_test_entry
.type switch_pm_test_entry,%function
.thumb_func
switch_pm_test_entry:
    push {r4, lr}
    sub sp, sp, #136
    str r1, [sp, #36]
    str r2, [sp, #100]
    movw r3, #0x1148
    str r3, [sp, #64]
    movw r1, #0x1202
    movw r2, #0x2233
    movw r3, #0x4455
    movw r12, #0x6677
    bl switch_a3_pm_status_entry
    add sp, sp, #136
    pop {r4, pc}
.size switch_pm_test_entry, .-switch_pm_test_entry
.global switch_a3_pm_status_original
.type switch_a3_pm_status_original,%function
.thumb_func
switch_a3_pm_status_original:
    push {r4, lr}
    sub sp, sp, #8
    str r12, [sp]
    bl switch_a3_pm_status_record
    add sp, sp, #8
    pop {r4, pc}
.size switch_a3_pm_status_original, .-switch_a3_pm_status_original
