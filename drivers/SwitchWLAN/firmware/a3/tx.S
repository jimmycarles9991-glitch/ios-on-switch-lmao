/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.cpu cortex-r4
.thumb
.section .text.switch_a3_tx_entry,"ax",%progbits
.global switch_a3_tx_entry
.type switch_a3_tx_entry,%function
.thumb_func
switch_a3_tx_entry:
    b.w switch_a3_tx_ingress
.size switch_a3_tx_entry, .-switch_a3_tx_entry
