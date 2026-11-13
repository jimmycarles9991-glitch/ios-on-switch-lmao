// SPDX-License-Identifier: MIT
// The gate on GM20B's method ranges: SwitchGM20BMacroSlots.h's limits are the Fermi MME's, its scratch ranges overlap
// neither each other nor the words NVK's keep, the channel's context init writes only methods its classes take, and
// every raw macro names only MAXWELL_B's methods in its code and writes and reads only those when Mesa's Fermi MME
// simulator runs it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../../SwitchGM20B/SwitchGM20B3D.h"
#include "../SwitchGM20BMacroSlots.h"
#include "SwitchGM20BContext.h"
#include "SwitchGM20BMacros.h"
#include "kits/SwitchGM20B/SwitchGM20BCaps.h"
#include "method_checks.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

using switch_gm20b_test::expectMaxwellB;
using switch_gm20b_test::isScratch;
using switch_gm20b_test::Scratch;

struct Macro {
    const char *name;
    const uint32_t *words;
    uint32_t size;
};

template <size_t N> Macro macro(const char *name, const uint32_t (&words)[N])
{
    return {name, words, uint32_t(N)};
}

const Macro kMacros[] = {
    macro("draw", switch_gm20b_macro_draw),
    macro("draw_indexed", switch_gm20b_macro_draw_indexed),
    macro("indirect", switch_gm20b_macro_indirect),
    macro("indexed_indirect", switch_gm20b_macro_indexed_indirect),
    macro("draw_params", switch_gm20b_macro_draw_params),
    macro("draw_indexed_params", switch_gm20b_macro_draw_indexed_params),
    macro("vcm_constants", switch_gm20b_macro_vcm_constants),
    macro("icb_range", switch_gm20b_macro_icb_range),
    macro("icb_draw", switch_gm20b_macro_icb_draw),
    macro("icb_draw_indexed", switch_gm20b_macro_icb_draw_indexed),
    macro("icb_gate", switch_gm20b_macro_icb_gate),
    macro("icb_bind", switch_gm20b_macro_icb_bind),
    macro("icb_patches", switch_gm20b_macro_icb_patches),
    macro("amplify", switch_gm20b_macro_amplify),
    macro("amplify_indexed", switch_gm20b_macro_amplify_indexed),
    macro("clear_layers", switch_gm20b_macro_clear_layers),
};

std::vector<mme_fermi_inst> decode(const Macro &m)
{
    std::vector<mme_fermi_inst> insts(m.size);
    mme_fermi_decode(insts.data(), m.words, m.size);
    return insts;
}

// A macro on the simulator: every argument it loads is `argument`, its scratch the channel's
struct MacroRun {
    uint32_t argument = 0;
    Scratch scratch;
    unsigned methods = 0;

    static uint32_t load(void *opaque)
    {
        return static_cast<MacroRun *>(opaque)->argument;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        auto &r = *static_cast<MacroRun *>(opaque);
        EXPECT_TRUE(isScratch(method)) << "a raw macro reads no state but its scratch, not 0x" << std::hex << method;
        return isScratch(method) ? r.scratch.method(method) : 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        auto &r = *static_cast<MacroRun *>(opaque);
        r.methods++;
        if (isScratch(method))
            r.scratch.method(method) = value;
        else
            expectMaxwellB(method);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "the Fermi MME has no data RAM";
        return nullptr;
    }
};

