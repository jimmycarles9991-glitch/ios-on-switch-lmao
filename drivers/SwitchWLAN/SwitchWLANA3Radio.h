/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_A3_RADIO_H
#define SWITCH_WLAN_A3_RADIO_H

#include "SwitchWLANAwdlRadio.h"
#include "firmware/a3/abi.h"

void switch_wlan_a3_radio_attach(const switch_a3_caps *caps, const uint8_t mac[6], int d11n, uint16_t firstRing,
                                 unsigned ringCount);
void switch_wlan_a3_radio_lost(void);
void switch_wlan_a3_radio_power(bool on);
int switch_wlan_a3_radio_station_prepare(void);
void switch_wlan_a3_radio_station_resume(void);
bool switch_wlan_a3_radio_caps(swlan_awdl_caps *caps);
bool switch_wlan_a3_radio_present(void);
int switch_wlan_a3_radio_close(void);
int switch_wlan_a3_radio_state(swlan_awdl_state *state);
int switch_wlan_a3_radio_clock(void);
int switch_wlan_a3_radio_refresh(void);
int switch_wlan_a3_radio_rx(swlan_rx_fn fn, void *context);
int switch_wlan_a3_radio_filter(const uint8_t address[6], const uint8_t bssid[6], uint16_t ethertype);
int switch_wlan_a3_radio_schedule(const swlan_chan_seq *schedule);
int switch_wlan_a3_radio_tx(const swlan_tx_req *request);
bool switch_wlan_a3_radio_tx_allowed(const uint8_t *envelope, size_t length);
bool switch_wlan_a3_radio_frame(const uint8_t *frame, size_t length);
void switch_wlan_a3_radio_dma_ready(void);

int switch_wlan_a3_control(const void *request, size_t length, void *reply, size_t capacity, size_t *returned);
int switch_wlan_a3_dma_tx(const uint8_t *envelope, size_t length);
int switch_wlan_a3_transport_idle(void);
void switch_wlan_a3_notify(const swlan_awdl_event *event);

#endif
