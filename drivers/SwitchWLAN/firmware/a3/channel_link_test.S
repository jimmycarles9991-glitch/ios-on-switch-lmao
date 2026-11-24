/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.global switch_a3_channel_may_leave
.thumb_func
switch_a3_channel_may_leave:
    movs r0, #1
    bx lr
