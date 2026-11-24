/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_control_test_start,"ax",%progbits
.global switch_control_test_start
.type switch_control_test_start,%function
.thumb_func
switch_control_test_start:
    ldr sp, =0xfff000
    bl switch_control_test_run
1:  wfi
    b 1b
