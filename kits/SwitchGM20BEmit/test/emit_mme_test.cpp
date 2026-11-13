// SPDX-License-Identifier: MIT
// The macros Mesa's builder makes (gen/SwitchGM20BMacros.h) against the ones raw_mme.h hand-encoded before it installed
// the builder's (their words kept here), both run by Mesa's Fermi MME simulator (the in-tree copy) on the same
// arguments: the same methods with the same values, every argument consumed, no shadow state read. The one difference
// allowed: the builder sends the root table's two words to LOAD_CONSTANT_BUFFER(0) and (1), raw both to (0); the
// hardware takes either as the next word of the load.
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../SwitchGM20B/SwitchGM20B3D.h"
#include "../SwitchGM20BMacroSlots.h"
#include "SwitchGM20BMacros.h"
#include "method_checks.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

using Method = std::pair<uint32_t, uint32_t>;

constexpr uint32_t kHandDraw[] = {0x00000201, 0x00000301, 0x00000411, 0x00030827, 0x01618021, 0x00002041,
                                  0x04d74021, 0x00001041, 0x00001841, 0x01614021, 0x00000041, 0x00004411,
                                  0xd0410413, 0xffffc911, 0xfffd8837, 0x00000091, 0x00000011};
constexpr uint32_t kHandDrawIndexed[] = {0x00000201, 0x00000301, 0x00000411, 0x00030827, 0x01618021, 0x00002041,
                                         0x057dc021, 0x00001041, 0x00001841, 0x01614021, 0x00000041, 0x00004411,
                                         0xd0410413, 0xffffc911, 0xfffd8837, 0x00000091, 0x00000011};
constexpr uint32_t kHandIndirect[] = {0x00000201, 0x00000301, 0x00000401, 0x0238c021, 0x00000041, 0x02390021,
                                      0x00001841, 0x00002041, 0x01438021, 0x00002041, 0x01434021, 0x00000041,
                                      0x01118021, 0x00000041, 0x00000611, 0x00034827, 0x00031027, 0x01618021,
                                      0x00003041, 0x04d74021, 0x00001841, 0x00000841, 0x01614021, 0x00000041,
                                      0x00004611, 0xd0418613, 0xffffd211, 0xfffd9037, 0x00000091, 0x00000011};
constexpr uint32_t kHandIndexedIndirect[] = {
    0x00000201, 0x00000301, 0x00000401, 0x00000501, 0x0238c021, 0x00000041, 0x02390021, 0x00002041,
    0x00002841, 0x01438021, 0x00002841, 0x01434021, 0x00002041, 0x01118021, 0x00002041, 0x00000611,
    0x00034827, 0x00031027, 0x01618021, 0x00003041, 0x057dc021, 0x00001841, 0x00000841, 0x01614021,
    0x00000041, 0x00004611, 0xd0418613, 0xffffd211, 0xfffd9037, 0x00000091, 0x00000011};

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
        ADD_FAILURE() << "the raw path's macros read no shadow state";
        return 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        switch_gm20b_test::expectMaxwellB(method);
        const uint32_t load = 0x2390;  // LOAD_CONSTANT_BUFFER(0): (0..15) all load the next word
        static_cast<Simulation *>(opaque)->methods.emplace_back(method >= load && method < load + 64 ? load : method,
                                                                value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "a draw macro needs no barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "a draw macro needs no MME memory";
        return nullptr;
    }
};

std::vector<Method> run(const uint32_t *words, size_t size, const std::vector<uint32_t> &arguments)
{
    Simulation s;
    s.arguments = arguments;
    std::vector<mme_fermi_inst> decoded(size);
    mme_fermi_decode(decoded.data(), words, uint32_t(size));
    const mme_sim_state_ops ops = {Simulation::load, Simulation::state, Simulation::emit, Simulation::barrier,
                                   Simulation::dram};
    mme_fermi_sim_core(uint32_t(size), decoded.data(), &ops, &s);
    EXPECT_EQ(s.consumed, arguments.size());
    return s.methods;
}

template <size_t N> size_t count(const uint32_t (&)[N])
{
    return N;
}

TEST(EmitMme, DirectDrawsAsRawMacrosDrawThem)
{
    for (uint32_t instances : {0u, 1u, 2u, 5u}) {
        const std::vector<uint32_t> args = {instances, 7, 300};
        EXPECT_EQ(run(switch_gm20b_macro_draw, count(switch_gm20b_macro_draw), args),
                  run(kHandDraw, count(kHandDraw), args))
            << instances;
        EXPECT_EQ(run(switch_gm20b_macro_draw_indexed, count(switch_gm20b_macro_draw_indexed), args),
                  run(kHandDrawIndexed, count(kHandDrawIndexed), args))
            << instances;
    }
}

