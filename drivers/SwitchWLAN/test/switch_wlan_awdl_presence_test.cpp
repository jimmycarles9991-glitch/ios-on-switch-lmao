// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchWLANAwdlPresence.h"

namespace {
using Change = SwitchWLANAwdlPresence::Change;

SwitchWLANPeerIdentity peer(unsigned index, uint32_t generation = 1)
{
    return {generation, 1, {2, 0, 0, 0, 0, static_cast<uint8_t>(index)}, -53};
}

TEST(SwitchWLANAwdlPresence, AllValidatedPeersCanCommitInOneBurst)
{
    SwitchWLANAwdlPresence slots[32] = {};
    for (unsigned i = 0; i < 32; ++i) {
        const auto desired = peer(i);
        EXPECT_EQ(slots[i].change(&desired), Change::Add);
        EXPECT_FALSE(slots[i].ready(desired));
        EXPECT_EQ(slots[i].complete(Change::Add, desired, &desired), 1);
        EXPECT_TRUE(slots[i].ready(desired));
        EXPECT_EQ(slots[i].change(&desired), Change::None);
    }
}

TEST(SwitchWLANAwdlPresence, VanishedDuringInstallationIsRemovedWithoutPublication)
{
    const auto request = peer(1);
    SwitchWLANAwdlPresence slot = {};
    EXPECT_EQ(slot.complete(Change::Add, request, nullptr), 0);
    EXPECT_FALSE(slot.ready(request));
    EXPECT_EQ(slot.change(nullptr), Change::Remove);
    EXPECT_EQ(slot.complete(Change::Remove, request, nullptr), 0);
    EXPECT_EQ(slot.change(nullptr), Change::None);
}

TEST(SwitchWLANAwdlPresence, FailedInstallStaysPendingAndRotationRequiresFreshRoute)
{
    const auto old = peer(1);
    const auto rotated = peer(1, 2);
    SwitchWLANAwdlPresence slot = {};
    EXPECT_EQ(slot.change(&old), Change::Add);
    EXPECT_FALSE(slot.ready(old));
    EXPECT_EQ(slot.change(&old), Change::Add);
    EXPECT_EQ(slot.complete(Change::Add, old, &old), 1);
    EXPECT_EQ(slot.change(&rotated), Change::Remove);
    EXPECT_FALSE(slot.ready(rotated));
    EXPECT_EQ(slot.complete(Change::Remove, old, &rotated), -1);
    EXPECT_EQ(slot.change(&rotated), Change::Add);
    EXPECT_EQ(slot.complete(Change::Add, rotated, &rotated), 1);
}

TEST(SwitchWLANAwdlPresence, ReusedSlotNeverPublishesEarlierAddress)
{
    const auto first = peer(2), next = peer(3);
    SwitchWLANAwdlPresence slot = {};
    EXPECT_EQ(slot.complete(Change::Add, first, &next), 0);
    EXPECT_EQ(slot.change(&next), Change::Remove);
    EXPECT_FALSE(slot.ready(next));
    EXPECT_EQ(slot.complete(Change::Remove, first, &next), 0);
    EXPECT_EQ(slot.complete(Change::Add, next, &next), 1);
    EXPECT_EQ(slot.change(nullptr), Change::Remove);
}

TEST(SwitchWLANAwdlPresence, RediscoveryOfSameAddressInvalidatesInflightInstallAndBufferedInput)
{
    auto original = peer(9), rediscovered = original;
    ++rediscovered.incarnation;
    EXPECT_FALSE(original.same(rediscovered));
    SwitchWLANAwdlPresence slot = {};
    EXPECT_EQ(slot.complete(Change::Add, original, &rediscovered), 0);
    EXPECT_EQ(slot.change(&rediscovered), Change::Remove);
    EXPECT_FALSE(slot.ready(rediscovered));
    EXPECT_EQ(slot.complete(Change::Remove, original, &rediscovered), 0);
    EXPECT_EQ(slot.change(&rediscovered), Change::Add);
    EXPECT_EQ(slot.complete(Change::Add, rediscovered, &rediscovered), 1);
}
}
