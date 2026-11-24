/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_excursion_test_start,"ax",%progbits
.global switch_excursion_test_start
.type switch_excursion_test_start,%function
.thumb_func
switch_excursion_test_start:
    ldr sp, =0xfff000
    bl switch_excursion_test_run
1:  wfi
    b 1b
.size switch_excursion_test_start, .-switch_excursion_test_start
