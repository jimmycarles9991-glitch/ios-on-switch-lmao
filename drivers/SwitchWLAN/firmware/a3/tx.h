/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_TX_H
#define SWITCH_A3_TX_H
#include "wire.h"

struct switch_a3_tx_packet {
    uint16_t id;
    uint8_t private_bytes[6];
    uint8_t *data;
    uint16_t length, flags;
    uint32_t private_word;
    uint16_t next, link;
};

struct switch_a3_tx_fetch {
    struct switch_a3_tx_packet *original;
    uint16_t headroom;
    int16_t offset;
    void (*callback)(struct switch_a3_tx_packet *, struct switch_a3_tx_packet *, void *, int);
    void *context, *next;
};

int switch_a3_tx_configure(void *wlc, const struct switch_a3_configuration *configuration,
                           const struct switch_a3_ring_binding *rings, uint32_t session, uint32_t clock_epoch,
                           uint32_t schedule_generation, uint32_t firmware_generation);
int switch_a3_tx_cancel(void *wlc);
uint32_t switch_a3_tx_credits(void);
uint32_t switch_a3_tx_pending(void);
void switch_a3_tx_wake(void *wlc);
int switch_a3_tx_binding_valid(const struct switch_a3_ring_binding *rings);
int switch_a3_tx_ingress(void *source_device, void *destination_device, void *packets);
int switch_a3_tx_entry(void *source_device, void *destination_device, void *packets);
void switch_a3_tx_radio_status(void *pcb, void *packet, uint32_t status);

#endif
