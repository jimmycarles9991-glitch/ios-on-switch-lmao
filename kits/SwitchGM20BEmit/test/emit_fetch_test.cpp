// SPDX-License-Identifier: MIT
// The fetch macros (gen/SwitchGM20BMacros.h) on Mesa's Fermi MME simulator over the scratch words
// SwitchGM20BMacroSlots.h gives them: each instance one split draw, a BEGIN a primitive over that primitive's own
// vertices with the fetch barrier before it, the last BEGIN of none, every argument consumed whatever the primitive
// count.
#include <cstdint>
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

constexpr uint32_t kSplit = 29, kInstance = 26, kPrimitive = 24;
constexpr uint32_t kFirst = NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_OPEN_END << kSplit,
                   kNext = kFirst | NVB197_BEGIN_INSTANCE_ID_SUBSEQUENT << kInstance,
                   kContinued = NVB197_BEGIN_SPLIT_MODE_OPEN_BEGIN_OPEN_END << kSplit |
                                NVB197_BEGIN_INSTANCE_ID_UNCHANGED << kInstance |
                                NVB197_BEGIN_PRIMITIVE_ID_UNCHANGED << kPrimitive,
                   kLast = NVB197_BEGIN_SPLIT_MODE_OPEN_BEGIN_NORMAL_END << kSplit |
                           NVB197_BEGIN_INSTANCE_ID_UNCHANGED << kInstance |
                           NVB197_BEGIN_PRIMITIVE_ID_UNCHANGED << kPrimitive,
                   kWhole = NVB197_BEGIN_SPLIT_MODE_NORMAL_BEGIN_NORMAL_END << kSplit;

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    switch_gm20b_test::Scratch scratch;
    uint32_t topology = 0;
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
        if (method == NVB197_SET_PRIMITIVE_TOPOLOGY)
            return s.topology;
        const uint32_t word = (method - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4;
        const bool icb = word == SWITCH_GM20B_SCRATCH_ICB_LOCATION || word == SWITCH_GM20B_SCRATCH_ICB_LENGTH;
        EXPECT_TRUE(icb || (word >= SWITCH_GM20B_SCRATCH_FETCH_FIRST && word < SWITCH_GM20B_SCRATCH_FETCH_END)) << word;
        return s.scratch.method(method);
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        switch_gm20b_test::expectMaxwellB(method);
        const uint32_t load = NVB197_LOAD_CONSTANT_BUFFER(0);
        static_cast<Simulation *>(opaque)->methods.emplace_back(method >= load && method < load + 64 ? load : method,
                                                                value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "a fetch macro needs no MME barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "a fetch macro needs no MME memory";
        return nullptr;
    }
};

// MTLPrimitiveType's run sizes as mme.h fetchShape writes them
constexpr uint32_t kRunFirst[5] = {1, 2, 2, 3, 3}, kRunStep[5] = {1, 2, 1, 3, 1};

// An indirect command buffer's: commands [0, 4) in range, `ordered` its execute's FETCH_ICB, `topology` the command's
// SET_PRIMITIVE_TOPOLOGY, its scratch sizes left 0 as an execute leaves them
struct Execute {
    bool ordered;
    uint32_t topology;
};

template <size_t N>
std::vector<Method> run(const uint32_t (&words)[N], uint32_t primitive, const std::vector<uint32_t> &arguments,
                        const Execute *execute = nullptr)
{
    Simulation s;
    s.arguments = arguments;
    if (execute) {
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_LOCATION] = 0;
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_LENGTH] = 4;
        s.scratch[SWITCH_GM20B_SCRATCH_FETCH_ICB] = execute->ordered;
        s.topology = execute->topology;
    }
    s.scratch[SWITCH_GM20B_SCRATCH_FETCH_FIRST] = execute ? 0 : kRunFirst[primitive];
    s.scratch[SWITCH_GM20B_SCRATCH_FETCH_STEP] = execute ? 0 : kRunStep[primitive];
    s.scratch[SWITCH_GM20B_SCRATCH_FETCH_BEGIN] = kFirst;
    s.scratch[SWITCH_GM20B_SCRATCH_FETCH_NEXT] = kNext;
    s.scratch[SWITCH_GM20B_SCRATCH_FETCH_CONTINUED] = kContinued;
    std::vector<mme_fermi_inst> decoded(N);
    mme_fermi_decode(decoded.data(), words, uint32_t(N));
    const mme_sim_state_ops ops = {Simulation::load, Simulation::state, Simulation::emit, Simulation::barrier,
                                   Simulation::dram};
    mme_fermi_sim_core(uint32_t(N), decoded.data(), &ops, &s);
    EXPECT_EQ(s.consumed, arguments.size());
    return s.methods;
}

// A BEGIN .. END: its BEGIN word, its first vertex or index and count, and whether the fetch barrier came before it
struct Begin {
    uint32_t word = 0, first = 0, count = 0, firstMethod = 0;
    bool barrier = false;

    bool operator==(const Begin &o) const
    {
        return word == o.word && first == o.first && count == o.count && barrier == o.barrier;
    }
};

