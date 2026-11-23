// SPDX-License-Identifier: MIT
// What the driver took over from m2v, against its sources of truth: metal_formats.h's description of
// every pixel format below 2048 is Metal's (metal_format_golden.h, Metal's answers as m2v measured them), and a format
// has a linear row exactly where Metal makes one (a described format not of blocks and with no depth or stencil
// aspect, measured by m2v as Metal's minimumLinearTextureAlignmentForPixelFormat: answers); a sampler's
// word (sampler_word.h) is the translator's (metal_rules.rs) over every enumerator value and one past each, and over
// the level clamps on and around Metal's grid; which heap image array holds a texture and its handle-table flags
// (heap_fit.h) are the translator's: the flags over every format value below 2048 and every swizzle of the six channel
// sources and one past them, and holding over every format, texture type, usage of the sampled and storage classes, sample
// count and kind of swizzle against each class word of the type's own dimension, plus every class word's field values
// against a few textures.
#include "metal_format_golden.h"
#include "../device_answers.h"
#include "../heap_fit.h"
#include "../metal_formats.h"
#include "../sampler_word.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

extern "C" {
void metal_rules_layouts(std::array<size_t, 4> *out);
bool metal_rules_heap_holds(uint32_t heapClass, const gm20b::HeapTextureFacts *facts);
uint32_t metal_rules_heap_texture_flags(const gm20b::HeapTextureFacts *facts);
uint32_t metal_rules_sampler_word(const gm20b::SamplerFacts *facts);
bool metal_rules_depth_stencil(uint32_t format);
}

namespace {

namespace mf = gm20b::metal_formats;

const metal_format_golden::Block *metalDescribes(uint32_t format)
{
    const auto *golden =
        std::find_if(std::begin(metal_format_golden::kDescribed), std::end(metal_format_golden::kDescribed),
                     [&](const metal_format_golden::Block &b) {
                         return b.mtl == format;
                     });
    return golden == std::end(metal_format_golden::kDescribed) ? nullptr : golden;
}

uint64_t linearAlignment(uint64_t format)
{
    const gm20b::answers::Kind &k = gm20b::answers::kThisKind;
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, "minimumLinearTextureAlignmentForPixelFormat:") && format >= k.args[i].first &&
            format <= k.args[i].last)
            return k.args[i].value;
    return 0;
}

TEST(GM20BMetalRules, TheDescriptionIsMetals)
{
    size_t described = 0;
    for (uint32_t format = 0; format < 2048; format++) {
        const mf::Described *d = mf::described(format);
        const metal_format_golden::Block *golden = metalDescribes(format);
        const bool metal = golden != nullptr;
        EXPECT_EQ(metal, d != nullptr) << format;
        if (metal && d) {
            described++;
            EXPECT_EQ(golden->bytes, d->bytes) << format;
            EXPECT_EQ(golden->blockW, d->blockW) << format;
            EXPECT_EQ(golden->blockH, d->blockH) << format;
        }
    }
    EXPECT_EQ(described, std::size(mf::kDescribed));
}

TEST(GM20BMetalRules, AFormatHasALinearRowWhereMetalMakesOne)
{
    size_t rows = 0;
    for (uint32_t format = 0; format < 2048; format++) {
        const metal_format_golden::Block *golden = metalDescribes(format);
        const bool metal = golden && !golden->blocks && !metal_rules_depth_stencil(format);
        const uint64_t gm20bRow = mf::linearRow(100, mf::texelBytes(format), linearAlignment(format));
        rows += gm20bRow != 0;
        EXPECT_EQ(metal, gm20bRow != 0) << format << ": raw " << gm20bRow;
    }
    EXPECT_GT(rows, 150u);
}

TEST(GM20BMetalRules, TheTranslatorTakesRawsStructs)
{
    std::array<size_t, 4> layouts = {};
    metal_rules_layouts(&layouts);
    EXPECT_EQ(layouts[0], sizeof(gm20b::SamplerFacts));
    EXPECT_EQ(layouts[1], offsetof(gm20b::SamplerFacts, lodMin));
    EXPECT_EQ(layouts[2], sizeof(gm20b::HeapTextureFacts));
    EXPECT_EQ(layouts[3], offsetof(gm20b::HeapTextureFacts, swizzle));
}

