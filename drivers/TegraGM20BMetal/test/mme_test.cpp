// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../mme.h"
#include "../pack/mesa/mme_fermi.h"
#include "../pack/mesa/mme_fermi_sim.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include <map>
#include <utility>
#include <vector>

namespace {
using Method = std::pair<uint16_t, uint32_t>;

constexpr uint16_t kBegin = NVB197_BEGIN, kEnd = NVB197_END, kVertexFirst = NVB197_SET_VERTEX_ARRAY_START,
                   kIndexFirst = NVB197_SET_INDEX_BUFFER_F;

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    std::vector<Method> methods;

    static uint32_t load(void *opaque)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        EXPECT_LT(s.consumed, s.arguments.size());
        return s.consumed < s.arguments.size() ? s.arguments[s.consumed++] : 0;
    }

    static uint32_t state(void *, uint16_t)
    {
        ADD_FAILURE() << "native draw macro must not read mutable shadow scratch or root state";
        return 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        switch_gm20b_test::expectMaxwellB(method);
        static_cast<Simulation *>(opaque)->methods.emplace_back(method, value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "an instancing macro requires no barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "an instancing macro requires no internal DRAM";
        return nullptr;
    }

    void runEncoded(const uint32_t *encoded, size_t size, std::vector<uint32_t> params)
    {
        arguments = std::move(params);
        consumed = 0;
        std::vector<mme_fermi_inst> decoded(size);
        mme_fermi_decode(decoded.data(), encoded, uint32_t(size));
        std::vector<uint32_t> roundTrip(size);
        mme_fermi_encode(roundTrip.data(), uint32_t(decoded.size()), decoded.data());
        EXPECT_EQ(roundTrip, (std::vector<uint32_t>(encoded, encoded + size)));
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
        EXPECT_EQ(consumed, arguments.size());
    }

    void run(bool indexed, uint32_t first, uint32_t count, uint32_t instances)
    {
        const auto &encoded = indexed ? gm20b::mme::kIndexedProgram : gm20b::mme::kDrawProgram;
        runEncoded(encoded.words, encoded.size, {instances, first, count});
    }
};

std::vector<Method> expected(bool indexed, uint32_t first, uint32_t count, uint32_t instances)
{
    std::vector<Method> methods;
    const uint16_t start = indexed ? kIndexFirst : kVertexFirst;
    for (uint32_t i = 0; i < instances; ++i) {
        methods.emplace_back(kBegin, i ? 1u << 26 : 0);
        methods.emplace_back(start, first);
        methods.emplace_back(start + 4, count);
        methods.emplace_back(kEnd, 0);
    }
    return methods;
}

TEST(GM20BMme, ActualEncodedProgramsMatchDirectDrawSemantics)
{
    for (bool indexed : {false, true})
        for (uint32_t instances : {0u, 1u, 2u, 7u, 1000u}) {
            SCOPED_TRACE(::testing::Message() << indexed << ":" << instances);
            Simulation simulation;
            simulation.run(indexed, 0x81234567, 0x12345, instances);
            EXPECT_EQ(simulation.methods, expected(indexed, 0x81234567, 0x12345, instances));
        }
}

TEST(GM20BMme, MixedDirectAndMacroDrawsResetInstanceIdWithoutTouchingRootState)
{
    Simulation simulation;
    simulation.run(false, 8, 6, 3);
    const auto direct = expected(true, 0, 12, 1);
    simulation.methods.insert(simulation.methods.end(), direct.begin(), direct.end());
    simulation.run(true, 4, 9, 2);
    auto reference = expected(false, 8, 6, 3);
    reference.insert(reference.end(), direct.begin(), direct.end());
    const auto indexed = expected(true, 4, 9, 2);
    reference.insert(reference.end(), indexed.begin(), indexed.end());
    EXPECT_EQ(simulation.methods, reference);
}

