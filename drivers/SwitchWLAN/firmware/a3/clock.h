/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_CLOCK_H
#define SWITCH_A3_CLOCK_H
#include <stdint.h>

int switch_a3_clock_read(void *wlc, uint64_t *tsf);

#endif
