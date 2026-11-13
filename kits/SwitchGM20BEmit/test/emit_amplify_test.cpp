// SPDX-License-Identifier: MIT
// The amplify macros (gen/SwitchGM20BMacros.h) on Mesa's Fermi MME simulator over the scratch words SwitchGM20BMacroSlots.h
// gives them: one call draws every amplification in order, each after its id, viewport offset and layer, every argument
// consumed, a pixel barrier between two amplifications only where the state's fetches need one.
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../SwitchGM20B/SwitchGM20B3D.h"
#include "../../SwitchGM20B/SwitchGM20BTegra.h"
#include "../SwitchGM20BMacroSlots.h"
#include "SwitchGM20BMacros.h"
#include "method_checks.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

using Method = std::pair<uint32_t, uint32_t>;

constexpr uint32_t kLoadOffset = NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kLoad = NVB197_LOAD_CONSTANT_BUFFER(0);
constexpr uint32_t kIdWord = 0x700 + 4 * 5, kCountWord = 0x700 + 4 * 6, kViewportWord = 0x700 + 4 * 7,
                   kLayerWord = 0x700 + 4 * 8;

struct Scratch {
    uint32_t count = 1, countWord = 0, id = kIdWord, viewport = 0, layer = SWITCH_GM20B_AMP_LAYER_FIRST, barrier = 0;
    uint32_t layers[SWITCH_GM20B_AMP_MAX] = {}, viewports[SWITCH_GM20B_AMP_MAX] = {};
};

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    switch_gm20b_test::Scratch scratch;
    std::vector<Method> methods;

    static uint32_t load(void *opaque)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        EXPECT_LT(s.consumed, s.arguments.size());
        return s.consumed < s.arguments.size() ? s.arguments[s.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        const uint32_t first = NVB197_SET_MME_SHADOW_SCRATCH(0);
        EXPECT_GE(method, first) << "a scratch word";
        const uint32_t word = (method - first) / 4;
        EXPECT_GE(word, uint32_t(SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD));
        EXPECT_LT(word, uint32_t(SWITCH_GM20B_SCRATCH_AMP_END));
        return s.scratch.method(method);
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        switch_gm20b_test::expectMaxwellB(method);
        static_cast<Simulation *>(opaque)->methods.emplace_back(method >= kLoad && method < kLoad + 64 ? kLoad : method,
                                                                value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "an amplify macro needs no MME barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "an amplify macro needs no MME memory";
        return nullptr;
    }
};

template <size_t N>
std::vector<Method> run(const uint32_t (&words)[N], const Scratch &scratch, const std::vector<uint32_t> &arguments)
{
    Simulation s;
    s.arguments = arguments;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_COUNT] = scratch.count;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD] = scratch.countWord;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_ID_WORD] = scratch.id;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_VIEWPORT_WORD] = scratch.viewport;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_LAYER] = scratch.layer;
    s.scratch[SWITCH_GM20B_SCRATCH_AMP_BARRIER] = scratch.barrier;
    for (uint32_t a = 0; a < SWITCH_GM20B_AMP_MAX; a++)
        s.scratch[SWITCH_GM20B_SCRATCH_AMP_MAPS + a] = scratch.layers[a] | scratch.viewports[a]
                                                                               << SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT;
    std::vector<mme_fermi_inst> decoded(N);
    mme_fermi_decode(decoded.data(), words, uint32_t(N));
    const mme_sim_state_ops ops = {Simulation::load, Simulation::state, Simulation::emit, Simulation::barrier,
                                   Simulation::dram};
    mme_fermi_sim_core(uint32_t(N), decoded.data(), &ops, &s);
    EXPECT_EQ(s.consumed, arguments.size());
    return s.methods;
}

// What each draw (a BEGIN .. END) followed: the root words loaded since the last draw, by offset, and SET_RT_LAYER's
struct Drawn {
    std::map<uint32_t, uint32_t> words;
    int64_t layer = -1;
    uint32_t first = 0, count = 0, firstMethod = 0;
    bool barrier = false;
};

std::vector<Drawn> draws(const std::vector<Method> &methods)
{
    std::vector<Drawn> out;
    Drawn next;
    uint32_t offset = 0;
    for (size_t i = 0; i < methods.size(); i++) {
        const auto [m, v] = methods[i];
        if (m == kLoadOffset) {
            offset = v;
        } else if (m == kLoad) {
            next.words[offset] = v;
            offset += 4;
        } else if (m == NVB197_SET_RT_LAYER) {
            next.layer = v;
        } else if (m == NVB197_PIXEL_SHADER_BARRIER) {
            next.barrier = i + 1 < methods.size() &&
                           methods[i + 1].first == SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED;
        } else if (m == NVB197_SET_VERTEX_ARRAY_START || m == NVB197_SET_INDEX_BUFFER_F) {
            next.firstMethod = m;
            next.first = v;
            next.count = methods[i + 1].second;
            i++;
        } else if (m == NVB197_END) {
            out.push_back(next);
            next = {};
        }
    }
    return out;
}

