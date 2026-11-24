/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_RX_H
#define SWITCH_A3_RX_H

#include "wire.h"

#define SWITCH_A3_RX_MAGIC        SWITCH_A3_RAW_RX_MAGIC
#define SWITCH_A3_RX_VERSION      SWITCH_A3_RAW_RX_VERSION
#define SWITCH_A3_RX_FCS_PRESENT  1u
#define SWITCH_A3_RX_BUS_HEADROOM 16u
#define SWITCH_A3_RX_BUFFER_BYTES 1920u

#ifdef __cplusplus
extern "C" {
#endif

/* Calls and DMA callbacks run on the firmware's serialized radio execution context. */
int switch_a3_rx_configure(void *wlc, const uint8_t address[6], int enabled, uint32_t session, uint32_t clock_epoch,
                           uint32_t config_generation);
uint32_t switch_a3_rx_pending(void);
int switch_a3_rx_consume(void *wlc, void *packet);
int switch_a3_rx_event(void *wlc, const void *data, uint32_t length);
void switch_a3_rx_entry(void *wlc, void *packet);

#ifdef __cplusplus
}
#endif
#endif
