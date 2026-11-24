/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.cpu cortex-r4
.section .text
.global switch_wlan_rom_start
.thumb_func
switch_wlan_rom_start:
    cpsid if
    ldr r0, =0x180100
    ldr r1, [r0]
    ldr r2, =0x4d525753
    cmp r1, r2
    bne .Linvalid
    ldr r1, [r0, #8]
    ldr r2, [r0, #12]
    cmp r1, #0
    beq .Linvalid
    tst r1, #3
    bne .Linvalid
    movw r3, #0x1004
    cmp r2, r3
    bls .Linvalid
    subs r2, r2, r3
    cmp r1, r2
    bhi .Linvalid
    movs r2, #1
    str r2, [r0, #4]
    movs r2, #0
    ldr r3, =0x181000
    movs r5, #0
.Lcopy:
    ldr r4, [r2]
    str r4, [r3], #4
    eors r5, r4
    adds r2, #4
    cmp r2, r1
    blo .Lcopy
    str r2, [r0, #16]
    str r5, [r0, #20]
    dsb sy
    movs r1, #2
    b .Lpublish
.Linvalid:
    movs r1, #3
.Lpublish:
    str r1, [r0, #4]
    dsb sy
.Lstop:
    wfi
    b .Lstop
    .ltorg