TEST(EmitAmplify, EachAmplificationIsDrawnInOrderAfterItsId)
{
    Scratch scratch;
    scratch.count = 3;
    const auto drawn = draws(run(switch_gm20b_macro_amplify, scratch, {6, 1, 10, 0}));
    ASSERT_EQ(drawn.size(), 3u);
    for (uint32_t a = 0; a < 3; a++) {
        EXPECT_EQ(drawn[a].words.at(kIdWord), a) << a;
        EXPECT_EQ(drawn[a].firstMethod, uint32_t(NVB197_SET_VERTEX_ARRAY_START));
        EXPECT_EQ(drawn[a].first, 10u);
        EXPECT_EQ(drawn[a].count, 6u);
        EXPECT_FALSE(drawn[a].barrier);
        EXPECT_EQ(drawn[a].layer, -1);
    }
}

TEST(EmitAmplify, EveryInstanceOfOneAmplificationBeforeTheNext)
{
    Scratch scratch;
    scratch.count = 2;
    const auto drawn = draws(run(switch_gm20b_macro_amplify, scratch, {3, 2, 0, 0}));
    ASSERT_EQ(drawn.size(), 4u);
    EXPECT_EQ(drawn[0].words.at(kIdWord), 0u);
    EXPECT_TRUE(drawn[1].words.empty());
    EXPECT_EQ(drawn[2].words.at(kIdWord), 1u);
    EXPECT_TRUE(drawn[3].words.empty());
}

TEST(EmitAmplify, TheDrawParametersAndTheCountWordOnce)
{
    Scratch scratch;
    scratch.count = 2;
    scratch.countWord = kCountWord;
    const auto methods = run(switch_gm20b_macro_amplify_indexed, scratch, {9, 1, 4, 0xfffffff0u, 7});
    const auto drawn = draws(methods);
    ASSERT_EQ(drawn.size(), 2u);
    EXPECT_EQ(drawn[0].words.at(0), 0xfffffff0u);
    EXPECT_EQ(drawn[0].words.at(4), 7u);
    EXPECT_EQ(drawn[0].words.at(kCountWord), 2u);
    EXPECT_EQ(drawn[1].words.count(kCountWord), 0u);
    EXPECT_EQ(drawn[1].words.count(0), 0u);
    for (const Drawn &d : drawn) {
        EXPECT_EQ(d.firstMethod, uint32_t(NVB197_SET_INDEX_BUFFER_F));
        EXPECT_EQ(d.first, 4u);
        EXPECT_EQ(d.count, 9u);
    }
    size_t baseVertex = 0;
    for (const auto &[m, v] : methods)
        baseVertex += m == NVB197_SET_GLOBAL_BASE_VERTEX_INDEX && v == 0xfffffff0u;
    EXPECT_EQ(baseVertex, 1u);
}

TEST(EmitAmplify, EachAmplificationsViewportAndLayerWords)
{
    Scratch scratch;
    scratch.count = 2;
    scratch.viewport = kViewportWord;
    scratch.layer = kLayerWord;
    scratch.viewports[0] = 4;
    scratch.viewports[1] = 2;
    scratch.layers[0] = 1;
    scratch.layers[1] = 3;
    const auto drawn = draws(run(switch_gm20b_macro_amplify, scratch, {3, 1, 0, 0}));
    ASSERT_EQ(drawn.size(), 2u);
    EXPECT_EQ(drawn[0].words.at(kViewportWord), 4u);
    EXPECT_EQ(drawn[1].words.at(kViewportWord), 2u);
    EXPECT_EQ(drawn[0].words.at(kLayerWord), 1u);
    EXPECT_EQ(drawn[1].words.at(kLayerWord), 3u);
    EXPECT_EQ(drawn[0].layer, -1);
}

TEST(EmitAmplify, TheRenderTargetLayerBySetRtLayerAndBackToTheFirst)
{
    Scratch scratch;
    scratch.count = 2;
    scratch.layer = SWITCH_GM20B_AMP_LAYER_RT;
    scratch.layers[0] = 2;
    scratch.layers[1] = 5;
    const auto methods = run(switch_gm20b_macro_amplify, scratch, {3, 1, 0, 0});
    const auto drawn = draws(methods);
    ASSERT_EQ(drawn.size(), 2u);
    EXPECT_EQ(drawn[0].layer, 2);
    EXPECT_EQ(drawn[1].layer, 5);
    ASSERT_FALSE(methods.empty());
    EXPECT_EQ(methods.back(), Method(NVB197_SET_RT_LAYER, 0));
}

TEST(EmitAmplify, APixelBarrierBetweenAmplificationsWhereTheStateFetches)
{
    Scratch scratch;
    scratch.count = 3;
    scratch.barrier = 1;
    const auto drawn = draws(run(switch_gm20b_macro_amplify, scratch, {3, 1, 0, 0}));
    ASSERT_EQ(drawn.size(), 3u);
    EXPECT_FALSE(drawn[0].barrier);
    EXPECT_TRUE(drawn[1].barrier);
    EXPECT_TRUE(drawn[2].barrier);
}

TEST(EmitAmplify, NothingIsDrawnOfNoVerticesNoInstancesOrNoAmplifications)
{
    Scratch scratch;
    scratch.count = 2;
    EXPECT_TRUE(draws(run(switch_gm20b_macro_amplify, scratch, {0, 1, 0, 0})).empty());
    EXPECT_TRUE(draws(run(switch_gm20b_macro_amplify, scratch, {3, 0, 0, 0})).empty());
    scratch.count = 0;
    EXPECT_TRUE(draws(run(switch_gm20b_macro_amplify, scratch, {3, 1, 0, 0})).empty());
}

}  // namespace