TEST(EmitMme, IndirectDrawsAsRawMacrosDrawThem)
{
    for (uint32_t vertices : {0u, 6u}) {
        for (uint32_t instances : {0u, 1u, 3u}) {
            const std::vector<uint32_t> draw = {vertices, instances, 11, 4};
            EXPECT_EQ(run(switch_gm20b_macro_indirect, count(switch_gm20b_macro_indirect), draw),
                      run(kHandIndirect, count(kHandIndirect), draw))
                << vertices << " " << instances;
            const std::vector<uint32_t> indexed = {vertices, instances, 11, 0xfffffff0u, 4};
            EXPECT_EQ(run(switch_gm20b_macro_indexed_indirect, count(switch_gm20b_macro_indexed_indirect), indexed),
                      run(kHandIndexedIndirect, count(kHandIndexedIndirect), indexed))
                << vertices << " " << instances;
        }
    }
}

// The colour matrix uniform: rows M0..M4 of halves (r, g, b, a), then the clamp pair (p3, p4)
std::vector<uint32_t> vcmUniform(uint16_t p3, uint16_t p4)
{
    std::vector<uint32_t> words;
    for (uint32_t row = 0; row < 5; row++) {
        const uint32_t base = 0x3c00 + 0x100 * row;
        words.push_back(base | (base + 1) << 16);
        words.push_back((base + 2) | (base + 3) << 16);
    }
    words.push_back(p3 | uint32_t(p4) << 16);
    return words;
}

uint32_t constantMethod(uint32_t j, uint32_t lane)
{
    return 0x1480 + 16 * j + 4 * lane;  // SET_ITERATED_BLEND_CONSTANT_RED/GREEN/BLUE(j)
}

std::vector<uint32_t> loadedProgram(const std::vector<Method> &methods, uint32_t &passes, uint32_t &enable)
{
    std::vector<uint32_t> words;
    for (const auto &[method, value] : methods) {
        if (method == 0x0df4)  // LOAD_ITERATED_BLEND_INSTRUCTION
            words.push_back(value);
        if (method == 0x0dd4)  // SET_ITERATED_BLEND_PASS
            passes = value;
        if (method == 0x0dd0)  // SET_ITERATED_BLEND
            enable = value;
    }
    return words;
}

TEST(EmitMme, TheColourMatrixBecomesTheIteratedBlendsConstants)
{
    const std::vector<Method> methods =
        run(switch_gm20b_macro_vcm_constants, count(switch_gm20b_macro_vcm_constants), vcmUniform(0x3a66, 0));
    std::map<uint32_t, uint32_t> constants;
    for (const auto &[method, value] : methods)
        if (method >= 0x1480 && method < 0x1500)
            constants[method] = value;
    for (uint32_t row = 0; row < 5; row++)
        for (uint32_t lane = 0; lane < 3; lane++)
            EXPECT_EQ(constants[constantMethod(row, lane)], 0x3c00 + 0x100 * row + lane) << row << " " << lane;
    EXPECT_EQ(constants[constantMethod(5, 0)], 0x3c03u);
    EXPECT_EQ(constants[constantMethod(5, 1)], 0x3d03u);
    EXPECT_EQ(constants[constantMethod(5, 2)], 0x3e03u);
    EXPECT_EQ(constants[constantMethod(6, 0)], 0x3f03u);
    EXPECT_EQ(constants[constantMethod(6, 1)], 0x4003u);
    EXPECT_EQ(constants[constantMethod(6, 2)], 0x068eu);
    EXPECT_EQ(constants[constantMethod(7, 0)], 0x3a66u);
    EXPECT_EQ(constants[constantMethod(7, 1)], 0xb800u);
    EXPECT_EQ(constants[constantMethod(7, 2)], 0u);
}

TEST(EmitMme, TheClampPairPicksTheColourMatrixProgram)
{
    // programs_long.py vcm, vcm.clip and vcm.scale: 26, 34 and 38 passes
    const std::pair<std::pair<uint16_t, uint16_t>, uint32_t> cases[] = {
        {{0x0000, 0x3c00}, 26}, {{0xba66, 0x3c00}, 26}, {{0x7e00, 0x0000}, 26},
        {{0x3a66, 0x0000}, 34}, {{0x3a66, 0x8000}, 34}, {{0x3a66, 0x3c00}, 38},
    };
    for (const auto &[clamp, want] : cases) {
        uint32_t passes = 0, enable = 0;
        const std::vector<uint32_t> program =
            loadedProgram(run(switch_gm20b_macro_vcm_constants, count(switch_gm20b_macro_vcm_constants),
                              vcmUniform(clamp.first, clamp.second)),
                          passes, enable);
        EXPECT_EQ(program.size(), want) << clamp.first << " " << clamp.second;
        EXPECT_EQ(passes, want);
        EXPECT_EQ(enable, 3u);
        EXPECT_EQ(program.front(), 0x3200d799u);
        EXPECT_EQ(program.back(), 0x00009029u);
    }
}