// The scratch words as the raw path writes them: an execute's range and layout, its inherited buffers (each slot's
// address, stride and size), the root table's address, a bound state's amplification words, the count and the mappings
Scratch channelScratch(uint32_t amplifications, uint32_t layer, uint32_t barrier)
{
    Scratch s;
    s[SWITCH_GM20B_SCRATCH_ROOT_HI] = 0x1;
    s[SWITCH_GM20B_SCRATCH_ROOT_HI + 1] = 0x00200000;
    s[SWITCH_GM20B_SCRATCH_ICB_LOCATION] = 1;
    s[SWITCH_GM20B_SCRATCH_ICB_LENGTH] = 2;
    s[SWITCH_GM20B_SCRATCH_ICB_SET_HI] = 0x1;
    s[SWITCH_GM20B_SCRATCH_ICB_SET_HI + 1] = 0x00300000;
    for (uint32_t k = 0; k < SWITCH_GM20B_ICB_PATCH_BUFFERS; k++)
        s[SWITCH_GM20B_SCRATCH_ICB_ENTRY + k] = k == 1 ? SWITCH_GM20B_ICB_NO_ENTRY : 0x40 * k;
    s[SWITCH_GM20B_SCRATCH_ICB_CONTROL] = 0x720;
    for (uint32_t slot = 0; slot < SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS; slot++) {
        s[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * slot] = 0x10000 * slot + 4;
        s[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * slot + 1] = 16u << SWITCH_GM20B_ICB_STRIDE_SHIFT | 1;
        s[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * slot + 2] = 0x400;
    }
    s[SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD] = 0x704;
    s[SWITCH_GM20B_SCRATCH_AMP_ID_WORD] = 0x708;
    s[SWITCH_GM20B_SCRATCH_AMP_VIEWPORT_WORD] = 0x70c;
    s[SWITCH_GM20B_SCRATCH_AMP_LAYER] = layer;
    s[SWITCH_GM20B_SCRATCH_AMP_BARRIER] = barrier;
    s[SWITCH_GM20B_SCRATCH_AMP_COUNT] = amplifications;
    for (uint32_t a = 0; a < SWITCH_GM20B_AMP_MAX; a++)
        s[SWITCH_GM20B_SCRATCH_AMP_MAPS + a] = a | (a % 16) << SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT;
    return s;
}

TEST(MethodRanges, TheTableIsTheFermiMmes)
{
    static_assert(SWITCH_GM20B_SCRATCH_WORDS == MME_FERMI_SCRATCH_COUNT, "Mesa's Fermi MME's scratch words");
    EXPECT_EQ(uint32_t(SWITCH_GM20B_SCRATCH_WORDS), SWITCH_GM20B_LIMIT_MME_SHADOW_SCRATCH);
    EXPECT_EQ(uint32_t(SWITCH_GM20B_MACRO_SLOTS), SWITCH_GM20B_LIMIT_MME_MACROS);
    EXPECT_EQ(uint32_t(SWITCH_GM20B_MACRO_CODE_WORDS), SWITCH_GM20B_LIMIT_MME_CODE_WORDS);
    EXPECT_TRUE(switch_gm20b_method_legal(0xb197, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_WORDS - 1)));
    EXPECT_FALSE(switch_gm20b_method_legal(0xb197, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_WORDS)));
    EXPECT_TRUE(switch_gm20b_method_legal(0xb197, NVB197_CALL_MME_DATA(SWITCH_GM20B_MACRO_SLOTS - 1)));
    EXPECT_FALSE(switch_gm20b_method_legal(0xb197, NVB197_CALL_MME_MACRO(SWITCH_GM20B_MACRO_SLOTS)));
}

TEST(MethodRanges, EveryScratchWordHasOneOwner)
{
    struct Range {
        const char *owner;
        uint32_t first, count;
    };

    const Range ranges[] = {
        {"FECS's mailbox", 0, SWITCH_GM20B_SCRATCH_FECS_END},
        {"the root table's address", SWITCH_GM20B_SCRATCH_ROOT_HI, 2},
        {"an execute's range", SWITCH_GM20B_SCRATCH_ICB_LOCATION, 2},
        {"a patch execute's set", SWITCH_GM20B_SCRATCH_ICB_SET_HI, 2},
        {"a patch execute's entries", SWITCH_GM20B_SCRATCH_ICB_ENTRY, SWITCH_GM20B_ICB_PATCH_BUFFERS},
        {"a patch execute's control word", SWITCH_GM20B_SCRATCH_ICB_CONTROL, 1},
        {"an amplifying state's words", SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD, 5},
        {"the amplification count", SWITCH_GM20B_SCRATCH_AMP_COUNT, 1},
        {"the amplification mappings", SWITCH_GM20B_SCRATCH_AMP_MAPS, SWITCH_GM20B_AMP_MAX},
        {"an execute's inherited buffers", SWITCH_GM20B_SCRATCH_ICB_BUFFERS, 3 * SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS},
    };
    std::string owner[SWITCH_GM20B_SCRATCH_WORDS];
    for (const Range &r : ranges)
        for (uint32_t w = r.first; w < r.first + r.count; w++) {
            ASSERT_LT(w, uint32_t(SWITCH_GM20B_SCRATCH_WORDS)) << r.owner;
            EXPECT_TRUE(owner[w].empty()) << "word " << w << ": " << owner[w] << " and " << r.owner;
            owner[w] = r.owner;
        }
}