TEST(GM20BMme, CallsHaveConstantCpuSizeAndUseTheCorrectArgumentMethod)
{
    for (bool indexed : {false, true})
        for (uint32_t instances : {2u, 1000u, UINT32_MAX}) {
            std::array<uint32_t, gm20b::mme::kCallWords> words{};
            gm20b::Push push{words.data(), words.data() + words.size()};
            gm20b::mme::draw(push, indexed, 12, 65537, instances);
            EXPECT_EQ(push.at, push.end);
            EXPECT_EQ(words[0], gm20b::Push::hdr(5, gm20b::SUBC_3D, 0x3800 + 8 * (indexed ? 12 : 11), 3));
            EXPECT_EQ(words[1], instances);
            EXPECT_EQ(words[2], 12u);
            EXPECT_EQ(words[3], 65537u);
        }
}

TEST(GM20BMme, IndirectArgumentsSetRootAndHardwareBaseParametersBeforeDrawing)
{
    for (bool indexed : {false, true})
        for (uint32_t count : {0u, 19u})
            for (uint32_t instances : {0u, 1u, 5u}) {
                Simulation simulation;
                const auto &code = indexed ? gm20b::mme::kIndexedIndirectProgram : gm20b::mme::kIndirectProgram;
                const uint32_t first = 7, baseVertex = uint32_t(-13), baseInstance = 0xf1234567;
                const std::vector<uint32_t> args =
                    indexed ? std::vector<uint32_t>{count, instances, first, baseVertex, baseInstance}
                            : std::vector<uint32_t>{count, instances, first, baseInstance};
                simulation.runEncoded(code.words, code.size, args);
                // LOAD_CONSTANT_BUFFER(1) takes the load's next word as (0) does
                std::vector<Method> reference = {{0x238c, 0},
                                                 {0x2390, indexed ? baseVertex : first},
                                                 {0x2394, baseInstance},
                                                 {0x1438, baseInstance},
                                                 {0x1434, indexed ? baseVertex : 0},
                                                 {0x1118, indexed ? baseVertex : 0}};
                auto draws = expected(indexed, first, count, count ? instances : 0);
                reference.insert(reference.end(), draws.begin(), draws.end());
                EXPECT_EQ(simulation.methods, reference);
            }
}

TEST(GM20BMme, IndirectHeaderDeclaresExactlyTheMetalArgumentWords)
{
    for (bool indexed : {false, true}) {
        uint32_t word;
        gm20b::Push push{&word, &word + 1};
        gm20b::mme::indirectHeader(push, indexed);
        EXPECT_EQ(push.at, push.end);
        EXPECT_EQ(word, gm20b::Push::hdr(5, gm20b::SUBC_3D, 0x3800 + 8 * (indexed ? 14 : 13), indexed ? 5 : 4));
    }
}

TEST(GM20BMme, LayeredClearSurfacesEveryLayerFromTheBase)
{
    constexpr uint32_t kLayer = 1u << 10;  // CLEAR_SURFACE RT_ARRAY_INDEX's lowest bit
    for (const uint32_t layers : {1u, 2u, 6u, 2048u}) {
        const uint32_t payload = 0x3c | 2u << 6 | 3 | 5 * kLayer;
        Simulation simulation;
        simulation.runEncoded(gm20b::mme::kClearLayersProgram.words, gm20b::mme::kClearLayersProgram.size,
                              {payload, layers});
        std::vector<Method> want;
        for (uint32_t i = 0; i < layers; i++)
            want.emplace_back(uint16_t(NVB197_CLEAR_SURFACE), payload + i * kLayer);
        EXPECT_EQ(simulation.methods, want) << layers;
    }
}

TEST(GM20BMme, ContextReservationCoversEveryProgram)
{
    std::array<uint32_t, gm20b::mme::kContextWords> words{};
    gm20b::Push push{words.data(), words.data() + words.size()};
    gm20b::mme::install(push, 0x1234500000ull, 0x2345670000ull);
    EXPECT_EQ(push.at, push.end);
    EXPECT_EQ(words[1], gm20b::mme::kDraw);
    EXPECT_EQ(words[2], 0u);
    const size_t second = 5 + gm20b::mme::kDrawProgram.size;
    EXPECT_EQ(words[second + 1], gm20b::mme::kDrawIndexed);
    EXPECT_EQ(words[second + 2], gm20b::mme::kDrawProgram.size);
    EXPECT_EQ(words[words.size() - 3],
              gm20b::Push::hdr(1, gm20b::SUBC_3D, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ROOT_HI), 2));
    EXPECT_EQ(words[words.size() - 2], 0x12u);
    EXPECT_EQ(words[words.size() - 1], 0x34500000u);
}

