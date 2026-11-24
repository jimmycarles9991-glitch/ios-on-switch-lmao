// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel headers SwitchWLANAwdlNeighbor.c includes, reduced to what it uses; switch_wlan_neighbor_model.c is the
// kernel behind them.
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "drivers/SwitchWLAN/test/switch_wlan_neighbor_model.h"

struct ifnet {
    uint16_t if_index;
};
typedef struct ifnet *ifnet_t;

struct sockaddr {
    uint8_t sa_len, sa_family;
    char sa_data[14];
};

struct sockaddr_dl {
    uint8_t sdl_len, sdl_family;
    uint16_t sdl_index;
    uint8_t sdl_type, sdl_nlen, sdl_alen, sdl_slen;
    char sdl_data[12];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    uint8_t sin6_len, sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct rtentry {
    uint32_t rt_flags;
    struct ifnet *rt_ifp;
    struct sockaddr *rt_gateway;
};

#define AF_LINK         18
#define IFT_ETHER       0x6
#define ETHER_ADDR_LEN  6
#define LLADDR(s)       ((char *)((s)->sdl_data + (s)->sdl_nlen))
#define CONST_LLADDR(s) ((const char *)((s)->sdl_data + (s)->sdl_nlen))
#define RT_UNLOCK(rt)   switch_neighbor_model_unlock(rt)

uint32_t ifnet_index(ifnet_t ifp);
int ifnet_notice_node_presence(ifnet_t ifp, struct sockaddr *sa, int32_t rssi, int lqm, int npm, uint8_t srvinfo[48]);
int ifnet_notice_node_absence(ifnet_t ifp, struct sockaddr *sa);
void nd6_alt_node_addr_decompose(struct ifnet *ifp, struct sockaddr *sa, struct sockaddr_dl *sdl,
                                 struct sockaddr_in6 *sin6);
int in6_setscope(struct in6_addr *in6, struct ifnet *ifp, uint32_t *zone);
struct rtentry *nd6_lookup(struct in6_addr *addr6, int create, struct ifnet *ifp, int rt_locked);
void rtfree(struct rtentry *rt);
void nd6_free(struct rtentry *rt);
void switch_neighbor_model_unlock(struct rtentry *rt);