struct SamplerParity {
    size_t compared = 0, differ = 0;

    void check(const gm20b::SamplerFacts &f)
    {
        const uint32_t ours = gm20b::samplerWord(f), rule = metal_rules_sampler_word(&f);
        compared++;
        if (ours != rule && ++differ <= 8)
            ADD_FAILURE() << std::hex << "raw " << ours << ", translator " << rule << std::dec << ": filters "
                          << f.minFilter << "/" << f.magFilter << "/" << f.mipFilter << " address " << f.sAddress << "/"
                          << f.tAddress << "/" << f.rAddress << " border " << f.borderColor << " compare " << f.compare
                          << " normalized " << f.normalized << " anisotropy " << f.maxAnisotropy << " lod " << f.lodMin
                          << "-" << f.lodMax;
    }
};

TEST(GM20BMetalRules, TheSamplerWordIsTheTranslatorsOverEveryEnumerator)
{
    SamplerParity p;
    gm20b::SamplerFacts f{};
    f.lodMax = FLT_MAX;
    for (f.minFilter = 0; f.minFilter < 4; f.minFilter++)
        for (f.magFilter = 0; f.magFilter < 4; f.magFilter++)
            for (f.mipFilter = 0; f.mipFilter < 4; f.mipFilter++)
                for (f.sAddress = 0; f.sAddress < 7; f.sAddress++)
                    for (f.tAddress = 0; f.tAddress < 7; f.tAddress++)
                        for (f.rAddress = 0; f.rAddress < 7; f.rAddress++)
                            for (f.borderColor = 0; f.borderColor < 4; f.borderColor++)
                                for (f.compare = 0; f.compare < 9; f.compare++)
                                    for (f.normalized = 0; f.normalized < 2; f.normalized++)
                                        for (uint64_t a : {0ull, 1ull, 2ull, 16ull, (1ull << 32) | 1})
                                            f.maxAnisotropy = a, p.check(f);
    f = {};
    for (uint64_t past : {1ull << 32, ~0ull}) {
        f.minFilter = past;
        p.check(f);
        f.minFilter = 0;
        f.compare = past;
        p.check(f);
        f.compare = 0;
    }
    EXPECT_EQ(p.differ, 0u);
    EXPECT_GT(p.compared, 7000000u);
}

TEST(GM20BMetalRules, TheSamplerWordIsTheTranslatorsOverTheLevelClamps)
{
    std::vector<float> clamps = {NAN, -INFINITY, INFINITY, -FLT_MAX, FLT_MAX, -0.0f, FLT_MIN, 1e-9f, 1e9f};
    for (int k = -256; k <= 70 * 256; k++)
        clamps.push_back(float(k) / 256.0f);
    for (float c = 0.49f; c < 15.6f; c += 1.0f)
        clamps.insert(clamps.end(), {std::nextafter(c + 0.01f, 0.0f), c + 0.01f, std::nextafter(c + 0.01f, 99.0f)});
    SamplerParity p;
    gm20b::SamplerFacts f{};
    for (uint64_t mip = 0; mip < 3; mip++) {
        f.mipFilter = mip;
        for (float c : clamps) {
            f.lodMin = c, f.lodMax = FLT_MAX;
            p.check(f);
            f.lodMin = 0, f.lodMax = c;
            p.check(f);
        }
    }
    for (float lo : {NAN, 0.0f, 0.5f, 0.515625f, 7.25f, 64.0f})
        for (float hi : {NAN, 0.0f, 0.5f, 0.515625f, 14.99f, 15.0f, INFINITY})
            f.lodMin = lo, f.lodMax = hi, p.check(f);
    EXPECT_EQ(p.differ, 0u);
}

struct HeapParity {
    size_t compared = 0, differ = 0;

