// SPDX-License-Identifier: BSD-2-Clause-Patent
// The neighbour cache SwitchWLANAwdlNeighbor.c drives, modelled on xnu-12377's nd6_alt_node_present/absent and nd6_free.
#pragma once
#include <stdint.h>

#define RTF_GATEWAY    0x2
#define RTF_HOST       0x4
#define RTF_CLONING    0x100
#define RTF_LLINFO     0x400
#define RTF_STATIC     0x800
#define RTF_PRCLONING  0x10000
#define RTF_WASCLONED  0x20000

#ifdef __cplusplus
extern "C" {
#endif
struct ifnet;

struct switch_neighbor_model_state {
    unsigned routes, references, locked, freed;
};

struct ifnet *switch_neighbor_model_reset(uint32_t created_flags);
void switch_neighbor_model_pin(int pinned);  // nd6_free leaves every route in place
uint32_t switch_neighbor_model_flags(const uint8_t address[6]);  // 0: no route
struct switch_neighbor_model_state switch_neighbor_model_state(void);
#ifdef __cplusplus
}
#endif
