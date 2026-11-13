// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "../raw/runtime/raw_key_tally.h"
#include "../raw/runtime/raw_storage_form.h"

namespace {

struct Attachment {
    uint32_t bpp = 8;
    uint32_t samples = 1;
    bool compressed = false;  // Metal's lossless-compression flag: how m2v clears it, not how its memory is kept
    bool comptags = false;
};

// Core Animation's XR offscreen targets (RGB10A8_2P_XR10, 8 bytes a texel) carry Metal's compression flag; their memory
// is pitch or plain block-linear, which the storage form's RG32_UINT view reads as written
TEST(StorageForm, AnXrTargetMetalCallsCompressedTakesItsStorageView)
{
    Attachment xr;
    xr.compressed = true;
    EXPECT_EQ(raw::storageViewMismatch(8, 1, xr), raw::StorageFallback::None);
}

TEST(StorageForm, MemoryWithCompressionTagsTakesTheBarrierForm)
{
    Attachment backing;
    backing.comptags = true;
    EXPECT_EQ(raw::storageViewMismatch(8, 1, backing), raw::StorageFallback::Compressed);
}

TEST(StorageForm, AnotherTexelSizeOrSampleCountTakesTheBarrierForm)
{
    Attachment target;
    target.bpp = 4;
    EXPECT_EQ(raw::storageViewMismatch(8, 1, target), raw::StorageFallback::TexelBytes);
    target.bpp = 8;
    target.samples = 4;
    EXPECT_EQ(raw::storageViewMismatch(8, 1, target), raw::StorageFallback::Samples);
    EXPECT_EQ(raw::storageViewMismatch(8, 4, target), raw::StorageFallback::None);
}

// PBGRABsovXm_TimgA2Xhf_IsrcM1: blends c0 in the ROP, samples memoryless c1 as an image, writes no c1
TEST(StorageForm, AStateFetchingOnlyAColorItNeverWritesTakesItsBarrierForm)
{
    EXPECT_TRUE(raw::fetchesOnlyUnwritten(0b10, 0b01, 0, 0b10, 0b01));
    EXPECT_TRUE(raw::fetchesOnlyUnwritten(0b01, 0b10, 0, 0b01, 0b10));  // ..._IsrcM0 under S1: reads c0, writes c1
}

// PBGRAXmw_A3Xghfc fetches and writes c0..c2 in its storage form (storageWrite), its twin through the ROP; a fragment
// blend fetches the color it writes
TEST(StorageForm, AStateFetchingWhatItWritesKeepsTheInterlock)
{
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0b111, 0, 0b111, 0b111, 0b111));
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0b01, 0, 0b01, 0b01, 0b01));
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0b11, 0b01, 0, 0b11, 0b01));  // reads c0 and c1, writes c0
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0b10, 0b01, 0, 0b10, 0b11));  // its twin writes what it fetches
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0b10, 0b01, 0, 0b11, 0b01));  // its twin fetches more
    EXPECT_FALSE(raw::fetchesOnlyUnwritten(0, 0b01, 0, 0, 0b01));
}

TEST(StorageForm, EveryFallbackHasALogName)
{
    for (size_t r = 0; r < raw::kStorageFallbacks; r++)
        EXPECT_NE(raw::kStorageFallbackNames[r], nullptr);
}

TEST(KeyTally, TakesTheMostCountedKeysFirstAndStartsAgain)
{
    raw::KeyTally tally;
    for (uint64_t key = 1; key <= 12; key++)
        for (uint64_t n = 0; n < key; n++)
            tally.note(key << 20);
    const auto top = tally.take(8);
    ASSERT_EQ(top.size(), 8u);
    EXPECT_EQ(top.front(), (std::pair<uint64_t, uint64_t>{12ull << 20, 12}));
    EXPECT_EQ(top.back(), (std::pair<uint64_t, uint64_t>{5ull << 20, 5}));
    EXPECT_TRUE(tally.take(8).empty());
    tally.note(3ull << 20);
    EXPECT_EQ(tally.take(8), (std::vector<std::pair<uint64_t, uint64_t>>{{3ull << 20, 1}}));
}

TEST(KeyTally, CountsEveryNoteFromManyThreads)
{
    raw::KeyTally tally;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&] {
            for (uint64_t i = 0; i < 4000; i++)
                tally.note(1 + i % 16);
        });
    for (auto &thread : threads)
        thread.join();
    uint64_t total = 0;
    for (const auto &[key, count] : tally.take(raw::KeyTally::kSlots))
        total += count;
    EXPECT_EQ(total + tally.overflow.load(), 16000u);
    EXPECT_EQ(tally.overflow.load(), 0u);
}

TEST(KeyTally, AFullTableCountsTheRestAsOverflow)
{
    raw::KeyTally tally;
    for (uint64_t key = 1; key <= raw::KeyTally::kSlots + 3; key++)
        tally.note(key);
    EXPECT_EQ(tally.overflow.load(), 3u);
}

}  // namespace
