/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "abi.h"
.syntax unified
.thumb
.cpu cortex-r4
.text
.global switch_heap_test_start
.thumb_func
switch_heap_test_start:
    cpsid if
    ldr sp, =0xfff000
    ldr r0, =SWITCH_TEST_BEGIN
    ldr r1, =SWITCH_TEST_END
    ldr r2, =SWITCH_TEST_BOUNDARY
    bl switch_a3_heap_init
    ldr r3, =switch_heap_test_results
    ldr r0, =0x53574133
    str r0, [r3, #12]
1:
    b 1b
.global switch_a3_initialize
.thumb_func
switch_a3_initialize:
    bx lr

.section .heap_stub,"ax",%progbits
.thumb_func
switch_heap_test_native:
    ldr r3, =switch_heap_test_results
    stmia r3, {r0-r2}
    bx lr

.data
.balign 4
.global switch_a3_state
switch_a3_state:
    .space SWITCH_A3_CAPS_BYTES
.global switch_heap_test_results
switch_heap_test_results:
    .space 16
