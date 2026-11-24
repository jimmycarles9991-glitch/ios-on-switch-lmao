/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.syntax unified
.thumb
.global switch_excursion_link
.thumb_func
switch_excursion_link:
    b.w switch_a3_excursion_scan_request
