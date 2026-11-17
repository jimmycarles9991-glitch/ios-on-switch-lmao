// SPDX-License-Identifier: MIT
// What the raw path took over from the bridge, against the bridge: the m2v device's Metal description
// (nmb_device_pixel_format_block) is raw/runtime/raw_metal_formats.h's row and its linear row
// (nmb_device_linear_bytes_per_row) is refused exactly where the raw device answers none; every capability query the
// bridge installed on m2v's device class (nmb_device_answer_selector) has an answer of its kind in raw_device_answers.h, so
// the raw device opens m2v for none of them; a sampler's word (raw_sampler_word.h) is switch_nmb_sampler_word's over every
// enumerator value and one past each, and over the level clamps on and around Metal's grid; which heap image array holds
// a texture and its handle-table flags (raw_heap_fit.h) are switch_nmb_heap_holds' and switch_nmb_heap_texture_flags':
// the flags over every format value below 2048 and every swizzle of the six channel sources and one past them, and
// holding over every format, texture type, usage of the sampled and storage classes, sample count and kind of swizzle
// against each class word of the type's own dimension, plus every class word's field values against a few textures. The m2v device is the host
// bridge over NVK's host twin and hostsim's mock GM20B.
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"
#include "../raw/runtime/raw_device_answers.h"
#include "../raw/runtime/raw_heap_fit.h"
#include "../raw/runtime/raw_metal_formats.h"
#include "../raw/runtime/raw_sampler_word.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstddef>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace {

namespace mf = raw::metal_formats;

const raw::answers::ArgRange *argument(const char *selector, uint64_t value = ~0ull)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, selector) &&
            (value == ~0ull || (value >= k.args[i].first && value <= k.args[i].last)))
            return &k.args[i];
    return nullptr;
}

const raw::answers::Count *count(const char *selector)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    for (size_t i = 0; i < k.countCount; i++)
        if (!strcmp(k.counts[i].selector, selector))
            return &k.counts[i];
    return nullptr;
}

uint64_t linearAlignment(uint64_t format)
{
    const raw::answers::ArgRange *r = argument("minimumLinearTextureAlignmentForPixelFormat:", format);
    return r ? r->value : 0;
}

TEST(RawBridgeParity, TheDescriptionAndTheLinearRowsAreTheBridges)
{
    const char *nvk = getenv("SWITCH_NVK");
    ASSERT_NE(nvk, nullptr);
    char loader[PATH_MAX];
    ASSERT_NE(realpath(nvk, loader), nullptr);
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    NmbDevice *device = nmb_device_open(loader, nullptr);
    ASSERT_NE(device, nullptr) << nmb_last_error();
    size_t described = 0, rows = 0;
    for (uint32_t format = 0; format < 2048; format++) {
        uint64_t block[4] = {};
        const bool bridge = nmb_device_pixel_format_block(device, format, block);
        const mf::Described *d = mf::described(format);
        EXPECT_EQ(bridge, d != nullptr) << format;
        if (bridge && d) {
            described++;
            EXPECT_EQ(block[0], d->bytes) << format;
            EXPECT_EQ(block[1], d->blockW) << format;
            EXPECT_EQ(block[2], d->blockH) << format;
        }
        const uint64_t bridgeRow = nmb_device_linear_bytes_per_row(device, format, 100);
        const uint64_t rawRow = mf::linearRow(100, mf::texelBytes(format), linearAlignment(format));
        rows += rawRow != 0;
        EXPECT_EQ(bridgeRow != 0, rawRow != 0) << format << ": bridge " << bridgeRow << ", raw " << rawRow;
    }
    EXPECT_EQ(described, std::size(mf::kDescribed));
    EXPECT_GT(rows, 150u);
}

TEST(RawBridgeParity, EveryCapabilityQueryTheBridgeAnsweredIsRaws)
{
    size_t asked = 0;
    uint32_t kind = 0;
    for (size_t i = 0; const char *name = nmb_device_answer_selector(i, &kind); i++) {
        asked++;
        const bool flag = kind == NMB_ANSWER_FLAG || kind == NMB_ANSWER_FLAG_ARG;
        if (kind == NMB_ANSWER_COUNT || kind == NMB_ANSWER_FLAG) {
            const raw::answers::Count *c = count(name);
            ASSERT_NE(c, nullptr) << name;
            EXPECT_EQ(c->flag, flag) << name;
        } else {
            const raw::answers::ArgRange *r = argument(name);
            ASSERT_NE(r, nullptr) << name;
            EXPECT_EQ(r->flag, flag) << name;
        }
    }
    EXPECT_GT(asked, 60u);
}

static_assert(sizeof(raw::SamplerFacts) == sizeof(NmbSamplerDescriptor) &&
                  offsetof(raw::SamplerFacts, lodMin) == offsetof(NmbSamplerDescriptor, lod_min),
              "the bridge's sampler descriptor");