TEST(MethodRanges, TheContextInitWritesOnlyItsClassesMethods)
{
    for (const switch_gm20b_context_method &m : switch_gm20b_context)
        switch_gm20b_test::expectMethod(switch_gm20b_test::subchannelClass(m.subc, m.mthd), m.mthd);
}

TEST(MethodRanges, TheContextInitLeavesTheInheritedBuffersZero)
{
    for (const switch_gm20b_context_method &m : switch_gm20b_context)
        if (m.subc == 0 && m.mthd >= NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_BUFFERS) &&
            m.mthd < NVB197_CALL_MME_MACRO(0))
            EXPECT_TRUE(m.data == 0 && m.reloc == SWITCH_GM20B_CONTEXT_FIXED) << std::hex << m.mthd;
}

TEST(MethodRanges, EveryMacroNamesOnlyMaxwellBsMethods)
{
    for (const Macro &m : kMacros) {
        SCOPED_TRACE(m.name);
        EXPECT_LE(m.size, uint32_t(SWITCH_GM20B_MACRO_CODE_WORDS));
        for (const mme_fermi_inst &i : decode(m)) {
            if (i.op == MME_FERMI_OP_BRANCH)
                continue;
            if (i.op == MME_FERMI_OP_STATE)
                expectMaxwellB(uint32_t(i.imm) * 4);
            const bool setsMethod = i.assign_op == MME_FERMI_ASSIGN_OP_MOVE_SET_MADDR ||
                                    i.assign_op == MME_FERMI_ASSIGN_OP_LOAD_SET_MADDR ||
                                    i.assign_op == MME_FERMI_ASSIGN_OP_MOVE_SET_MADDR_LOAD_EMIT ||
                                    i.assign_op == MME_FERMI_ASSIGN_OP_MOVE_SET_MADDR_LOAD_EMIT_HIGH;
            if (!setsMethod)
                continue;
            EXPECT_EQ(i.op, MME_FERMI_OP_ADD_IMM) << "a method computed from more than a register and a constant";
            if (i.op == MME_FERMI_OP_ADD_IMM)
                expectMaxwellB((uint32_t(i.imm) & 0xfff) << 2);
        }
    }
}

TEST(MethodRanges, EveryMacroRunsOnMaxwellBsMethods)
{
    const uint32_t layers[] = {SWITCH_GM20B_AMP_LAYER_FIRST, SWITCH_GM20B_AMP_LAYER_RT, 0x710};
    for (const Macro &m : kMacros) {
        const std::vector<mme_fermi_inst> insts = decode(m);
        unsigned methods = 0;
        for (uint32_t argument : {0u, 1u, 2u, 3u})
            for (uint32_t amplifications : {1u, 3u, uint32_t(SWITCH_GM20B_AMP_MAX)})
                for (uint32_t layer : layers)
                    for (uint32_t barrier : {0u, 1u}) {
                        SCOPED_TRACE(std::string(m.name) + " argument " + std::to_string(argument));
                        MacroRun r;
                        r.argument = argument;
                        r.scratch = channelScratch(amplifications, layer, barrier);
                        const mme_sim_state_ops ops = {MacroRun::load, MacroRun::state, MacroRun::emit,
                                                       MacroRun::barrier, MacroRun::dram};
                        mme_fermi_sim_core(uint32_t(insts.size()), insts.data(), &ops, &r);
                        methods += r.methods;
                    }
        EXPECT_GT(methods, 0u) << m.name << " ran no method";
    }
}

}  // namespace
