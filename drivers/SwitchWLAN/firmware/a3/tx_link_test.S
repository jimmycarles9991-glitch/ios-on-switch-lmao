/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.global switch_a3_scheduler_admit
.thumb_func
switch_a3_scheduler_admit:
    bx lr
.global switch_a3_control_retired
.thumb_func
switch_a3_control_retired:
    bx lr
.global switch_a3_control_fault
.thumb_func
switch_a3_control_fault:
    bx lr
.global switch_a3_control_timer_fault
.thumb_func
switch_a3_control_timer_fault:
    bx lr
.global switch_a3_rx_event
.thumb_func
switch_a3_rx_event:
    bx lr
.global switch_a3_timer_initialize_owned
.thumb_func
switch_a3_timer_initialize_owned:
    bx lr
.global switch_a3_timer_arm_owned
.thumb_func
switch_a3_timer_arm_owned:
    bx lr
.global switch_a3_timer_defer_owned
.thumb_func
switch_a3_timer_defer_owned:
    bx lr
.global switch_a3_timer_cancel_owned
.thumb_func
switch_a3_timer_cancel_owned:
    bx lr
