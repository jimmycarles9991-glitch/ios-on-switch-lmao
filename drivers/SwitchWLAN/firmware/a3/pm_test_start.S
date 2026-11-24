/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.text
.global switch_pm_test_start
.type switch_pm_test_start,%function
.thumb_func
switch_pm_test_start:
    ldr sp, =0xfff000
    bl switch_pm_test_run
1:  wfi
    b 1b

.global switch_pm_test_prepare
.type switch_pm_test_prepare,%function
.thumb_func
switch_pm_test_prepare:
    push {r4-r11, lr}
    sub sp, #4
    movs r4, #4
    movs r5, #5
    movs r6, #6
    movs r7, #7
    movs r1, #8
    mov r8, r1
    adds r1, #1
    mov r9, r1
    adds r1, #1
    mov r10, r1
    adds r1, #1
    mov r11, r1
    ldr r1, =0x1cf695
    blx r1
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
    b 3f
2:  movs r0, #0
    mvns r0, r0
3:  add sp, #4
    pop {r4-r11, pc}

.global switch_pm_test_status
.type switch_pm_test_status,%function
.thumb_func
switch_pm_test_status:
    /* The native 19602c prologue reserves 132 bytes after these nine registers. */
    push {r4-r11, lr}
    sub sp, #132
    mov r5, r0
    mov r10, r1
    mov r9, r3
    str r2, [sp, #36]
    ldr r0, =switch_pm_test_header
    ldr r0, [r0]
    str r0, [sp, #100]
    ldrh r0, [r0]
    str r0, [sp, #64]
    ldr r0, =0x1969ef
    bx r0
.global switch_pm_test_status_return
.type switch_pm_test_status_return,%function
.thumb_func
switch_pm_test_status_return:
    add sp, #132
    pop {r4-r11, pc}
