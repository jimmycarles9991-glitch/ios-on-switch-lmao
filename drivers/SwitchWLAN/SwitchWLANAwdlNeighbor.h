// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <net/kpi_interface.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int switch_wlan_awdl_neighbor_update(ifnet_t ifp, const uint8_t address[6], int present, int8_t rssi,
                                     uint32_t *route_flags);

#if DEVELOPMENT || DEBUG
enum { SWITCH_WLAN_IPV6_TEXT_LEN = 46 };
const char *switch_wlan_awdl_ipv6_text(const uint8_t address[16], char text[SWITCH_WLAN_IPV6_TEXT_LEN]);
struct switch_wlan_echo_counters {
    uint64_t input_requests, output_replies;
    int stats_error, neighbor_error, neighbor_state;
};
void switch_wlan_awdl_echo_counters(ifnet_t ifp, const uint8_t peer_ip[16], struct switch_wlan_echo_counters *out);
#endif
#ifdef __cplusplus
}
#endif