    void check(uint32_t word, const gm20b::HeapTextureFacts &f, bool flagsToo)
    {
        const bool ours = gm20b::heapHolds(word, &f), rule = metal_rules_heap_holds(word, &f);
        const uint32_t ourFlags = flagsToo ? gm20b::heapTextureFlags(f) : 0,
                       ruleFlags = flagsToo ? metal_rules_heap_texture_flags(&f) : 0;
        compared++;
        if ((ours != rule || ourFlags != ruleFlags) && ++differ <= 8)
            ADD_FAILURE() << std::hex << "class " << word << " format " << std::dec << f.pixelFormat << " type "
                          << f.textureType << " samples " << f.sampleCount << " usage " << std::hex << f.usage
                          << " swizzle " << int(f.swizzle[0]) << int(f.swizzle[1]) << int(f.swizzle[2])
                          << int(f.swizzle[3]) << ": raw " << ours << "/" << ourFlags << ", translator " << rule << "/"
                          << ruleFlags;
    }
};

TEST(GM20BMetalRules, TheHeapTextureFlagsAreTheTranslatorsOverEveryFormatAndSwizzle)
{
    HeapParity p;
    for (uint32_t format = 0; format < 2048; format++)
        for (uint32_t s = 0; s < 7 * 7 * 7 * 7; s++) {
            gm20b::HeapTextureFacts f = {format, 2, 1, 1, 1, 1, {}};
            for (int c = 0, v = int(s); c < 4; c++, v /= 7)
                f.swizzle[c] = uint8_t(v % 7);
            p.check(0, f, true);
        }
    EXPECT_EQ(p.differ, 0u);
}

TEST(GM20BMetalRules, AHeapArrayHoldsWhatTheTranslatorsHolds)
{
    // MTLTextureType's dimension, arrayedness and multisampling in a class word's fields
    constexpr uint32_t kShapes[10][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 0, 1},
                                         {3, 0, 0}, {3, 1, 0}, {2, 0, 0}, {1, 1, 1}, {4, 0, 0}};
    constexpr uint8_t kSwizzles[][4] = {{2, 3, 4, 5}, {2, 3, 4, 1}, {2, 0, 0, 1}, {3, 2, 4, 5}, {2, 3, 4, 6}};
    constexpr uint32_t kFormatClasses[][2] = {{0, 0}, {1, 0}, {3, 0}, {2, 37}};
    HeapParity p;
    for (uint32_t format = 0; format < 1024; format++)
        for (uint32_t type = 0; type < 11; type++)
            for (uint32_t usage : {0x1u, 0x2u, 0x3u, 0x10000u, 0x40u})
                for (uint32_t samples : {0u, 1u, 4u})
                    for (const auto &swizzle : kSwizzles) {
                        const gm20b::HeapTextureFacts f = {
                            format, type, samples, usage, 1, 1, {swizzle[0], swizzle[1], swizzle[2], swizzle[3]}};
                        const uint32_t *shape = kShapes[std::min(type, 9u)];
                        for (uint32_t sampled = 0; sampled < 4; sampled++)
                            for (uint32_t compareStorage = 0; compareStorage < 4; compareStorage++)
                                for (const auto &fc : kFormatClasses)
                                    p.check(1 | shape[0] << 2 | shape[1] << 5 | shape[2] << 6 | sampled << 7 |
                                                (compareStorage & 1) << 9 | (compareStorage >> 1) << 10 | fc[0] << 11 |
                                                fc[1] << 13,
                                            f, false);
                    }
    const gm20b::HeapTextureFacts few[] = {{70, 2, 1, 1, 1, 1, {2, 3, 4, 5}},
                                           {25, 2, 1, 2, 1, 1, {2, 3, 4, 5}},
                                           {252, 2, 1, 1, 1, 1, {2, 3, 4, 5}},
                                           {70, 4, 4, 1, 1, 1, {2, 3, 4, 5}},
                                           {26, 3, 1, 3, 1, 1, {2, 3, 4, 5}}};
    for (uint32_t word = 0; word < 1u << 15; word++)
        for (const gm20b::HeapTextureFacts &f : few) {
            p.check(word, f, false);
            p.check(word | 37u << 13, f, false);
            p.check(word | 1u << 31, f, false);
        }
    EXPECT_EQ(p.differ, 0u);
    EXPECT_GT(p.compared, 50000000u);
}

}  // namespace
