// SPDX-License-Identifier: MIT
// raw/runtime/raw_heap_fit.h: which heap image array holds a texture and its handle-table flags, m2v's rules over
// raw_heap_classes.h's rows. raw_bridge_parity_test (m2v) compares both with the bridge over the domain; these pin the
// rules' cases: a class of the texture's shape and numeric type, a form a heap reads by its raw texels, the half-bits
// storage twin, depth comparison, sample counts, and facts no Metal enumerator has.
#include "../raw/runtime/raw_heap_fit.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <initializer_list>

namespace {

constexpr uint32_t kR8Unorm = 10, kR16Float = 25, kR16Bfloat = 26, kRGBA8 = 70, kDepth32Float = 252;
constexpr uint32_t k2D = 2, k2DMultisample = 4, kTextureBuffer = 9;
constexpr uint32_t kRead = 1, kWrite = 2;

// heap_code.rs's words: an image class of a dimension and a sampled type, and its flags
constexpr uint32_t kImage = 1, kSampler = 2, kDim2D = 1 << 2, kDimBuffer = 4 << 2, kUint = 2 << 7,
                   kMultisampled = 1 << 6, kDepthCompare = 1 << 9, kStorage = 1 << 10, kHalfBits = 3 << 11;

raw::HeapTextureFacts facts(uint32_t format, uint32_t type = k2D, uint32_t usage = kRead, uint32_t samples = 1,
                            std::initializer_list<uint8_t> swizzle = {2, 3, 4, 5})
{
    raw::HeapTextureFacts f = {format, type, samples, usage, 1, 1, {}};
    std::copy(swizzle.begin(), swizzle.end(), f.swizzle);
    return f;
}

bool holds(uint32_t word, const raw::HeapTextureFacts &f)
{
    return raw::heapHolds(word, &f);
}

TEST(RawHeapFit, AClassOfTheTexturesShapeAndNumericTypeHoldsIt)
{
    EXPECT_TRUE(holds(kImage | kDim2D, facts(kRGBA8)));
    EXPECT_FALSE(holds(kImage | kDim2D | kUint, facts(kRGBA8))) << "another numeric type";
    EXPECT_FALSE(holds(kImage | kDimBuffer, facts(kRGBA8, kTextureBuffer))) << "no heap array of texture buffers";
    EXPECT_FALSE(holds(kImage | kDim2D, facts(kRGBA8, k2D, kWrite))) << "a sampled class needs ShaderRead";
    EXPECT_FALSE(holds(kSampler, facts(kRGBA8)));
    EXPECT_FALSE(raw::heapHolds(kImage | kDim2D, nullptr));
}

TEST(RawHeapFit, AFormAHeapReadsByItsRawTexelsIsTheUnsignedClasss)
{
    EXPECT_TRUE(holds(kImage | kDim2D | kUint, facts(kR16Bfloat)));
    EXPECT_FALSE(holds(kImage | kDim2D, facts(kR16Bfloat)));
    EXPECT_TRUE(holds(kImage | kDim2D, facts(kR16Bfloat, k2D, kRead, 1, {3, 2, 4, 5}))) << "swizzled: its own class";
}

TEST(RawHeapFit, TheHalfBitsTwinHoldsAWritableHalfTextureReadUnswizzled)
{
    const uint32_t twin = kImage | kDim2D | kStorage | kHalfBits;
    EXPECT_TRUE(holds(twin, facts(kR16Float, k2D, kWrite)));
    EXPECT_TRUE(holds(twin, facts(kR16Float, k2D, kWrite, 1, {2, 0, 0, 1}))) << "its lacking channels as they read";
    EXPECT_FALSE(holds(twin, facts(kR16Float, k2D, kRead)));
    EXPECT_FALSE(holds(twin, facts(kR16Float, k2D, kWrite, 1, {3, 2, 4, 5})));
    EXPECT_FALSE(holds(twin, facts(kRGBA8, k2D, kWrite)));
}

TEST(RawHeapFit, DepthComparisonAndSampleCountsFollowTheClass)
{
    EXPECT_TRUE(holds(kImage | kDim2D | kDepthCompare, facts(kDepth32Float)));
    EXPECT_FALSE(holds(kImage | kDim2D | kDepthCompare, facts(kRGBA8)));
    EXPECT_FALSE(holds(kImage | kDim2D | kStorage, facts(kDepth32Float, k2D, kWrite))) << "no depth storage";
    EXPECT_TRUE(holds(kImage | kDim2D | kMultisampled, facts(kRGBA8, k2DMultisample, kRead, 4)));
    EXPECT_FALSE(holds(kImage | kDim2D | kMultisampled, facts(kRGBA8, k2DMultisample, kRead, 1)));
    EXPECT_FALSE(holds(kImage | kDim2D, facts(kRGBA8, k2D, kRead, 4)));
}

TEST(RawHeapFit, FlagsAreTheFormatsOwnUnswizzledAndTheOthersSwizzled)
{
    EXPECT_EQ(raw::heapTextureFlags(facts(kRGBA8)), 0x00040000u);
    EXPECT_EQ(raw::heapTextureFlags(facts(kRGBA8, k2D, kRead, 1, {3, 2, 4, 5})), 0u);
    EXPECT_EQ(raw::heapTextureFlags(facts(kR8Unorm, k2D, kRead, 1, {2, 0, 4, 5})),
              raw::heapTextureFlags(facts(kR8Unorm)))
        << "green read as the zero R8 lacks";
    EXPECT_EQ(raw::heapTextureFlags(facts(kR16Bfloat, k2D, kRead, 1, {3, 2, 4, 5})), 0x00390002u);
}

TEST(RawHeapFit, FactsNoMetalEnumeratorHasFitNothingAndCarryNoFlags)
{
    const raw::HeapTextureFacts unknown[] = {facts(9999), facts(kRGBA8, 10), facts(kRGBA8, k2D, kRead | 0x40),
                                             facts(kRGBA8, k2D, kRead, 1, {2, 3, 4, 6})};
    for (const raw::HeapTextureFacts &f : unknown) {
        EXPECT_FALSE(holds(kImage | kDim2D, f));
        EXPECT_EQ(raw::heapTextureFlags(f), 0u);
    }
}

}  // namespace