// What the channel's context and the encoders write of the macros' scratch, at the largest each writes: only methods
// MAXWELL_B takes, so every word below the Fermi MME's 128
TEST(GM20BMme, TheContextAndTheScratchWritersWriteOnlyMaxwellBsMethods)
{
    std::vector<uint32_t> words(gm20b::mme::kContextWords + gm20b::mme::kAmplificationScratchWords + 32);
    gm20b::Push push{words.data(), words.data() + words.size()};
    gm20b::mme::install(push, 0x1234500000ull, 0x2345670000ull);
    uint32_t layers[SWITCH_GM20B_AMP_MAX], viewports[SWITCH_GM20B_AMP_MAX];
    for (uint32_t a = 0; a < SWITCH_GM20B_AMP_MAX; a++) {
        layers[a] = 2047 - a;
        viewports[a] = 15 - a;
    }
    gm20b::mme::amplification(push, SWITCH_GM20B_AMP_MAX, layers, viewports);
    gm20b::mme::amplificationWords(push, {0x770, 0x77c, 0x778, 0x774, 1});
    gm20b::mme::icbRange(push, 7, 9);
    gm20b::mme::icbPatchLayout(push, 0x1100000000ull, 0x200, {0x40, gm20b::mme::kNoEntry, 0x80}, 0x77c);
    ASSERT_LE(push.at, push.end);
    EXPECT_TRUE(switch_gm20b_test::expectPushMethods(words.data(), size_t(push.at - words.data())));
}

TEST(GM20BMme, AnAmplificationsMappingWordHoldsItsLayerAndViewportOffsets)
{
    uint32_t words[2 + SWITCH_GM20B_AMP_MAX];
    gm20b::Push push{words, words + 2 + SWITCH_GM20B_AMP_MAX};
    const uint32_t layers[SWITCH_GM20B_AMP_MAX] = {0, 1, 2, 3, 4, 5, 6, 2047};
    const uint32_t viewports[SWITCH_GM20B_AMP_MAX] = {15, 14, 13, 12, 11, 10, 9, 0};
    gm20b::mme::amplification(push, 8, layers, viewports);
    EXPECT_EQ(push.at, push.end);
    EXPECT_EQ(words[0],
              gm20b::Push::hdr(1, gm20b::SUBC_3D, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_COUNT),
                               1 + SWITCH_GM20B_AMP_MAX));
    EXPECT_EQ(words[1], 8u);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_AMP_MAPS, SWITCH_GM20B_SCRATCH_AMP_COUNT + 1);
    for (uint32_t a = 0; a < SWITCH_GM20B_AMP_MAX; a++) {
        EXPECT_EQ(words[2 + a] & 0xffff, layers[a]) << a;
        EXPECT_EQ(words[2 + a] >> SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT, viewports[a]) << a;
    }
}

// The words the encoder wrote per draw before the macros took every draw: the draw parameters where they changed
// (nvk_mme_build_set_draw_params's methods), then BEGIN, the first and count, END; a macro call for instances
struct DirectDraws {
    uint32_t root = 0, baseInstance = 0, baseVertex = 0;