struct SamplerParity {
    size_t compared = 0, differ = 0;

    void check(const raw::SamplerFacts &f)
    {
        NmbSamplerDescriptor d;
        memcpy(&d, &f, sizeof d);
        const uint32_t ours = raw::samplerWord(f), bridge = switch_nmb_sampler_word(&d);
        compared++;
        if (ours != bridge && ++differ <= 8)
            ADD_FAILURE() << std::hex << "raw " << ours << ", bridge " << bridge << std::dec << ": filters "
                          << f.minFilter << "/" << f.magFilter << "/" << f.mipFilter << " address " << f.sAddress << "/"
                          << f.tAddress << "/" << f.rAddress << " border " << f.borderColor << " compare " << f.compare
                          << " normalized " << f.normalized << " anisotropy " << f.maxAnisotropy << " lod " << f.lodMin
                          << "-" << f.lodMax;
    }
};

TEST(RawBridgeParity, TheSamplerWordIsTheBridgesOverEveryEnumerator)
{
    SamplerParity p;
    raw::SamplerFacts f{};
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

TEST(RawBridgeParity, TheSamplerWordIsTheBridgesOverTheLevelClamps)
{
    std::vector<float> clamps = {NAN, -INFINITY, INFINITY, -FLT_MAX, FLT_MAX, -0.0f, FLT_MIN, 1e-9f, 1e9f};
    for (int k = -256; k <= 70 * 256; k++)
        clamps.push_back(float(k) / 256.0f);
    for (float c = 0.49f; c < 15.6f; c += 1.0f)
        clamps.insert(clamps.end(), {std::nextafter(c + 0.01f, 0.0f), c + 0.01f, std::nextafter(c + 0.01f, 99.0f)});
    SamplerParity p;
    raw::SamplerFacts f{};
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

static_assert(sizeof(raw::HeapTextureFacts) == sizeof(NmbHeapTextureFacts) &&
                  offsetof(raw::HeapTextureFacts, swizzle) == offsetof(NmbHeapTextureFacts, swizzle),
              "the bridge's texture facts");

struct HeapParity {
    size_t compared = 0, differ = 0;

    void check(uint32_t word, const raw::HeapTextureFacts &f, bool flagsToo)
    {
        const auto *bridgeFacts = reinterpret_cast<const NmbHeapTextureFacts *>(&f);
        const bool ours = raw::heapHolds(word, &f), bridge = switch_nmb_heap_holds(word, bridgeFacts);
        const uint32_t ourFlags = flagsToo ? raw::heapTextureFlags(f) : 0,
                       bridgeFlags = flagsToo ? switch_nmb_heap_texture_flags(bridgeFacts) : 0;
        compared++;
        if ((ours != bridge || ourFlags != bridgeFlags) && ++differ <= 8)
            ADD_FAILURE() << std::hex << "class " << word << " format " << std::dec << f.pixelFormat << " type "
                          << f.textureType << " samples " << f.sampleCount << " usage " << std::hex << f.usage
                          << " swizzle " << int(f.swizzle[0]) << int(f.swizzle[1]) << int(f.swizzle[2])
                          << int(f.swizzle[3]) << ": raw " << ours << "/" << ourFlags << ", bridge " << bridge << "/"
                          << bridgeFlags;
    }
};

TEST(RawBridgeParity, TheHeapTextureFlagsAreTheBridgesOverEveryFormatAndSwizzle)
{
    HeapParity p;
    for (uint32_t format = 0; format < 2048; format++)
        for (uint32_t s = 0; s < 7 * 7 * 7 * 7; s++) {
            raw::HeapTextureFacts f = {format, 2, 1, 1, 1, 1, {}};
            for (int c = 0, v = int(s); c < 4; c++, v /= 7)
                f.swizzle[c] = uint8_t(v % 7);
            p.check(0, f, true);
        }
    EXPECT_EQ(p.differ, 0u);
}

TEST(RawBridgeParity, AHeapArrayHoldsWhatTheBridgesHolds)
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
                        const raw::HeapTextureFacts f = {
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
    const raw::HeapTextureFacts few[] = {{70, 2, 1, 1, 1, 1, {2, 3, 4, 5}},
                                         {25, 2, 1, 2, 1, 1, {2, 3, 4, 5}},
                                         {252, 2, 1, 1, 1, 1, {2, 3, 4, 5}},
                                         {70, 4, 4, 1, 1, 1, {2, 3, 4, 5}},
                                         {26, 3, 1, 3, 1, 1, {2, 3, 4, 5}}};
    for (uint32_t word = 0; word < 1u << 15; word++)
        for (const raw::HeapTextureFacts &f : few) {
            p.check(word, f, false);
            p.check(word | 37u << 13, f, false);
            p.check(word | 1u << 31, f, false);
        }
    EXPECT_EQ(p.differ, 0u);
    EXPECT_GT(p.compared, 50000000u);
}

}  // namespace
