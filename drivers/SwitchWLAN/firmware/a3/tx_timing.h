/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_TX_TIMING_H
#define SWITCH_A3_TX_TIMING_H
#include <stdint.h>

int switch_a3_tx_timing(uint8_t *frame, uint32_t bytes, uint64_t now, uint64_t expiry, uint32_t *lifetime);

#endif