    void draw(gm20b::Push &p, bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root_,
              uint32_t baseInstance_)
    {
        if (root_ != root || baseInstance_ != baseInstance) {
            const uint32_t v[2] = {root_, baseInstance_};
            p.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0);
            p.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), v, 2);
            p.m(NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, baseInstance_);
            root = root_;
            baseInstance = baseInstance_;
        }
        const uint32_t baseVertex_ = indexed ? root_ : 0;
        if (baseVertex_ != baseVertex) {
            p.m(NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, baseVertex_);
            p.m(NVB197_SET_VERTEX_ID_BASE, baseVertex_);
            baseVertex = baseVertex_;
        }
        if (instances == 1) {
            p.m(NVB197_BEGIN, 0);
            p.inc(indexed ? kIndexFirst : kVertexFirst, {first, count});
            p.m(NVB197_END, 0);
        } else {
            gm20b::mme::draw(p, indexed, first, count, instances);
        }
    }
};

struct DrawCase {
    bool indexed;
    uint32_t first, count, instances, root, baseInstance;
};

// Core Animation's shapes: quads from one stream at moving first vertices, text runs indexed at a base vertex, a few
// instanced draws, draws at the same first vertex
const std::vector<DrawCase> kFrame = {
    {false, 0, 4, 1, 0, 0},           {false, 4, 4, 1, 4, 0},   {false, 8, 4, 1, 8, 0},
    {false, 8, 4, 1, 8, 0},           {true, 0, 6, 1, 0, 0},    {true, 6, 6, 1, 0, 0},
    {true, 0, 6, 1, 64, 0},           {false, 0, 4, 3, 0, 0},   {false, 0, 4, 3, 0, 2},
    {false, 12, 4, 1, 12, 0},         {true, 0, 12, 1, 128, 0}, {false, 0, 4, 1, 0, 0},
    {false, 0x3000, 4, 1, 0x3000, 0},
};

// What both forms leave the GPU doing, through the MME simulator: the same methods with the same values
std::vector<Method> methodsOf(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    const gm20b::mme::Program programs[] = {gm20b::mme::kDrawProgram,       gm20b::mme::kIndexedProgram,
                                            gm20b::mme::kIndirectProgram,   gm20b::mme::kIndexedIndirectProgram,
                                            gm20b::mme::kDrawParamsProgram, gm20b::mme::kIndexedParamsProgram};
    for (size_t i = 0; i < words.size();) {
        const uint32_t hdr = words[i++], op = hdr >> 29, count = (hdr >> 16) & 0x1fff;
        const uint16_t method = uint16_t((hdr & 0xfff) << 2);
        if (op == 4) {
            out.emplace_back(method, count);
            continue;
        }
        if (method >= 0x3800 && method < 0x3c00) {
            Simulation simulation;
            const uint32_t slot = (method - 0x3800) / 8;
            const gm20b::mme::Program &code = programs[slot - gm20b::mme::kDraw];
            simulation.runEncoded(code.words, code.size, std::vector<uint32_t>(&words[i], &words[i] + count));
            for (auto [m, v] : simulation.methods)
                out.emplace_back(m >= 0x2390 && m < 0x23d0 ? uint16_t(0x2390) : m, v);
            i += count;
            continue;
        }
        for (uint32_t k = 0; k < count; ++k)
            out.emplace_back(op == 1 ? uint16_t(method + 4 * k) : method, words[i++]);
    }
    return out;
}

