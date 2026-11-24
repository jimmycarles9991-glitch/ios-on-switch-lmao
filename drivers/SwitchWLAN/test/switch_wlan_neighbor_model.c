// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <net/route.h>

struct neighbor {
    int used;
    uint8_t address[6];
    struct rtentry route;
    struct sockaddr_dl gateway;
};

static struct {
    struct ifnet ifp;
    struct neighbor neighbors[4];
    uint32_t created_flags;
    int pinned;
    unsigned references, locked, freed;
} model;

struct ifnet *switch_neighbor_model_reset(uint32_t created_flags)
{
    memset(&model, 0, sizeof model);
    model.ifp.if_index = 7;
    model.created_flags = created_flags;
    return &model.ifp;
}

void switch_neighbor_model_pin(int pinned)
{
    model.pinned = pinned;
}

static struct neighbor *find(const uint8_t address[6])
{
    for (unsigned i = 0; i < 4; ++i)
        if (model.neighbors[i].used && !memcmp(model.neighbors[i].address, address, 6))
            return &model.neighbors[i];
    return NULL;
}

uint32_t switch_neighbor_model_flags(const uint8_t address[6])
{
    const struct neighbor *n = find(address);
    return n ? n->route.rt_flags : 0;
}

struct switch_neighbor_model_state switch_neighbor_model_state(void)
{
    struct switch_neighbor_model_state state = {0, model.references, model.locked, model.freed};
    for (unsigned i = 0; i < 4; ++i)
        state.routes += model.neighbors[i].used != 0;
    return state;
}

uint32_t ifnet_index(ifnet_t ifp)
{
    return ifp->if_index;
}

static const uint8_t *link_address(const struct sockaddr *sa)
{
    const struct sockaddr_dl *sdl = (const struct sockaddr_dl *)(const void *)sa;
    return (const uint8_t *)CONST_LLADDR(sdl);
}

int ifnet_notice_node_presence(ifnet_t ifp, struct sockaddr *sa, int32_t rssi, int lqm, int npm, uint8_t srvinfo[48])
{
    (void)rssi;
    (void)lqm;
    (void)npm;
    (void)srvinfo;
    const uint8_t *address = link_address(sa);
    if (find(address))
        return EEXIST;
    for (unsigned i = 0; i < 4; ++i) {
        struct neighbor *n = &model.neighbors[i];
        if (n->used)
            continue;
        memset(n, 0, sizeof *n);
        n->used = 1;
        memcpy(n->address, address, 6);
        n->gateway.sdl_len = sizeof n->gateway;
        n->gateway.sdl_family = AF_LINK;
        n->gateway.sdl_index = ifp->if_index;
        n->gateway.sdl_alen = ETHER_ADDR_LEN;
        memcpy(LLADDR(&n->gateway), address, 6);
        n->route.rt_flags = model.created_flags;
        n->route.rt_ifp = ifp;
        n->route.rt_gateway = (struct sockaddr *)(void *)&n->gateway;
        return 0;
    }
    return ENOBUFS;
}

// nd6_alt_node_absent deletes only IS_DYNAMIC_DIRECT_HOSTROUTE, and DLIL drops its result.
int ifnet_notice_node_absence(ifnet_t ifp, struct sockaddr *sa)
{
    (void)ifp;
    struct neighbor *n = find(link_address(sa));
    const uint32_t kinds = RTF_CLONING | RTF_PRCLONING | RTF_HOST | RTF_LLINFO | RTF_WASCLONED | RTF_GATEWAY;
    if (n && (n->route.rt_flags & kinds) == (RTF_HOST | RTF_LLINFO | RTF_WASCLONED))
        n->used = 0;
    return 0;
}

void nd6_alt_node_addr_decompose(struct ifnet *ifp, struct sockaddr *sa, struct sockaddr_dl *sdl,
                                 struct sockaddr_in6 *sin6)
{
    (void)ifp;
    (void)sdl;
    memset(sin6, 0, sizeof *sin6);
    sin6->sin6_addr.s6_addr[0] = 0xfe;
    sin6->sin6_addr.s6_addr[1] = 0x80;
    memcpy(sin6->sin6_addr.s6_addr + 10, link_address(sa), 6);
}

int in6_setscope(struct in6_addr *in6, struct ifnet *ifp, uint32_t *zone)
{
    (void)ifp;
    (void)zone;
    in6->s6_addr[3] = 7;
    return 0;
}

struct rtentry *nd6_lookup(struct in6_addr *addr6, int create, struct ifnet *ifp, int rt_locked)
{
    (void)ifp;
    (void)rt_locked;
    if (create)
        return NULL;
    struct neighbor *n = find(addr6->s6_addr + 10);
    if (!n)
        return NULL;
    ++model.references;
    ++model.locked;
    return &n->route;
}

void switch_neighbor_model_unlock(struct rtentry *rt)
{
    (void)rt;
    --model.locked;
}

void rtfree(struct rtentry *rt)
{
    (void)rt;
    --model.references;
}

void nd6_free(struct rtentry *rt)
{
    ++model.freed;
    if (model.locked || model.pinned)
        return;
    for (unsigned i = 0; i < 4; ++i)
        if (&model.neighbors[i].route == rt)
            model.neighbors[i].used = 0;
}
