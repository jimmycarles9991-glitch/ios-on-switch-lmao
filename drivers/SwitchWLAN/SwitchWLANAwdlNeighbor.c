// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/kpi_interface.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#include <netinet6/scope6_var.h>
#if DEVELOPMENT || DEBUG
#include <sys/kpi_socket.h>
#endif
#include "SwitchWLANAwdlNeighbor.h"

// The neighbour cache entry for the peer's link-local address, returned referenced and unlocked when it carries the
// peer's link address.
static int switch_wlan_neighbor_route(struct ifnet *ifp, struct sockaddr_dl *peer, struct rtentry **found,
                                      uint32_t *flags)
{
    struct sockaddr_dl link = {};
    struct sockaddr_in6 ip = {};
    nd6_alt_node_addr_decompose(ifp, (struct sockaddr *)peer, &link, &ip);
    if (in6_setscope(&ip.sin6_addr, ifp, NULL))
        return ENXIO;
    struct rtentry *route = nd6_lookup(&ip.sin6_addr, 0, ifp, 0);
    if (!route)
        return ENOENT;
    const struct sockaddr_dl *gateway = (const struct sockaddr_dl *)(const void *)route->rt_gateway;
    *flags = route->rt_flags;
    const int match = route->rt_ifp == ifp && gateway && gateway->sdl_family == AF_LINK &&
                      gateway->sdl_index == peer->sdl_index && gateway->sdl_alen == ETHER_ADDR_LEN &&
                      gateway->sdl_len >= offsetof(struct sockaddr_dl, sdl_data) + gateway->sdl_nlen + ETHER_ADDR_LEN &&
                      !memcmp(CONST_LLADDR(gateway), LLADDR(peer), ETHER_ADDR_LEN);
    RT_UNLOCK(route);
    if (!match) {
        rtfree(route);
        return EADDRNOTAVAIL;
    }
    *found = route;
    return 0;
}

static int switch_wlan_neighbor_gone(struct ifnet *ifp, struct sockaddr_dl *peer, uint32_t *flags)
{
    struct rtentry *route = NULL;
    const int match = switch_wlan_neighbor_route(ifp, peer, &route, flags);
    if (match == ENOENT || match == EADDRNOTAVAIL)
        return 0;
    if (!match)
        rtfree(route);
    return match ? match : EBUSY;
}

int switch_wlan_awdl_neighbor_update(ifnet_t ifp, const uint8_t address[6], int present, int8_t rssi,
                                     uint32_t *route_flags)
{
    struct sockaddr_dl peer = {};
    peer.sdl_len = sizeof peer;
    peer.sdl_family = AF_LINK;
    peer.sdl_type = IFT_ETHER;
    peer.sdl_index = (uint16_t)ifnet_index(ifp);
    peer.sdl_alen = ETHER_ADDR_LEN;
    memcpy(LLADDR(&peer), address, ETHER_ADDR_LEN);
    *route_flags = 0;
    struct rtentry *route = NULL;
    if (present) {
        uint8_t service[48] = {};
        int error = ifnet_notice_node_presence(ifp, (struct sockaddr *)&peer, rssi, 0, 0, service);
        // DLIL also returns success for an unchanged, including static, neighbor.
        if (error && error != EEXIST)
            return error;
        error = switch_wlan_neighbor_route(ifp, &peer, &route, route_flags);
        if (!error)
            rtfree(route);
        return error;
    }
    int match = switch_wlan_neighbor_route(ifp, &peer, &route, route_flags);
    if (match == ENOENT || match == EADDRNOTAVAIL)
        return 0;
    if (match)
        return match;
    rtfree(route);
    if (*route_flags & RTF_STATIC)
        return 0;
    const int error = ifnet_notice_node_absence(ifp, (struct sockaddr *)&peer);
    if (error)
        return error;
    // DLIL discards nd6_alt_node_absent's result, and that deletes only a cloned host route
    // (IS_DYNAMIC_DIRECT_HOSTROUTE); nd6_lookup creates an uncloned one when no cloning route covers the address.
    match = switch_wlan_neighbor_route(ifp, &peer, &route, route_flags);
    if (match == ENOENT || match == EADDRNOTAVAIL)
        return 0;
    if (match)
        return match;
    if ((*route_flags & (RTF_HOST | RTF_LLINFO | RTF_GATEWAY | RTF_STATIC)) == (RTF_HOST | RTF_LLINFO))
        nd6_free(route);
    rtfree(route);
    return switch_wlan_neighbor_gone(ifp, &peer, route_flags);
}

#if DEVELOPMENT || DEBUG
const char *switch_wlan_awdl_ipv6_text(const uint8_t address[16], char text[SWITCH_WLAN_IPV6_TEXT_LEN])
{
    return inet_ntop(AF_INET6, address, text, SWITCH_WLAN_IPV6_TEXT_LEN);
}

void switch_wlan_awdl_echo_counters(ifnet_t ifp, const uint8_t peer_ip[16], struct switch_wlan_echo_counters *out)
{
    socket_t socket = NULL;
    out->stats_error = sock_socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &socket);
    out->neighbor_error = out->stats_error;
    if (out->stats_error)
        return;
    struct in6_ifreq stats = {};
    snprintf(stats.ifr_name, sizeof stats.ifr_name, "%s%u", ifnet_name(ifp), ifnet_unit(ifp));
    out->stats_error = sock_ioctl(socket, SIOCGIFSTAT_ICMP6, &stats);
    out->input_requests = stats.ifr_ifru.ifru_icmp6stat.ifs6_in_echo;
    out->output_replies = stats.ifr_ifru.ifru_icmp6stat.ifs6_out_echoreply;
    struct in6_nbrinfo_64 peer = {};
    strlcpy(peer.ifname, stats.ifr_name, sizeof peer.ifname);
    memcpy(&peer.addr, peer_ip, sizeof peer.addr);
    out->neighbor_error = sock_ioctl(socket, SIOCGNBRINFO_IN6_64, &peer);
    out->neighbor_state = peer.state;
    sock_close(socket);
}
#endif