TEST(GM20BMme, EveryDrawAsOneCallLeavesTheMethodsTheDirectWordsDid)
{
    std::vector<uint32_t> before(1024), after(1024);
    gm20b::Push p{before.data(), before.data() + before.size()}, q{after.data(), after.data() + after.size()};
    DirectDraws direct;
    gm20b::mme::DrawParams held;
    for (const DrawCase &c : kFrame) {
        direct.draw(p, c.indexed, c.first, c.count, c.instances, c.root, c.baseInstance);
        gm20b::mme::draw(q, held, c.indexed, c.first, c.count, c.instances, c.root, c.baseInstance);
    }
    before.resize(size_t(p.at - before.data()));
    after.resize(size_t(q.at - after.data()));
    // the parameter macros write the base vertex registers with every parameter change, the direct words where they
    // changed: the same registers and root table words at every BEGIN, from the zeros a pass starts with
    auto last = [](const std::vector<Method> &methods) {
        std::map<uint32_t, uint32_t> state = {{NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 0},
                                              {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, 0},
                                              {NVB197_SET_VERTEX_ID_BASE, 0},
                                              {0x10000, 0},
                                              {0x10004, 0}};
        std::vector<std::map<uint32_t, uint32_t>> draws;
        uint32_t offset = 0;
        for (const Method &m : methods) {
            if (m.first == NVB197_LOAD_CONSTANT_BUFFER_OFFSET)
                offset = m.second;
            else if (m.first == 0x2390)
                state[0x10000 + std::exchange(offset, offset + 4)] = m.second;
            else
                state[m.first] = m.second;
            if (m.first == kBegin)
                draws.push_back(state);
        }
        return draws;
    };
    EXPECT_EQ(last(methodsOf(after)), last(methodsOf(before)));
    printf("words per draw over %zu draws: direct %.2f, macro calls %.2f\n", kFrame.size(),
           double(before.size()) / kFrame.size(), double(after.size()) / kFrame.size());
    EXPECT_LT(after.size(), before.size());
}

TEST(GM20BMme, AnIndirectDrawLeavesTheParametersUnknown)
{
    std::array<uint32_t, 16> words{};
    gm20b::Push push{words.data(), words.data() + words.size()};
    gm20b::mme::DrawParams held;
    held.unknown = true;
    gm20b::mme::draw(push, held, false, 0, 4, 1, 0, 0);
    EXPECT_EQ(words[0], gm20b::Push::hdr(5, gm20b::SUBC_3D, 0x3800 + 8 * gm20b::mme::kDrawParams, 4));
    EXPECT_FALSE(held.unknown);
    const uint32_t *before = push.at;
    gm20b::mme::draw(push, held, false, 0, 4, 1, 0, 0);
    EXPECT_EQ(push.at - before, ptrdiff_t(gm20b::mme::kCallWords));
}

TEST(GM20BMme, BandDrawCallSyncsParamsAndEmitsClassWfiBetweenBands)
{
    std::array<uint32_t, gm20b::mme::kBandDrawMaxWords + gm20b::mme::kBandDrawCallWords> words{};
    gm20b::Push push{words.data(), words.data() + words.size()};
    gm20b::mme::DrawParams held;
    held.unknown = true;
    gm20b::mme::bandDraw(push, held, 4, 2, 3, 2, 5, 0, 1280, 0, 720);
    EXPECT_FALSE(held.unknown);
    EXPECT_EQ(held.root, 2u);
    EXPECT_EQ(held.baseInstance, 5u);
    EXPECT_EQ(held.baseVertex, 0u);
    const uint32_t *second = push.at;
    gm20b::mme::bandDraw(push, held, 4, 2, 3, 2, 5, 0, 1280, 0, 720);
    EXPECT_EQ(push.at - second, ptrdiff_t(gm20b::mme::kBandDrawCallWords));
    EXPECT_EQ(second[0], gm20b::Push::hdr(5, gm20b::SUBC_3D, 0x3800 + 8 * gm20b::mme::kBandDraw, 8));
    EXPECT_EQ(second[1], 4u);
    EXPECT_EQ(second[6], 180u);
    EXPECT_EQ(second[7], 720u);
    EXPECT_EQ(second[8], 720u << 16);

    Simulation sim;
    sim.runEncoded(gm20b::mme::kBandDrawProgram.words, gm20b::mme::kBandDrawProgram.size,
                   std::vector<uint32_t>(second + 1, second + 9));
    unsigned draws = 0, wfis = 0;
    for (const Method &m : sim.methods) {
        if (m.first == NVB197_END)
            ++draws;
        if (m.first == NVB197_WAIT_FOR_IDLE)
            ++wfis;
    }
    EXPECT_EQ(draws, 4u);
    EXPECT_EQ(wfis, 3u);
    ASSERT_FALSE(sim.methods.empty());
    EXPECT_EQ(sim.methods.back(), Method(NVB197_SET_SCISSOR_VERTICAL(0), 720u << 16));
}
}  // namespace