// The draw parameters as the raw path wrote them from the CPU before each draw (raw_gpu.cpp prepare), then the draw loop
std::vector<Method> withParams(bool indexed, uint32_t instances, uint32_t first, uint32_t count, uint32_t baseVertex,
                               uint32_t baseInstance)
{
    std::vector<Method> m = {{NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                             {NVB197_LOAD_CONSTANT_BUFFER(0), indexed ? baseVertex : first},
                             {NVB197_LOAD_CONSTANT_BUFFER(0), baseInstance},
                             {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, baseInstance},
                             {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, indexed ? baseVertex : 0},
                             {NVB197_SET_VERTEX_ID_BASE, indexed ? baseVertex : 0}};
    const uint16_t start = indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START;
    for (uint32_t i = 0; i < instances; ++i) {
        m.push_back({NVB197_BEGIN, i ? 1u << 26 : 0});
        m.push_back({start, first});
        m.push_back({uint16_t(start + 4), count});
        m.push_back({NVB197_END, 0});
    }
    return m;
}

TEST(EmitMme, ParameterDrawsWriteTheParametersThenDraw)
{
    for (uint32_t instances : {0u, 1u, 3u}) {
        EXPECT_EQ(run(switch_gm20b_macro_draw_params, count(switch_gm20b_macro_draw_params), {instances, 40, 6, 9}),
                  withParams(false, instances, 40, 6, 0, 9))
            << instances;
        EXPECT_EQ(run(switch_gm20b_macro_draw_indexed_params, count(switch_gm20b_macro_draw_indexed_params),
                      {instances, 12, 30, 0xfffffffcu, 2}),
                  withParams(true, instances, 12, 30, 0xfffffffcu, 2))
            << instances;
    }
}

TEST(EmitMme, TheMacrosTakeNvksDrawSlots)
{
    EXPECT_EQ(SWITCH_GM20B_MACRO_DRAW, 11);
    EXPECT_EQ(SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS, 16);
    EXPECT_EQ(SWITCH_GM20B_MACRO_VCM_CONSTANTS, 17);
    EXPECT_EQ(SWITCH_GM20B_MACRO_ICB_RANGE, 18);
    EXPECT_EQ(SWITCH_GM20B_MACRO_ICB_GATE, 21);
    EXPECT_EQ(SWITCH_GM20B_MACRO_ICB_BIND, 22);
    EXPECT_EQ(SWITCH_GM20B_MACRO_ICB_PATCHES, 23);
    EXPECT_EQ(SWITCH_GM20B_MACRO_AMPLIFY, 24);
    EXPECT_EQ(SWITCH_GM20B_MACRO_AMPLIFY_INDEXED, 25);
    EXPECT_EQ(SWITCH_GM20B_MACRO_CLEAR_LAYERS, 26);
    EXPECT_EQ(SWITCH_GM20B_MACRO_RAW_END, 27);
}

TEST(EmitMme, TheScratchRangesAreTheTables)
{
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_LOCATION, 3);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_LENGTH, 4);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_SET_HI, 5);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_CONTROL, 10);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD, 11);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_AMP_COUNT, 16);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_AMP_MAPS, 17);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_AMP_END, 25);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ROOT_HI, 26);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_BUFFERS, 35);
    EXPECT_EQ(SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END, 128);
}

TEST(EmitMme, TheMacrosFitTheRawPathsCodeSpace)
{
    const size_t words = count(switch_gm20b_macro_draw) + count(switch_gm20b_macro_draw_indexed) +
                         count(switch_gm20b_macro_indirect) + count(switch_gm20b_macro_indexed_indirect) +
                         count(switch_gm20b_macro_draw_params) + count(switch_gm20b_macro_draw_indexed_params) +
                         count(switch_gm20b_macro_vcm_constants) + count(switch_gm20b_macro_icb_range) +
                         count(switch_gm20b_macro_icb_draw) + count(switch_gm20b_macro_icb_draw_indexed) +
                         count(switch_gm20b_macro_icb_gate) + count(switch_gm20b_macro_icb_bind) +
                         count(switch_gm20b_macro_icb_patches) + count(switch_gm20b_macro_amplify) +
                         count(switch_gm20b_macro_amplify_indexed) + count(switch_gm20b_macro_clear_layers);
    RecordProperty("words", int(words));
    EXPECT_LE(words, uint32_t(SWITCH_GM20B_MACRO_CODE_WORDS));
}

}  // namespace
