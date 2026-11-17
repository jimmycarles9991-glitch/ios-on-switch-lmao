// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_slot_mask.h"
#include "../raw/runtime/raw_slot_snapshots.h"
#include "../raw/runtime/raw_xr_pass.h"
#include <type_traits>
#include <vector>

namespace {

using Wide = raw::SlotMask<128>;

static_assert(std::is_same_v<raw::SlotMask<31>, uint32_t> && std::is_same_v<raw::SlotMask<32>, uint32_t>,
              "a table of 32 slots or fewer keeps its one word");
static_assert(std::is_same_v<Wide, raw::WideSlotMask<4>>);

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

std::vector<uint32_t> slotsOf(const Wide &mask)
{
    std::vector<uint32_t> out;
    raw::forEachSlot(mask, [&](uint32_t slot) {
        out.push_back(slot);
    });
    return out;
}

TEST(SlotMask, AWideMaskNamesEverySlotOfItsWords)
{
    Wide mask{};
    for (uint32_t slot : {127u, 0u, 32u, 31u, 64u})
        raw::addSlot(mask, slot);
    EXPECT_EQ(slotsOf(mask), (std::vector<uint32_t>{0, 31, 32, 64, 127}));
    EXPECT_EQ(raw::slotCount(mask), 5u);
    EXPECT_TRUE(raw::hasSlot(mask, 64));
    EXPECT_FALSE(raw::hasSlot(mask, 63));
    EXPECT_EQ(raw::slotCount(raw::firstSlots<Wide>(128)), 128u);
    EXPECT_EQ(slotsOf(raw::firstSlots<Wide>(33)).back(), 32u);
    EXPECT_EQ(raw::firstSlots<uint32_t>(31), 0x7fffffffu);
    EXPECT_EQ(raw::firstSlots<uint32_t>(32), ~0u);
    EXPECT_TRUE((raw::everySlot<Wide>() & raw::oneSlot<Wide>(100)) == raw::oneSlot<Wide>(100));
    EXPECT_FALSE(raw::anySlot(Wide{}));
}

// The capture after a bind at slot 40 takes that slot's chunk anew and every other slot from the capture before
TEST(SlotMask, AWideObjectTablesCaptureCopiesTheEditedChunkAndKeepsTheRest)
{
    raw::ObjectSnapshots<128> table;
    table.clear();
    for (uint32_t slot : {3u, 40u, 127u})
        ASSERT_TRUE(table.bind(slot, object(100 + slot)));
    EXPECT_FALSE(table.bind(128, object(1))) << "no slot 128";
    const uint32_t first = table.capture();
    ASSERT_TRUE(table.bind(40, object(7)));
    const uint32_t second = table.capture();
    EXPECT_TRUE(table.changedFrom(first, second) == raw::oneSlot<Wide>(40));
    const auto view = table[second];
    EXPECT_EQ(view.objects[3], object(103));
    EXPECT_EQ(view.objects[40], object(7));
    EXPECT_EQ(view.objects[127], object(227));
    EXPECT_EQ(table[first].objects[40], object(140));
    EXPECT_EQ(slotsOf(view.set), (std::vector<uint32_t>{3, 40, 127}));
    ASSERT_TRUE(table.bind(127, object(8)));
    const uint32_t third = table.capture();
    EXPECT_EQ(table[third].objects[127], object(8));
    EXPECT_EQ(table[third].objects[40], object(7));
    EXPECT_TRUE(table.changedFrom(first, third) == raw::everySlot<Wide>()) << "not the capture next after";
}

// A variant reads fragment slots 0 to 15 through header pairs; every other slot a two-plane texture is sampled at is its
// stand-in's, 24B119's slots past 31 among them
TEST(SlotMask, TwoPlaneSlotsPast15AreSampledThroughStandIns)
{
    Wide sampled{};
    for (uint32_t slot : {15u, 16u, 64u, 127u})
        raw::addSlot(sampled, slot);
    EXPECT_EQ(raw::xrPairSlots(sampled), uint16_t(1u << 15));
    EXPECT_EQ(slotsOf(raw::pastXrPairSlots(sampled)), (std::vector<uint32_t>{16, 64, 127}));
    const uint32_t narrow = 1u << 15 | 1u << 16 | 1u << 30;
    EXPECT_EQ(raw::xrPairSlots(narrow), uint16_t(1u << 15));
    EXPECT_EQ(raw::pastXrPairSlots(narrow), 1u << 16 | 1u << 30);
    EXPECT_TRUE(raw::xrPairSlot(1u << 15, 15));
    EXPECT_FALSE(raw::xrPairSlot(0xffff, 64)) << "a slot past the pairs is never one";
}

}  // namespace
