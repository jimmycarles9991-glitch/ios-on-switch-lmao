// SPDX-License-Identifier: MIT
// The bind macro of an indirect command buffer's dispatches (gen/SwitchGM20BMacros.h icb_bind) on Mesa's Fermi MME
// simulator, over the bind slots the raw path writes (drivers/TegraGM20BMetal/raw/runtime/raw_icb_compute.h binds): the
// call's data is SWITCH_GM20B_ICB_BIND_WORDS words, the slot holds exactly those before its no-ops, and the macro reads
// every one and none past them. On the GPU a call one word short leaves the MME waiting for it.
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_icb_compute.h"
#include "method_checks.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    switch_gm20b_test::Scratch scratch;
    uint32_t launches = 0;

    static uint32_t load(void *opaque)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        EXPECT_LT(s.consumed, s.arguments.size()) << "a word past the call's";
        return s.consumed < s.arguments.size() ? s.arguments[s.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        const uint32_t word = (method - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4;
        EXPECT_GE(word, uint32_t(SWITCH_GM20B_SCRATCH_ICB_BUFFERS));
        EXPECT_LT(word, uint32_t(SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END));
        return static_cast<Simulation *>(opaque)->scratch.method(method);
    }

    static void emit(void *opaque, uint16_t method, uint32_t)
    {
        switch_gm20b_test::expectMaxwellB(method);
        static_cast<Simulation *>(opaque)->launches += method == NVB197_LAUNCH_DMA;
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "the bind macro needs no MME barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "the bind macro needs no MME memory";
        return nullptr;
    }
};

// An execute's inherited buffers in scratch, as computeBindHead writes them: every buffer's base 4 bytes past 16, its
// address-high word with a bind stride above it, its size
Simulation simulation(std::vector<uint32_t> arguments)
{
    Simulation s;
    s.arguments = std::move(arguments);
    for (uint32_t k = 0; k < SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS; k++) {
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k] = 0x10000 + 0x1000 * k + 4;
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k + 1] = (16u << SWITCH_GM20B_ICB_STRIDE_SHIFT) | 1;
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k + 2] = 0x400;
    }
    return s;
}

void run(Simulation &s)
{
    const raw::mme::Program &p = raw::mme::kIcbBindProgram;
    std::vector<mme_fermi_inst> decoded(p.size);
    mme_fermi_decode(decoded.data(), p.words, p.size);
    const mme_sim_state_ops ops = {Simulation::load, Simulation::state, Simulation::emit, Simulation::barrier,
                                   Simulation::dram};
    mme_fermi_sim_core(p.size, decoded.data(), &ops, &s);
}

TEST(EmitIcbBind, TheMacroReadsExactlyTheWordsARawBindSlotCalls)
{
    const std::pair<uint32_t, uint32_t> shapes[] = {
        {1, 0}, {0, 1}, {1, 1}, {2, 3}, {5, 2}, {raw::icb::kComputeBindsMax, 0},
    };
    for (const auto &[descriptors, strides] : shapes) {
        std::vector<raw::icb::ComputeBind> binds;
        uint32_t residuals = 0;
        for (uint32_t i = 0; i < descriptors; i++) {
            const bool residual = i % 2 != 0;
            binds.push_back(
                {0x100000000ull + 0x100 * i, residual ? 0x100002000ull + 4 * i : 0, i % 31, i ? 0u : 0x80u});
            residuals += residual;
        }
        std::vector<raw::icb::ComputeStride> strideWords;
        for (uint32_t i = 0; i < strides; i++)
            strideWords.push_back({0x100003000ull + 4 * i, (i + 3) % 31});
        uint32_t slot[raw::icb::kComputeBindSlotWords];
        ASSERT_TRUE(raw::icb::binds(slot, binds.data(), descriptors, strideWords.data(), strides));

        const uint32_t header = slot[0], count = header >> 16 & 0x1fff;
        EXPECT_EQ(header >> 29, 5u) << "ONE_INC";
        EXPECT_EQ((header & 0xfff) << 2, raw::mme::kMacroCall + 8 * SWITCH_GM20B_MACRO_ICB_BIND);
        EXPECT_EQ(count, uint32_t(SWITCH_GM20B_ICB_BIND_WORDS(descriptors, strides)));
        ASSERT_LT(count, raw::icb::kComputeBindSlotWords);
        for (uint32_t i = 1 + count; i < raw::icb::kComputeBindSlotWords; i++)
            EXPECT_EQ(slot[i], raw::icb::kNop) << i;

        Simulation s = simulation(std::vector<uint32_t>(slot + 1, slot + 1 + count));
        run(s);
        EXPECT_EQ(s.consumed, count) << descriptors << " descriptors, " << strides << " strides";
        // an inline-to-memory write of each descriptor, each residual word and each stride word
        EXPECT_EQ(s.launches, descriptors + residuals + strides);
    }
}

TEST(EmitIcbBind, ASlotWithNothingToBindCallsNothing)
{
    uint32_t slot[raw::icb::kComputeBindSlotWords];
    ASSERT_TRUE(raw::icb::binds(slot, nullptr, 0));
    for (const uint32_t word : slot)
        EXPECT_EQ(word, raw::icb::kNop);
}

}  // namespace