std::ostream &operator<<(std::ostream &out, const Begin &b)
{
    return out << std::hex << "{0x" << b.word << std::dec << ", " << b.first << ", " << b.count << ", "
               << (b.barrier ? "barrier" : "-") << "}";
}

std::vector<Begin> begins(const std::vector<Method> &methods)
{
    std::vector<Begin> out;
    Begin next;
    for (size_t i = 0; i < methods.size(); i++) {
        const auto [m, v] = methods[i];
        if (m == NVB197_PIXEL_SHADER_BARRIER) {
            next.barrier = i + 1 < methods.size() &&
                           methods[i + 1].first == SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED;
        } else if (m == NVB197_BEGIN) {
            next.word = v;
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

constexpr uint32_t kTriangles = 3, kStrip = 4, kLineStrip = 2, kLines = 1, kPoints = 0;

TEST(EmitFetch, EachTriangleOfEachInstanceIsABeginBehindTheBarrier)
{
    const auto drawn = begins(run(switch_gm20b_macro_fetch_draw, kTriangles, {9, 2, 10, 0}));
    const std::vector<Begin> expected = {
        {kFirst, 10, 3, 0, true}, {kContinued, 13, 3, 0, true}, {kContinued, 16, 3, 0, true}, {kLast, 19, 0, 0, false},
        {kNext, 10, 3, 0, true},  {kContinued, 13, 3, 0, true}, {kContinued, 16, 3, 0, true}, {kLast, 19, 0, 0, false},
    };
    EXPECT_EQ(drawn, expected);
}

TEST(EmitFetch, AStripsBeginsEachRepeatTheVerticesTheirTriangleShares)
{
    const auto drawn = begins(run(switch_gm20b_macro_fetch_draw, kStrip, {6, 1, 0, 0}));
    const std::vector<Begin> expected = {{kFirst, 0, 3, 0, true},
                                         {kContinued, 1, 3, 0, true},
                                         {kContinued, 2, 3, 0, true},
                                         {kContinued, 3, 3, 0, true},
                                         {kLast, 4, 0, 0, false}};
    EXPECT_EQ(drawn, expected);
}

TEST(EmitFetch, WhatCompletesNoPrimitiveIsNotDrawn)
{
    EXPECT_EQ(begins(run(switch_gm20b_macro_fetch_draw, kTriangles, {7, 1, 0, 0})),
              (std::vector<Begin>{{kFirst, 0, 3, 0, true}, {kContinued, 3, 3, 0, true}, {kLast, 6, 0, 0, false}}));
    EXPECT_EQ(begins(run(switch_gm20b_macro_fetch_draw, kTriangles, {2, 1, 0, 0})),
              (std::vector<Begin>{{kWhole, 0, 0, 0, false}}));
}

TEST(EmitFetch, PointsAndLineStripsByTheirRuns)
{
    EXPECT_EQ(begins(run(switch_gm20b_macro_fetch_draw, kPoints, {2, 1, 4, 0})),
              (std::vector<Begin>{{kFirst, 4, 1, 0, true}, {kContinued, 5, 1, 0, true}, {kLast, 6, 0, 0, false}}));
    EXPECT_EQ(begins(run(switch_gm20b_macro_fetch_draw, kLineStrip, {3, 1, 0, 0})),
              (std::vector<Begin>{{kFirst, 0, 2, 0, true}, {kContinued, 1, 2, 0, true}, {kLast, 2, 0, 0, false}}));
}

using Primitive = std::vector<uint32_t>;

// The primitives `count` vertices from `first` of MTLPrimitiveType `primitive` make, each as its vertices in order
std::vector<Primitive> primitivesOf(uint32_t primitive, uint32_t first, uint32_t count)
{
    std::vector<Primitive> out;
    for (uint32_t k = 0; k + kRunFirst[primitive] <= count; k += kRunStep[primitive]) {
        Primitive p;
        for (uint32_t v = 0; v < kRunFirst[primitive]; v++)
            p.push_back(first + k + v);
        out.push_back(p);
    }
    return out;
}

// What GM20B draws of a fetch macro's BEGINs: each BEGIN's primitives from its own vertices alone (on the device a
// strip's BEGINs of one new vertex each drew nothing, gpu.gm20b.fetch_icb), every one behind the fetch barrier and
// alone in its BEGIN, so that the barrier orders each after the one before
std::vector<Primitive> assembled(const std::vector<Begin> &drawn, uint32_t primitive)
{
    std::vector<Primitive> made;
    for (const Begin &b : drawn) {
        const auto own = primitivesOf(primitive, b.first, b.count);
        EXPECT_LE(own.size(), 1u) << "a BEGIN of " << b.count << " vertices from " << b.first;
        EXPECT_TRUE(own.empty() || b.barrier) << "a BEGIN from " << b.first << " drawn without the barrier";
        made.insert(made.end(), own.begin(), own.end());
    }
    return made;
}

std::vector<Primitive> instancesOf(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances)
{
    std::vector<Primitive> out;
    for (uint32_t i = 0; i < instances; i++) {
        const auto one = primitivesOf(primitive, first, count);
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

TEST(EmitFetch, EveryPrimitiveOfTheDrawIsOneBeginsOwnVertices)
{
    for (uint32_t primitive : {kPoints, kLines, kLineStrip, kTriangles, kStrip})
        for (uint32_t count : {1u, 2u, 3u, 4u, 5u, 7u, 9u})
            for (uint32_t instances : {1u, 3u}) {
                const auto want = instancesOf(primitive, 4, count, instances);
                const Execute execute = {true, primitive + 1};
                EXPECT_EQ(assembled(begins(run(switch_gm20b_macro_fetch_draw, primitive, {count, instances, 4, 0})),
                                    primitive),
                          want)
                    << "MTLPrimitiveType " << primitive << ", " << count << " vertices, " << instances << " instances";
                EXPECT_EQ(assembled(begins(run(switch_gm20b_macro_fetch_draw_indexed, primitive,
                                               {count, instances, 4, 0, 0})),
                                    primitive),
                          want)
                    << "indexed MTLPrimitiveType " << primitive << ", " << count << " indices";
                EXPECT_EQ(assembled(begins(run(switch_gm20b_macro_icb_draw, 0, {0, count, instances, 4, 0}, &execute)),
                                    primitive),
                          want)
                    << "a command of SET_PRIMITIVE_TOPOLOGY " << primitive + 1 << ", " << count << " vertices";
            }
}

TEST(EmitFetch, AnIndexedDrawsParametersOnceThenItsIndices)
{
    const auto methods = run(switch_gm20b_macro_fetch_draw_indexed, kStrip, {4, 1, 8, 0xfffffff0u, 7});
    const uint32_t load = NVB197_LOAD_CONSTANT_BUFFER(0);
    const std::vector<Method> head(methods.begin(), methods.begin() + 6);
    EXPECT_EQ(head, (std::vector<Method>{{NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                                         {load, 0xfffffff0u},
                                         {load, 7},
                                         {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 7},
                                         {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, 0xfffffff0u},
                                         {NVB197_SET_VERTEX_ID_BASE, 0xfffffff0u}}));
    const auto drawn = begins(methods);
    ASSERT_EQ(drawn.size(), 3u);
    for (const Begin &b : drawn)
        EXPECT_EQ(b.firstMethod, uint32_t(NVB197_SET_INDEX_BUFFER_F));
    EXPECT_EQ(drawn,
              (std::vector<Begin>{{kFirst, 8, 3, 0, true}, {kContinued, 9, 3, 0, true}, {kLast, 10, 0, 0, false}}));
}

TEST(EmitFetch, NothingIsDrawnOfNoVerticesOrNoInstances)
{
    EXPECT_TRUE(begins(run(switch_gm20b_macro_fetch_draw, kTriangles, {0, 1, 0, 0})).empty());
    EXPECT_TRUE(begins(run(switch_gm20b_macro_fetch_draw, kTriangles, {3, 0, 0, 0})).empty());
}

// An indirect command buffer's execute of a fetching state: each command a primitive at a time, its sizes from the
// topology it set (SET_PRIMITIVE_TOPOLOGY: triangle strip 5, triangle list 4, line strip 3); another execute's commands
// whole, as the plain indirect body draws them
TEST(EmitFetch, AnOrderedExecutesCommandsTakeTheirSizesFromTheirTopology)
{
    const Execute strip = {true, 5}, list = {true, 4}, lines = {true, 3}, whole = {false, 5};
    EXPECT_EQ(begins(run(switch_gm20b_macro_icb_draw, 0, {0, 5, 1, 0, 0}, &strip)),
              (std::vector<Begin>{{kFirst, 0, 3, 0, true},
                                  {kContinued, 1, 3, 0, true},
                                  {kContinued, 2, 3, 0, true},
                                  {kLast, 3, 0, 0, false}}));
    EXPECT_EQ(begins(run(switch_gm20b_macro_icb_draw, 0, {1, 6, 1, 0, 0}, &list)),
              (std::vector<Begin>{{kFirst, 0, 3, 0, true}, {kContinued, 3, 3, 0, true}, {kLast, 6, 0, 0, false}}));
    EXPECT_EQ(begins(run(switch_gm20b_macro_icb_draw_indexed, 0, {2, 3, 1, 0, 0, 0}, &lines)),
              (std::vector<Begin>{{kFirst, 0, 2, 0, true}, {kContinued, 1, 2, 0, true}, {kLast, 2, 0, 0, false}}));
    EXPECT_EQ(begins(run(switch_gm20b_macro_icb_draw, 0, {0, 5, 1, 0, 0}, &whole)),
              (std::vector<Begin>{{0, 0, 5, 0, false}}));
    EXPECT_TRUE(begins(run(switch_gm20b_macro_icb_draw, 0, {7, 5, 1, 0, 0}, &strip)).empty()) << "outside the range";
}

}  // namespace
