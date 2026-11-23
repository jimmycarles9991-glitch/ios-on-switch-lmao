// SPDX-License-Identifier: MIT
// The bind macro of an indirect command buffer's dispatches (gen/SwitchGM20BMacros.h icb_bind) on Mesa's Fermi MME
// simulator, over the bind slots the driver writes (drivers/TegraGM20BMetal/icb_compute.h binds):
// each call's data is SWITCH_GM20B_ICB_BIND_WORDS words, at most a method header's count, the slot holds exactly its
// calls before its no-ops, and the macro reads every word of a call and none past them. On the GPU a call one word
// short leaves the MME waiting for it.
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/icb_compute.h"
#include "method_checks.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

// The device's zero page, whose address >> 8 the channel's context keeps in scratch (mme.h install)
constexpr uint64_t kZeroPage = 0xc3'0046'0000ull;

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    switch_gm20b_test::Scratch scratch;
    uint32_t launches = 0;
    // the 3D class's inline-to-memory writes, by address
    std::map<uint64_t, uint32_t> memory;
    uint64_t out = 0;

    static uint32_t load(void *opaque)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        EXPECT_LT(s.consumed, s.arguments.size()) << "a word past the call's";
        return s.consumed < s.arguments.size() ? s.arguments[s.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        const uint32_t word = (method - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4;
        EXPECT_TRUE(word == uint32_t(SWITCH_GM20B_SCRATCH_ZERO_PAGE) ||
                    (word >= uint32_t(SWITCH_GM20B_SCRATCH_ICB_BUFFERS) &&
                     word < uint32_t(SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END)))
            << word;
        return static_cast<Simulation *>(opaque)->scratch.method(method);
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        switch_gm20b_test::expectMaxwellB(method);
        auto &s = *static_cast<Simulation *>(opaque);
        s.launches += method == NVB197_LAUNCH_DMA;
        if (method == NVB197_OFFSET_OUT_UPPER)
            s.out = (s.out & 0xffffffffull) | uint64_t(value) << 32;
        else if (method == NVB197_OFFSET_OUT)
            s.out = (s.out & ~0xffffffffull) | value;
        else if (method == NVB197_LOAD_INLINE_DATA) {
            s.memory[s.out] = value;
            s.out += 4;
        }
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

    void run(const uint32_t *words, uint32_t count)
    {
        arguments.assign(words, words + count);
        consumed = 0;
        const gm20b::mme::Program &p = gm20b::mme::kIcbBindProgram;
        std::vector<mme_fermi_inst> decoded(p.size);
        mme_fermi_decode(decoded.data(), p.words, p.size);
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(p.size, decoded.data(), &ops, this);
        EXPECT_EQ(consumed, count);
    }

    // Each call of a bind slot of `words`, as the channel reads it; the calls
    uint32_t slot(const uint32_t *slot, uint32_t words)
    {
        uint32_t calls = 0, i = 0;
        while (i < words && slot[i] != gm20b::icb::kNop) {
            const uint32_t header = slot[i++], count = header >> 16 & 0x1fff;
            EXPECT_EQ(header >> 29, 5u) << "ONE_INC";
            EXPECT_EQ((header & 0xfff) << 2, gm20b::mme::kMacroCall + 8 * SWITCH_GM20B_MACRO_ICB_BIND);
            EXPECT_LE(i + count, words);
            run(slot + i, count);
            i += count;
            calls++;
        }
        for (; i < words; i++)
            EXPECT_EQ(slot[i], gm20b::icb::kNop) << i;
        return calls;
    }
};

// An execute's inherited buffers in scratch, as computeBindHead writes them: every buffer's base 4 bytes past 16, its
// address-high word with a bind stride above it, its size; and the zero page's word
Simulation simulation()
{
    Simulation s;
    s.scratch[SWITCH_GM20B_SCRATCH_ZERO_PAGE] = uint32_t(kZeroPage >> 8);
    for (uint32_t k = 0; k < SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS; k++) {
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k] = 0x10000 + 0x1000 * k + 4;
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k + 1] = (16u << SWITCH_GM20B_ICB_STRIDE_SHIFT) | 1;
        s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * k + 2] = 0x400;
    }
    return s;
}

TEST(EmitIcbBind, TheMacroReadsExactlyTheWordsARawBindSlotCalls)
{
    const std::tuple<uint32_t, uint32_t, uint32_t> shapes[] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {2, 3, 0}, {5, 2, 4}, {20, 0, 0}, {1200, 3, 40}, {0, 0, 2800},
    };
    for (const auto &[descriptors, strides, roots] : shapes) {
        std::vector<gm20b::icb::ComputeBind> binds;
        uint32_t residuals = 0;
        for (uint32_t i = 0; i < descriptors; i++) {
            const bool residual = i % 2 != 0;
            binds.push_back({0x100000000ull + 0x100 * i, residual ? 0x100002000ull + 4 * i : 0, i % 31,
                             i % 3 ? 0u : 0x80u, residual ? i : 0});
            residuals += residual;
        }
        std::vector<gm20b::icb::ComputeStride> strideWords;
        for (uint32_t i = 0; i < strides; i++)
            strideWords.push_back({0x100003000ull + 4 * i, (i + 3) % 31});
        std::vector<gm20b::icb::ComputeRoot> entries;
        for (uint32_t i = 0; i < roots; i++)
            entries.push_back({0x100004000ull + 32 * i, (i + 7) % 31});
        const uint64_t words = gm20b::icb::computeBindSlotWords(descriptors, strides, roots);
        std::vector<uint32_t> slot(words);
        ASSERT_TRUE(gm20b::icb::binds(slot.data(), binds.data(), descriptors, strideWords.data(), strides,
                                      entries.data(), roots, uint32_t(words)));
        Simulation s = simulation();
        const uint32_t calls = s.slot(slot.data(), uint32_t(words));
        const uint64_t data = SWITCH_GM20B_ICB_BIND_DESCRIPTOR_WORDS * uint64_t(descriptors) +
                              SWITCH_GM20B_ICB_BIND_STRIDE_WORDS * uint64_t(strides) +
                              SWITCH_GM20B_ICB_BIND_ROOT_WORDS * uint64_t(roots);
        EXPECT_EQ(calls, data > gm20b::icb::kComputeBindCallWords - 3 ? 2u : 1u)
            << descriptors << " descriptors, " << strides << " strides, " << roots << " entries";
        // an inline-to-memory write of each descriptor, each residual word, each stride word and each entry
        EXPECT_EQ(s.launches, descriptors + residuals + strides + roots);
    }
}

