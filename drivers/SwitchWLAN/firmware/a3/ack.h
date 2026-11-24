/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_ACK_H
#define SWITCH_A3_ACK_H

#include <stdint.h>

/* Calls run on the firmware's serialized radio execution context. */
int32_t switch_a3_ack_prepare(void *wlc);
int32_t switch_a3_ack_configure(void *wlc, const uint8_t address[6], int enabled);
void switch_a3_ack_disable(void);
int32_t switch_a3_ack_reconcile(void *wlc);

/* Only after WLC teardown; ordinary close retains its unbound key reservation. */
void switch_a3_ack_detached(void);

#endif
