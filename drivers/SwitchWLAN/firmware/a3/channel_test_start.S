/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_channel_test_start,"ax",%progbits
.global switch_channel_test_start
.type switch_channel_test_start,%function
.thumb_func
switch_channel_test_start:
    ldr sp, =0xfff000
    bl switch_channel_test_run
1:  wfi
    b 1b

.global switch_channel_test_invoke
.type switch_channel_test_invoke,%function
.thumb_func
switch_channel_test_invoke:
    push.w {r4-r9, lr}
    sub sp, #28
    movs r6, #6
    bl switch_a3_channel_gate
    add sp, #28
    pop.w {r4-r9, pc}

.global switch_a3_channel_cleanup
.type switch_a3_channel_cleanup,%function
.thumb_func
switch_a3_channel_cleanup:
    mov r2, r6
    bl switch_channel_cleanup_record
    add sp, #28
    pop.w {r4-r9, pc}
