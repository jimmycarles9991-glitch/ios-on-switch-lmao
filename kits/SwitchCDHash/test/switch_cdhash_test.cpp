// The /chosen CDHash lists (kits/SwitchCDHash): which bytes make a list, and the binary search over one.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "kits/SwitchCDHash/switch_cdhash.h"

namespace {

using CDHash = std::array<uint8_t, SWITCH_CDHASH_BYTES>;

CDHash filled(uint8_t value)
{
    CDHash h;
    h.fill(value);
    return h;
}

// The list's bytes as adt_edit.py writes them: ascending, each CDHash once.
std::vector<uint8_t> encode(std::set<CDHash> hashes)
{
    std::vector<uint8_t> bytes;
    for (const CDHash &h : hashes)
        bytes.insert(bytes.end(), h.begin(), h.end());
    return bytes;
}

struct switch_cdhash_list list_of(const std::vector<uint8_t> &bytes)
{
    return switch_cdhash_list_of(bytes.data(), bytes.size());
}

TEST(SwitchCDHash, OnlyWholeCDHashesMakeAList)
{
    const std::vector<uint8_t> two = encode({filled(1), filled(2)});
    EXPECT_EQ(switch_cdhash_list_of(nullptr, 40).count, 0u);
    EXPECT_EQ(switch_cdhash_list_of(two.data(), 0).count, 0u);
    EXPECT_EQ(switch_cdhash_list_of(two.data(), 39).count, 0u);
    EXPECT_EQ(switch_cdhash_list_of(two.data(), 41).count, 0u);
    const struct switch_cdhash_list list = list_of(two);
    EXPECT_EQ(list.count, 2u);
    EXPECT_EQ(list.hashes, two.data());
}

TEST(SwitchCDHash, AnEmptyListHasNothing)
{
    const struct switch_cdhash_list empty = switch_cdhash_list_of(nullptr, 0);
    EXPECT_FALSE(switch_cdhash_listed(&empty, filled(0).data()));
}

TEST(SwitchCDHash, EveryMemberIsFoundAndEveryGapIsNot)
{
    for (unsigned size = 1; size <= 40; ++size) {
        std::set<CDHash> hashes;
        for (unsigned i = 0; i < size; ++i)
            hashes.insert(filled(static_cast<uint8_t>(2 * i + 1)));
        const std::vector<uint8_t> bytes = encode(hashes);
        const struct switch_cdhash_list list = list_of(bytes);
        for (unsigned value = 0; value <= 2 * size; ++value) {
            SCOPED_TRACE(testing::Message() << size << " entries, value " << value);
            EXPECT_EQ(switch_cdhash_listed(&list, filled(static_cast<uint8_t>(value)).data()), value % 2 == 1);
        }
    }
}

TEST(SwitchCDHash, AgreesWithASetOnRealisticListsAndNearMisses)
{
    std::mt19937 random(20261108);
    std::set<CDHash> hashes;
    while (hashes.size() < 3523) {
        CDHash h;
        for (uint8_t &byte : h)
            byte = static_cast<uint8_t>(random());
        hashes.insert(h);
    }
    const std::vector<uint8_t> bytes = encode(hashes);
    const struct switch_cdhash_list list = list_of(bytes);
    ASSERT_EQ(list.count, hashes.size());
    for (const CDHash &h : hashes) {
        EXPECT_TRUE(switch_cdhash_listed(&list, h.data()));
        for (const size_t at : {size_t{0}, size_t{SWITCH_CDHASH_BYTES - 1}}) {
            CDHash near = h;
            near[at] ^= 1;
            EXPECT_EQ(switch_cdhash_listed(&list, near.data()), hashes.count(near) == 1);
        }
    }
    for (const uint8_t edge : {uint8_t{0x00}, uint8_t{0xff}})
        EXPECT_EQ(switch_cdhash_listed(&list, filled(edge).data()), hashes.count(filled(edge)) == 1);
}

}  // namespace
