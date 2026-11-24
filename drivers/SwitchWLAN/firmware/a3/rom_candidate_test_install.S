/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The stock firmware's code at RAM 0x1e0344-0x1e0404 (the image loads at 0x180000). */
.syntax unified
.thumb
.text
.balign 2
.global switch_rom_test_install
.type switch_rom_test_install,%function
.thumb_func
switch_rom_test_install:
    .incbin "brcmfmac4356A3-pcie.bin", 0x60344, 0xc0
.size switch_rom_test_install, .-switch_rom_test_install
