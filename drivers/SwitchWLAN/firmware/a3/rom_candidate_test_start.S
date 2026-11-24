/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.cpu cortex-r4
.text
.global switch_rom_test_start
.thumb_func
switch_rom_test_start:
    cpsid if
    ldr sp, =0xfff000
    bl switch_rom_test_run
1:
    b 1b
