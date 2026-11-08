// SPDX-License-Identifier: MIT
// ImageFilterLink.h: Core Image's filter-function link request, as MTLCompiler's front end reads it (21H461).
#include <gtest/gtest.h>
#include "drivers/NativeMetal/ImageFilterLink.h"

namespace {
using switch_metal::ImageFilterArgument;
using switch_metal::ImageFilterFunction;
using switch_metal::imageFilterLinkRequest;

uint32_t u32(const std::vector<uint8_t> &r, size_t at)
{
    uint32_t v;
    memcpy(&v, r.data() + at, 4);
    return v;
}

ImageFilterFunction function(const char *name, const std::vector<uint8_t> &bitcode, size_t arguments)
{
    ImageFilterFunction f;
    f.name = name;
    f.bitcode = bitcode.data();
    f.bitcodeBytes = bitcode.size();
    for (size_t i = 0; i < arguments; i++)
        f.arguments.push_back({3, {i, 10 + i}});
    return f;
}

// CIKernelReflection::reflectConstants links a function to itself: one module, both functions naming it
TEST(ImageFilterLink, AFunctionLinkedToItselfSendsItsBitcodeOnce)
{
    const std::vector<uint8_t> bitcode = {0xde, 0xc0, 0x17, 0x0b, 1, 2, 3, 4, 5, 6, 7};
    const std::vector<uint8_t> r = imageFilterLinkRequest({function("tint", bitcode, 2), function("tint", bitcode, 2)});
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(u32(r, 0x00), 2u);
    EXPECT_EQ(u32(r, 0x08), 1u);
    EXPECT_EQ(u32(r, 0x10), 0u);
    EXPECT_STREQ(reinterpret_cast<const char *>(r.data() + 0x14), "tint");
    EXPECT_STREQ(reinterpret_cast<const char *>(r.data() + 0x19), "tint");
    const uint32_t infos = u32(r, 0x04);
    EXPECT_EQ(infos, 0x14u + 16);
    for (uint32_t i = 0, at = infos; i < 2; i++) {
        EXPECT_EQ(u32(r, at), 0u);
        EXPECT_EQ(u32(r, at + 4), 0u);
        EXPECT_EQ(u32(r, at + 8), 2u);
        at += 12;
        for (uint32_t a = 0; a < 2; a++, at += 12) {
            EXPECT_EQ(u32(r, at), 3u);
            EXPECT_EQ(u32(r, at + 4), a);
            EXPECT_EQ(u32(r, at + 8), 10 + a);
        }
    }
    const uint32_t table = u32(r, 0x0c);
    EXPECT_EQ(table, (infos + 2 * 36 + 7) & ~7u);
    EXPECT_EQ(u32(r, table), table + 8);
    EXPECT_EQ(u32(r, table + 4), bitcode.size());
    EXPECT_EQ(memcmp(r.data() + table + 8, bitcode.data(), bitcode.size()), 0);
    EXPECT_EQ(r.size(), table + 8 + 16);
}

// A Core Image library's functions share one module however they were copied out; other bitcode is a module of its own
TEST(ImageFilterLink, EachDistinctBitcodeIsOneModuleEightAligned)
{
    const std::vector<uint8_t> a = {1, 2, 3, 4, 5}, sameAsA = a, b = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    ImageFilterFunction early = function("_cm1x3lut", b, 3);
    early.earlyOut = true;
    const std::vector<uint8_t> r =
        imageFilterLinkRequest({function("_perspectiveWarp", a, 4), early, function("_hlg_oetf", sameAsA, 1)});
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(u32(r, 0x00), 3u);
    EXPECT_EQ(u32(r, 0x08), 2u);
    const uint32_t infos = u32(r, 0x04);
    EXPECT_EQ(infos % 8, 4u);
    EXPECT_EQ(u32(r, infos + 4), 0u);
    const uint32_t second = infos + 12 + 4 * 12;
    EXPECT_EQ(u32(r, second), 1u);
    EXPECT_EQ(u32(r, second + 4), 1u);
    EXPECT_EQ(u32(r, second + 8), 3u);
    EXPECT_EQ(u32(r, second + 12 + 3 * 12 + 4), 0u);
    const uint32_t table = u32(r, 0x0c);
    EXPECT_EQ(table % 8, 0u);
    const uint32_t first = u32(r, table), next = u32(r, table + 8);
    EXPECT_EQ(first, table + 16);
    EXPECT_EQ(next, first + 8);
    EXPECT_EQ(u32(r, table + 12), b.size());
    EXPECT_EQ(memcmp(r.data() + next, b.data(), b.size()), 0);
    EXPECT_EQ(r.size(), next + 16);
}

TEST(ImageFilterLink, NoFunctionsOrNoBitcodeIsNoRequest)
{
    EXPECT_TRUE(imageFilterLinkRequest({}).empty());
    ImageFilterFunction missing;
    missing.name = "tint";
    EXPECT_TRUE(imageFilterLinkRequest({missing}).empty());
}

}  // namespace
