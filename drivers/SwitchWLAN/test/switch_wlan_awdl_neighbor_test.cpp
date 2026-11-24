// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cerrno>
#include "drivers/SwitchWLAN/test/switch_wlan_neighbor_model.h"

extern "C" int switch_wlan_awdl_neighbor_update(struct ifnet *ifp, const uint8_t address[6], int present, int8_t rssi,
                                                uint32_t *route_flags);

namespace {
const uint8_t kPeer[6] = {0x22, 0x9f, 0x35, 0x31, 0x08, 0xe8};

void expectBalanced()
{
    const auto state = switch_neighbor_model_state();
    EXPECT_EQ(state.references, 0u);
    EXPECT_EQ(state.locked, 0u);
}

void addThenRemove(uint32_t created)
{
    struct ifnet *ifp = switch_neighbor_model_reset(created);
    uint32_t flags = 0;
    ASSERT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 1, -60, &flags), 0);
    EXPECT_EQ(flags, created);
    EXPECT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 0, 0, &flags), 0);
    EXPECT_EQ(switch_neighbor_model_flags(kPeer), 0u);
    EXPECT_EQ(switch_neighbor_model_state().routes, 0u);
    expectBalanced();
}

TEST(SwitchWLANAwdlNeighbor, ClonedRouteIsRemovedByTheAbsenceNotice)
{
    addThenRemove(RTF_HOST | RTF_LLINFO | RTF_WASCLONED);
    EXPECT_EQ(switch_neighbor_model_state().freed, 0u);
}

TEST(SwitchWLANAwdlNeighbor, UnclonedHostRouteIsFreedAsANeighbourCacheEntry)
{
    addThenRemove(RTF_HOST | RTF_LLINFO);
    EXPECT_EQ(switch_neighbor_model_state().freed, 1u);
}

TEST(SwitchWLANAwdlNeighbor, RemovingAnAbsentNeighbourSucceeds)
{
    struct ifnet *ifp = switch_neighbor_model_reset(RTF_HOST | RTF_LLINFO);
    uint32_t flags = 1;
    EXPECT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 0, 0, &flags), 0);
    EXPECT_EQ(flags, 0u);
    expectBalanced();
}

TEST(SwitchWLANAwdlNeighbor, StaticRouteStays)
{
    struct ifnet *ifp = switch_neighbor_model_reset(RTF_HOST | RTF_LLINFO | RTF_STATIC);
    uint32_t flags = 0;
    ASSERT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 1, -60, &flags), 0);
    EXPECT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 0, 0, &flags), 0);
    EXPECT_EQ(switch_neighbor_model_state().routes, 1u);
    EXPECT_EQ(switch_neighbor_model_state().freed, 0u);
    expectBalanced();
}

TEST(SwitchWLANAwdlNeighbor, RouteThatOutlivesRemovalIsReportedWithItsFlags)
{
    struct ifnet *ifp = switch_neighbor_model_reset(RTF_HOST | RTF_LLINFO);
    switch_neighbor_model_pin(1);
    uint32_t flags = 0;
    ASSERT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 1, -60, &flags), 0);
    EXPECT_EQ(switch_wlan_awdl_neighbor_update(ifp, kPeer, 0, 0, &flags), EBUSY);
    EXPECT_EQ(flags, static_cast<uint32_t>(RTF_HOST | RTF_LLINFO));
    expectBalanced();
}
}  // namespace
