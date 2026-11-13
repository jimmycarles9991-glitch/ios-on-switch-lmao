// SPDX-License-Identifier: MIT
// The iterated blend setup against the method list x1-units' gpu.x1.iterated_blend arms write through the probe channel
// (hw_test_app/app27/X1IteratedBlendProbe.swift words(), x1-units 571e55258), for NVN's overlay program.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitIterated.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t kAdd = 0x8006, kOne = 0x4001, kOneMinusSourceAlpha = 0x4303;

const std::vector<uint32_t> kOverlay = {
    0x30000421, 0x2000c001, 0x30000621, 0x1000c201, 0x8068c031, 0x0056ac05, 0x00501036,
    0x30581036, 0x3064c806, 0x00601036, 0x10217001, 0x30006401, 0x000dc801,
};
const std::vector<uint16_t> kHalves = {0x3800, 0x3800, 0x3800};

std::vector<uint32_t> emit(const switch_gm20b_iterated_program &b)
{
    std::vector<uint32_t> words(512);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_iterated_blend(&p, &b);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_ITERATED_BLEND_WORDS(b.count, b.constant_count));
    return words;
}

switch_gm20b_iterated_program overlay()
{
    switch_gm20b_iterated_program b = {};
    b.program = kOverlay.data();
    b.count = uint32_t(kOverlay.size());
    b.common = {kAdd, kOne, kOneMinusSourceAlpha, kAdd, kOne, kOneMinusSourceAlpha};
    b.constants = kHalves.data();
    b.constant_count = uint32_t(kHalves.size());
    return b;
}

TEST(EmitIterated, TheSetupIsTheProbesMethodsInItsOrder)
{
    std::vector<Method> expected = {
        {0, 0x12e4, 0},
        {0, 0x133c, 1},
        {0, 0x1340, kAdd},
        {0, 0x1344, kOne},
        {0, 0x1348, kOneMinusSourceAlpha},
        {0, 0x134c, kAdd},
        {0, 0x1350, kOne},
        {0, 0x1358, kOneMinusSourceAlpha},
        {0, 0x1480, 0x3800},
        {0, 0x1484, 0x3800},
        {0, 0x1488, 0x3800},
        {0, 0x02f4, 0},
        {0, 0x0df0, 0},
    };
    for (uint32_t w : kOverlay)
        expected.push_back({0, 0x0df4, w});
    expected.push_back({0, 0x0dd4, uint32_t(kOverlay.size())});
    expected.push_back({0, 0x0dd0, 1});
    EXPECT_EQ(Methods(emit(overlay())), expected);
}

TEST(EmitIterated, AlphaProgramsSetBothEnables)
{
    switch_gm20b_iterated_program b = overlay();
    b.alpha = true;
    b.constant_count = 0;
    const auto m = Methods(emit(b));
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.back(), (Method{0, NVB197_SET_ITERATED_BLEND, 3}));
}

TEST(EmitIterated, ConstantsPastThreeStartTheNextSet)
{
    const std::vector<uint16_t> halves = {1, 2, 3, 4, 5};
    switch_gm20b_iterated_program b = overlay();
    b.constants = halves.data();
    b.constant_count = uint32_t(halves.size());
    std::vector<Method> constants;
    for (const Method &m : Methods(emit(b)))
        if (m.method >= 0x1480 && m.method < 0x1500)
            constants.push_back(m);
    EXPECT_EQ(constants,
              (std::vector<Method>{{0, 0x1480, 1}, {0, 0x1484, 2}, {0, 0x1488, 3}, {0, 0x1490, 4}, {0, 0x1494, 5}}));
}

TEST(EmitIterated, OffIsTheProbesRestore)
{
    std::vector<uint32_t> words(8);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_iterated_blend_off(&p);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_ITERATED_BLEND_OFF_WORDS);
    EXPECT_EQ(Methods(words), (std::vector<Method>{{0, 0x0dd0, 0}, {0, 0x12e4, 1}}));
}

}  // namespace
