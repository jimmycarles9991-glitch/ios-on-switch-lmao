/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_timer_test_start,"ax",%progbits
.global switch_timer_test_start
.type switch_timer_test_start,%function
.thumb_func
switch_timer_test_start:
    ldr sp, =0xfff000
    bl switch_timer_test_run
1:  wfi
    b 1b