// The residual word of a base on 16 bytes takes the word the record leaves there; one off 16 bytes its residual
TEST(EmitIcbBind, AResidualWordTakesTheRecordsWordForABaseOn16Bytes)
{
    const gm20b::icb::ComputeBind binds[] = {{0x100000000ull, 0x100002000ull, 0, 0, 0xabc},
                                             {0x100000100ull, 0x100002004ull, 1, 0, 0xdef}};
    std::vector<uint32_t> slot(gm20b::icb::kComputeBindSlotWords);
    ASSERT_TRUE(gm20b::icb::binds(slot.data(), binds, 2));
    Simulation s = simulation();
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS] = 0x10000;
    s.slot(slot.data(), uint32_t(slot.size()));
    EXPECT_EQ(s.memory.at(0x100002000ull), 0xabcu);
    EXPECT_EQ(s.memory.at(0x100002004ull), 4u);
}

// A root table entry is writeDrawRoots' {va, va, size rounded up to 16, 0}, a buffer's bind stride left out of its
// address and a size within 15 of 2^32 carried into the size's high word; no buffer (address 0, size 0) is the zero
// page's {zero page, 0, 0, 0}
TEST(EmitIcbBind, ARootTableEntryIsTheCpusEntryOfTheBuffer)
{
    const gm20b::icb::ComputeRoot roots[] = {{0x100004040ull, 2}, {0x100004060ull, 3}, {0x100004080ull, 4}};
    std::vector<uint32_t> slot(gm20b::icb::kComputeBindSlotWords);
    ASSERT_TRUE(gm20b::icb::binds(slot.data(), nullptr, 0, nullptr, 0, roots, 3));
    Simulation s = simulation();
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 2 + 2] = 0x401;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 3 + 2] = 0xfffffff9;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 4] = 0;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 4 + 1] = 16u << SWITCH_GM20B_ICB_STRIDE_SHIFT;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 4 + 2] = 0;
    s.slot(slot.data(), uint32_t(slot.size()));
    const uint32_t two[8] = {0x12004, 1, 0x12004, 1, 0x410, 0, 0, 0}, three[8] = {0x13004, 1, 0x13004, 1, 0, 1, 0, 0},
                   none[8] = {uint32_t(kZeroPage), uint32_t(kZeroPage >> 32), 0, 0, 0, 0, 0, 0};
    for (uint32_t w = 0; w < 8; w++) {
        EXPECT_EQ(s.memory.at(0x100004040ull + 4 * w), two[w]) << w;
        EXPECT_EQ(s.memory.at(0x100004060ull + 4 * w), three[w]) << w;
        EXPECT_EQ(s.memory.at(0x100004080ull + 4 * w), none[w]) << w;
    }
}

// The descriptor of no buffer (address 0, size 0; a bind stride above it) is the CPU's icbRegion's: the zero page's
// address and range 0, whatever the kernel's window, and its residual word takes the record's word
TEST(EmitIcbBind, ADescriptorOfNoBufferReadsTheZeroPage)
{
    const gm20b::icb::ComputeBind bind = {0x100000000ull, 0x100002000ull, 5, 0x80, 0xabc};
    std::vector<uint32_t> slot(gm20b::icb::kComputeBindSlotWords);
    ASSERT_TRUE(gm20b::icb::binds(slot.data(), &bind, 1));
    Simulation s = simulation();
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 5] = 0;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 5 + 1] = 16u << SWITCH_GM20B_ICB_STRIDE_SHIFT;
    s.scratch[SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 3 * 5 + 2] = 0;
    s.slot(slot.data(), uint32_t(slot.size()));
    EXPECT_EQ(s.memory.at(0x100000000ull), uint32_t(kZeroPage));
    EXPECT_EQ(s.memory.at(0x100000004ull), uint32_t(kZeroPage >> 32));
    EXPECT_EQ(s.memory.at(0x100000008ull), 0u);
    EXPECT_EQ(s.memory.at(0x100002000ull), 0xabcu);
}

TEST(EmitIcbBind, ASlotWithNothingToBindCallsNothing)
{
    uint32_t slot[gm20b::icb::kComputeBindSlotWords];
    ASSERT_TRUE(gm20b::icb::binds(slot, nullptr, 0));
    for (const uint32_t word : slot)
        EXPECT_EQ(word, gm20b::icb::kNop);
}

}  // namespace
