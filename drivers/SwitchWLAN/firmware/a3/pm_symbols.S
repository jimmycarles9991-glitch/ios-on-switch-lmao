/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.global switch_a3_pm_status_original
.thumb_set switch_a3_pm_status_original, 0x195f31
.global switch_a3_pm_prepare_request
.thumb_set switch_a3_pm_prepare_request, 0x1cf6eb
.global switch_a3_pm_prepare_done
.thumb_set switch_a3_pm_prepare_done, 0x1cf6f3
.global switch_a3_pm_prepare_skip
.thumb_set switch_a3_pm_prepare_skip, 0x1cf701
.global switch_a3_pm_request
.thumb_set switch_a3_pm_request, 0x196e9b
.global switch_a3_pm_effective
.thumb_set switch_a3_pm_effective, 0x18ebaf
.global switch_a3_pm_submit_original
.thumb_set switch_a3_pm_submit_original, 0x19528d
.global switch_a3_pm_set_continue
.thumb_set switch_a3_pm_set_continue, 0x193667
