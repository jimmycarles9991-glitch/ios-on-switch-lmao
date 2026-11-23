// SPDX-License-Identifier: MIT
// ca_selectors.h, the DEVELOPMENT line of the FragmentShaderConfigs Core Animation's generic merge states are
// drawn with: a config is the layout's bytes but its padding (boot 21's stale arena bytes at 49-51 made one config two),
// each config's bytes are logged the first time the process draws it, each window's draws per config are counted in bounded
// memory and a window closes at its end without a later draw, in DEVELOPMENT builds alone.
#include "../ca_selectors.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>

namespace {

// 24B119's (precompile/ca-selectors/24B119.txt)
constexpr gm20b::CaSelectorLayout kLayout = {0, 52, 1ull << 3 | 1ull << 15 | 1ull << 19 | 1ull << 31 | 7ull << 49};
constexpr uint64_t kB0f4 = 0xb0f4f3daabb01e2eull, k64ed = 0x64ed02fd5d7a1865ull;

std::array<uint8_t, 1024> config(uint8_t texture, uint8_t blend)
{
    std::array<uint8_t, 1024> b{};
    b[12] = b[13] = 1;
    b[14] = b[30] = 0xff;
    b[33] = texture;
    b[34] = blend;
    b[36] = 1;
    return b;
}

gm20b::CaSelector selectorOf(const std::array<uint8_t, 1024> &bound)
{
    gm20b::CaSelector s;
    EXPECT_TRUE(gm20b::caSelector(kLayout, bound.data(), bound.size(), s));
    return s;
}

TEST(GM20BCaSelectors, AConfigIsTheLayoutsBytesButItsPadding)
{
    const std::array<uint8_t, 1024> clean = config(1, 3);
    std::array<uint8_t, 1024> stale = clean;
    stale[49] = 0x3c;
    stale[51] = 0x3c;
    stale[52] = stale[1023] = 9;
    const gm20b::CaSelector a = selectorOf(clean), b = selectorOf(stale);
    EXPECT_EQ(a.hash, b.hash);
    EXPECT_EQ(b.bound[49], 0x3c);
    EXPECT_NE(selectorOf(config(3, 3)).hash, a.hash);
    std::array<uint8_t, 1024> lut = clean;
    lut[48] = 1;
    EXPECT_NE(selectorOf(lut).hash, a.hash);
    gm20b::CaSelector s;
    EXPECT_FALSE(gm20b::caSelector(kLayout, nullptr, 1024, s));
    EXPECT_FALSE(gm20b::caSelector(kLayout, clean.data(), 51, s));
    EXPECT_FALSE(gm20b::caSelector(gm20b::CaSelectorLayout{}, clean.data(), clean.size(), s));
}

TEST(GM20BCaSelectors, EachConfigIsToldOnceAndEachWindowCountsItsConfigsAndDraws)
{
    gm20b::CaSelectorLog log;
    gm20b::CaSelectorLog::Events events;
    const gm20b::CaSelector a = selectorOf(config(1, 3)), b = selectorOf(config(3, 3)), c = selectorOf(config(68, 43));
    EXPECT_TRUE(log.note(kB0f4, &a, 1000, events));
    for (uint64_t i = 0; i < 9; i++)
        EXPECT_FALSE(log.note(kB0f4, &a, 1001 + i, events));
    log.note(kB0f4, &b, 2000, events);
    log.note(kB0f4, nullptr, 2001, events);
    EXPECT_TRUE(log.note(k64ed, &a, 3000, events));
    ASSERT_EQ(events.fresh.size(), 3u);
    EXPECT_EQ(events.fresh[0].keyHash, kB0f4);
    EXPECT_EQ(events.fresh[0].ordinal, 1u);
    EXPECT_EQ(events.fresh[1].ordinal, 2u);
    EXPECT_EQ(events.fresh[1].selector.bound[33], 3);
    EXPECT_EQ(events.fresh[2].keyHash, k64ed);
    EXPECT_EQ(events.fresh[2].ordinal, 1u);
    EXPECT_TRUE(events.closed.empty());

    events = {};
    log.flush(1000 + gm20b::CaSelectorLog::kWindowUs, events);
    ASSERT_EQ(events.closed.size(), 1u);
    const gm20b::CaSelectorLog::Window &w = events.closed[0];
    EXPECT_EQ(w.keyHash, kB0f4);
    EXPECT_EQ(w.draws, 12u);
    EXPECT_EQ(w.configs, 2u);
    EXPECT_EQ(w.fresh, 2u);
    EXPECT_EQ(w.most, 10u);
    EXPECT_EQ(w.mostConfig, a.hash);
    EXPECT_EQ(w.unmapped, 1u);
    EXPECT_EQ(w.processConfigs, 2u);
    EXPECT_EQ(w.processDraws, 12u);

    events = {};
    EXPECT_TRUE(log.note(kB0f4, &a, 10 * gm20b::CaSelectorLog::kWindowUs, events));
    log.note(kB0f4, &c, 10 * gm20b::CaSelectorLog::kWindowUs + 1, events);
    ASSERT_EQ(events.closed.size(), 1u);
    EXPECT_EQ(events.closed[0].keyHash, k64ed);
    ASSERT_EQ(events.fresh.size(), 1u);
    EXPECT_EQ(events.fresh[0].ordinal, 3u);
    events = {};
    log.flush(11 * gm20b::CaSelectorLog::kWindowUs, events);
    ASSERT_EQ(events.closed.size(), 1u);
    EXPECT_EQ(events.closed[0].draws, 2u);
    EXPECT_EQ(events.closed[0].configs, 2u);
    EXPECT_EQ(events.closed[0].fresh, 1u);
    EXPECT_EQ(events.closed[0].processConfigs, 3u);
    EXPECT_EQ(events.closed[0].processDraws, 14u);
    events = {};
    log.flush(20 * gm20b::CaSelectorLog::kWindowUs, events);
    EXPECT_TRUE(events.closed.empty());
}

// A process of thousands of configs (an arena of stale bytes the layout did not mask) stays within the bound, still counted
TEST(GM20BCaSelectors, ItsMemoryIsBounded)
{
    gm20b::CaSelectorLog log;
    gm20b::CaSelectorLog::Events events;
    const size_t many = gm20b::CaSelectorLog::kConfigsPerState + 500;
    for (size_t i = 0; i < many; i++) {
        std::array<uint8_t, 1024> bound = config(uint8_t(i), uint8_t(i >> 8));
        const gm20b::CaSelector s = selectorOf(bound);
        log.note(7, &s, 10 + i, events);
    }
    for (uint64_t k = 0; k < 2 * gm20b::CaSelectorLog::kStates; k++)
        log.note(100 + k, nullptr, 10 + many, events);
    EXPECT_EQ(events.fresh.size(), gm20b::CaSelectorLog::kConfigsPerState);
    events = {};
    log.flush(10 + many + gm20b::CaSelectorLog::kWindowUs, events);
    EXPECT_EQ(events.closed.size(), gm20b::CaSelectorLog::kStates);
    for (const gm20b::CaSelectorLog::Window &w : events.closed)
        if (w.keyHash == 7) {
            EXPECT_EQ(w.configs, gm20b::CaSelectorLog::kConfigsPerState);
            EXPECT_EQ(w.beyond, 500u);
            EXPECT_EQ(w.draws, many);
        }
}

// Counted for a generic merge state's draw alone, from the layout the facade gives the device, in DEVELOPMENT builds alone
TEST(GM20BCaSelectors, OnlyAGenericMergeStatesDrawIsCountedInADevelopmentBuild)
{
    const std::string gpu = switch_ios::test::SourceText("../gpu.cpp");
    const size_t prepare = gpu.find("bool RenderEncoder::prepare(Pipeline *pl,");
    ASSERT_NE(prepare, std::string::npos);
    const size_t logged = gpu.find("#if SWITCH_METAL_DIAGNOSTICS\n"
                                   "    if (genericMerge)\n"
                                   "        logCaSelector(*pl);\n"
                                   "#endif",
                                   prepare);
    EXPECT_NE(logged, std::string::npos);
    EXPECT_EQ(gpu.find("logCaSelector(", logged + 60), std::string::npos);
    const std::string variants = switch_ios::test::SourceText("../variants.cpp");
    const size_t defined = variants.find("void RenderEncoder::logCaSelector(");
    ASSERT_NE(defined, std::string::npos);
    EXPECT_LT(variants.rfind("#if SWITCH_METAL_DIAGNOSTICS", defined), defined);
    EXPECT_GT(variants.find("#endif", defined), defined);
    const std::string facade = switch_ios::test::SourceText("../TegraGM20BMetal.mm");
    EXPECT_NE(facade.find("#if SWITCH_METAL_DIAGNOSTICS\n"
                          "    gm20b::gCaSelectorLayout = {gm20b::kCaSelectorSlot, gm20b::kCaSelectorLength, "
                          "gm20b::kCaSelectorPadding};\n"
                          "#endif\n"
                          "    if (!dev->open(packPath, stack, err, role)) {"),
              std::string::npos);
}

}  // namespace
