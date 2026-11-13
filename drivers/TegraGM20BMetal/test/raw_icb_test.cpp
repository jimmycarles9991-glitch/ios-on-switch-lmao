// SPDX-License-Identifier: MIT
// raw/runtime/raw_icb.h and raw_mme.h's indirect command buffer macros: command words, run through the host FIFO's method
// decoding and the Fermi MME simulator with the macros the channel loads, after the range an execute writes (the CPU's
// scratch words, or kIcbRange reading a range from memory): each command inside the range sets its topology, restart and
// index buffer, then draws as an indirect draw of its arguments does (root draw parameters, base registers, the instance
// loop); one outside it sets the same and draws nothing; every other word is a no-op; the call sits at kCallWord. A patch
// command loads its control-stage descriptors and push words where its execute's layout says, inside the range or not, and
// draws its patches inside it alone.
#include <gtest/gtest.h>
#include "../raw/runtime/raw_icb.h"
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include <utility>
#include <vector>

namespace {
using Method = std::pair<uint16_t, uint32_t>;

struct Fifo {
    std::vector<Method> methods;
    std::vector<uint32_t> arguments;
    size_t consumed = 0, nops = 0;
    switch_gm20b_test::Scratch scratch;

    static uint32_t load(void *opaque)
    {
        auto &f = *static_cast<Fifo *>(opaque);
        return f.consumed < f.arguments.size() ? f.arguments[f.consumed++] : 0;
    }

    static bool isScratch(uint16_t method)
    {
        return method >= NVB197_SET_MME_SHADOW_SCRATCH(0) && method < NVB197_CALL_MME_MACRO(0);
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        EXPECT_TRUE(isScratch(method)) << "a draw macro reads no state but its scratch";
        return isScratch(method) ? static_cast<Fifo *>(opaque)->scratch.method(method) : 0;
    }

    void method(uint16_t m, uint32_t value)
    {
        switch_gm20b_test::expectMethod(switch_gm20b_test::subchannelClass(raw::SUBC_3D, m), m);
        if (isScratch(m))
            scratch.method(m) = value;
        else
            methods.emplace_back(m, value);
    }

    static void emit(void *opaque, uint16_t m, uint32_t value)
    {
        static_cast<Fifo *>(opaque)->method(m, value);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void macro(uint32_t slot)
    {
        const raw::mme::Program &p = slot == raw::mme::kIcbRange          ? raw::mme::kIcbRangeProgram
                                     : slot == raw::mme::kIcbPatches      ? raw::mme::kIcbPatchesProgram
                                     : slot == raw::mme::kIcbDraw         ? raw::mme::kIcbDrawProgram
                                     : slot == raw::mme::kIcbDrawIndexed  ? raw::mme::kIcbDrawIndexedProgram
                                     : slot == raw::mme::kIndirect        ? raw::mme::kIndirectProgram
                                     : slot == raw::mme::kIndexedIndirect ? raw::mme::kIndexedIndirectProgram
                                     : slot == raw::mme::kDraw            ? raw::mme::kDrawProgram
                                     : slot == raw::mme::kDrawParams      ? raw::mme::kDrawParamsProgram
                                     : slot == raw::mme::kIndexedParams   ? raw::mme::kIndexedParamsProgram
                                                                          : raw::mme::kIndexedProgram;
        std::vector<mme_fermi_inst> decoded(p.size);
        mme_fermi_decode(decoded.data(), p.words, p.size);
        consumed = 0;
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
        EXPECT_EQ(consumed, arguments.size());
        arguments.clear();
    }

    // Method headers as the host FIFO reads them (raw_push.h): 1 INC, 3 NON_INC, 4 IMMD, 5 ONE_INC; a macro call's data
    // feeds the macro
    void run(const uint32_t *w, size_t n)
    {
        for (size_t i = 0; i < n;) {
            const uint32_t header = w[i++], op = header >> 29, count = header >> 16 & 0x1fff,
                           method = (header & 0xfff) << 2;
            EXPECT_EQ(header >> 13 & 7, uint32_t(raw::SUBC_3D));
            if (method >= raw::mme::kMacroCall) {
                ASSERT_EQ(op, 5u);
                ASSERT_LE(i + count, n);
                arguments.assign(w + i, w + i + count);
                i += count;
                macro((method - raw::mme::kMacroCall) / 8);
                continue;
            }
            if (op == 4) {
                if (method == NVB197_NO_OPERATION)
                    nops++;
                else
                    this->method(uint16_t(method), count);
                continue;
            }
            ASSERT_TRUE(op == 1 || op == 3);
            ASSERT_LE(i + count, n);
            for (uint32_t k = 0; k < count; k++)
                this->method(uint16_t(op == 1 ? method + 4 * k : method), w[i++]);
        }
    }
};

// What an execute of commands [location, location + length) sends before them: the range as scratch words
void cpuRange(Fifo &f, uint32_t location, uint32_t length)
{
    uint32_t words[3];
    raw::Push p{words, words + 3};
    raw::mme::icbRange(p, location, length);
    EXPECT_EQ(p.at, p.end);
    f.run(words, 3);
}

std::vector<Method> drawn(uint16_t first, uint32_t start, uint32_t count, uint32_t instances)
{
    std::vector<Method> m;
    for (uint32_t i = 0; i < instances && count; i++) {
        m.emplace_back(NVB197_BEGIN, i ? 1u << 26 : 0);
        m.emplace_back(first, start);
        m.emplace_back(first + 4, count);
        m.emplace_back(NVB197_END, 0);
    }
    return m;
}

TEST(RawIcb, ADrawSetsItsTopologyThenDrawsAsAnIndirectDraw)
{
    for (uint32_t primitive : {0u, 2u, 3u, 4u}) {
        uint32_t slot[raw::icb::kSlotWords];
        raw::icb::draw(slot, 5, {primitive, 6, 3, 10, 7});
        EXPECT_EQ(slot[raw::icb::kIndexWord], 5u);
        Fifo f;
        cpuRange(f, 5, 1);
        f.run(slot, raw::icb::kSlotWords);
        std::vector<Method> want = {{NVB197_SET_PRIMITIVE_TOPOLOGY, raw::icb::topology(primitive)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART, primitive == 2 || primitive == 4},
                                    {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                                    {NVB197_LOAD_CONSTANT_BUFFER(0), 10},
                                    {NVB197_LOAD_CONSTANT_BUFFER(1), 7},
                                    {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 7},
                                    {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, 0},
                                    {NVB197_SET_VERTEX_ID_BASE, 0}};
        const auto loop = drawn(NVB197_SET_VERTEX_ARRAY_START, 10, 6, 3);
        want.insert(want.end(), loop.begin(), loop.end());
        EXPECT_EQ(f.methods, want) << primitive;
        EXPECT_EQ(f.nops, raw::icb::kSlotWords - 8);
    }
}

TEST(RawIcb, AnIndexedDrawBindsItsIndicesThenDrawsAsAnIndexedIndirectDraw)
{
    for (uint32_t size : {2u, 4u}) {
        uint32_t slot[raw::icb::kSlotWords];
        const uint64_t va = 0x1234567800ull, last = va + 0xfff;
        raw::icb::indexed(slot, 0, {4, size, va, last, 9, 2, 0, -5, 3});
        Fifo f;
        cpuRange(f, 0, 1);
        f.run(slot, raw::icb::kSlotWords);
        std::vector<Method> want = {{NVB197_SET_INDEX_BUFFER_A, uint32_t(va >> 32)},
                                    {NVB197_SET_INDEX_BUFFER_B, uint32_t(va)},
                                    {NVB197_SET_INDEX_BUFFER_C, uint32_t(last >> 32)},
                                    {NVB197_SET_INDEX_BUFFER_D, uint32_t(last)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART_INDEX, size == 2 ? 0xffffu : 0xffffffffu},
                                    {NVB197_SET_INDEX_BUFFER_E, size == 2 ? 1u : 2u},
                                    {NVB197_SET_PRIMITIVE_TOPOLOGY, raw::icb::topology(4)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART, 1},
                                    {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                                    {NVB197_LOAD_CONSTANT_BUFFER(0), uint32_t(-5)},
                                    {NVB197_LOAD_CONSTANT_BUFFER(1), 3},
                                    {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 3},
                                    {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, uint32_t(-5)},
                                    {NVB197_SET_VERTEX_ID_BASE, uint32_t(-5)}};
        const auto loop = drawn(NVB197_SET_INDEX_BUFFER_F, 0, 9, 2);
        want.insert(want.end(), loop.begin(), loop.end());
        EXPECT_EQ(f.methods, want) << size;
        EXPECT_EQ(f.nops, raw::icb::kSlotWords - raw::icb::kCallWord - 7);
        EXPECT_EQ(slot[raw::icb::kCallWord],
                  raw::Push::hdr(5, raw::SUBC_3D, 0x3800 + 8 * raw::mme::kIcbDrawIndexed, 6));
    }
}

TEST(RawIcb, AnEmptyCommandAndARangeOfThemDoNothing)
{
    std::vector<uint32_t> range(3 * raw::icb::kSlotWords);
    raw::icb::empty(range.data());
    raw::icb::draw(range.data() + raw::icb::kSlotWords, 1, {3, 3, 1, 0, 0});
    raw::icb::empty(range.data() + raw::icb::kSlotWords);
    raw::icb::empty(range.data() + 2 * raw::icb::kSlotWords);
    Fifo f;
    f.run(range.data(), range.size());
    EXPECT_TRUE(f.methods.empty());
    EXPECT_EQ(f.nops, range.size());
}

TEST(RawIcb, CommandsRunBackToBackInOneRange)
{
    std::vector<uint32_t> range(2 * raw::icb::kSlotWords);
    raw::icb::draw(range.data(), 0, {3, 3, 1, 0, 0});
    raw::icb::draw(range.data() + raw::icb::kSlotWords, 1, {1, 4, 2, 8, 0});
    Fifo f;
    cpuRange(f, 0, 2);
    f.run(range.data(), range.size());
    size_t begins = 0;
    for (const Method &m : f.methods)
        begins += m.first == NVB197_BEGIN;
    EXPECT_EQ(begins, 3u) << "one instance of the first, two of the second";
    EXPECT_EQ(f.methods.back(), Method(NVB197_END, 0));
}

std::vector<uint32_t> fourCommands()
{
    std::vector<uint32_t> words(4 * raw::icb::kSlotWords);
    for (uint32_t i = 0; i < 4; i++)
        raw::icb::draw(words.data() + i * raw::icb::kSlotWords, i, {3, 3, 1, 3 * i, 0});
    return words;
}

std::vector<uint32_t> firstVertices(const std::vector<Method> &methods)
{
    std::vector<uint32_t> out;
    for (const Method &m : methods)
        if (m.first == NVB197_SET_VERTEX_ARRAY_START)
            out.push_back(m.second);
    return out;
}

TEST(RawIcb, ARangeTheGpuReadsDrawsItsCommandsAlone)
{
    const std::vector<uint32_t> words = fourCommands();
    const std::vector<std::pair<uint32_t, uint32_t>> ranges = {{0, 4}, {1, 2}, {3, 1}, {2, 0}, {4, 3}, {3, 9}};
    for (const auto &[location, length] : ranges) {
        Fifo f;
        uint32_t call[1];
        raw::Push p{call, call + 1};
        raw::mme::icbRangeHeader(p);
        const uint32_t memory[] = {location, length};
        std::vector<uint32_t> stream(call, call + 1);
        stream.insert(stream.end(), memory, memory + 2);
        f.run(stream.data(), stream.size());
        EXPECT_EQ(f.scratch[raw::mme::kScratchLocation], location);
        EXPECT_EQ(f.scratch[raw::mme::kScratchLength], length);
        f.run(words.data(), words.size());
        std::vector<uint32_t> want;
        for (uint32_t i = 0; i < 4; i++)
            if (i >= location && i - location < length)
                want.push_back(3 * i);
        EXPECT_EQ(firstVertices(f.methods), want) << location << "+" << length;
    }
}

TEST(RawIcb, ACommandOutsideTheCpuRangeDrawsNothing)
{
    const std::vector<uint32_t> words = fourCommands();
    Fifo f;
    cpuRange(f, 2, 1);
    f.run(words.data(), words.size());
    EXPECT_EQ(firstVertices(f.methods), std::vector<uint32_t>{6});
}

constexpr uint64_t kSet = 0x1122334400ull, kRoot = 0x5566778800ull;
constexpr uint32_t kControl = 0x700 + 4 * 4;

// An execute's layout of a state whose factor and control-point index buffers are at set-0 bytes 0x40 and 0x80 and which
// reads no patch index buffer, in a channel whose install() wrote the root table's address
void patchLayout(Fifo &f)
{
    f.scratch[SWITCH_GM20B_SCRATCH_ROOT_HI] = uint32_t(kRoot >> 32);
    f.scratch[SWITCH_GM20B_SCRATCH_ROOT_HI + 1] = uint32_t(kRoot);
    uint32_t words[7];
    raw::Push p{words, words + 7};
    raw::mme::icbPatchLayout(p, kSet, {0x40, raw::mme::kNoEntry, 0x80}, kControl);
    EXPECT_EQ(p.at, p.end);
    f.run(words, size_t(p.at - words));
}

raw::icb::PatchDraw patchDraw()
{
    raw::icb::PatchDraw d{};
    d.count = 2;
    d.instances = 3;
    d.first = 5;
    d.baseInstance = 1;
    for (uint32_t k = 0; k < 3; k++)
        for (uint32_t w = 0; w < 3; w++)
            d.buffers[k][w] = 0x100 * (k + 1) + w;
    for (uint32_t w = 0; w < 6; w++)
        d.control[w] = 0xc0 + w;
    return d;
}

// What a patch command loads before it draws: the factor and control-point index descriptors into set 0, the push words
// into the root table, the root table selected again
std::vector<Method> patchLoads(const raw::icb::PatchDraw &d)
{
    std::vector<Method> m = {{NVB197_SET_PRIMITIVE_TOPOLOGY, raw::icb::topology(raw::icb::kPatchPrimitive)},
                             {NVB197_SET_DA_PRIMITIVE_RESTART, 0},
                             {NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, raw::mme::kIcbSetBytes},
                             {NVB197_SET_CONSTANT_BUFFER_SELECTOR_B, uint32_t(kSet >> 32)},
                             {NVB197_SET_CONSTANT_BUFFER_SELECTOR_C, uint32_t(kSet)}};
    for (const auto &[k, at] : {std::pair<uint32_t, uint32_t>{0, 0x40}, {2, 0x80}}) {
        m.emplace_back(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, at);
        for (uint32_t w = 0; w < 3; w++)
            m.emplace_back(NVB197_LOAD_CONSTANT_BUFFER(w), d.buffers[k][w]);
        m.emplace_back(NVB197_LOAD_CONSTANT_BUFFER(3), 0);
    }
    m.insert(m.end(), {{NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, raw::mme::kIcbRootBytes},
                       {NVB197_SET_CONSTANT_BUFFER_SELECTOR_B, uint32_t(kRoot >> 32)},
                       {NVB197_SET_CONSTANT_BUFFER_SELECTOR_C, uint32_t(kRoot)},
                       {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kControl}});
    for (uint32_t w = 0; w < 6; w++)
        m.emplace_back(NVB197_LOAD_CONSTANT_BUFFER(w), d.control[w]);
    m.insert(m.end(), {{NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                       {NVB197_LOAD_CONSTANT_BUFFER(0), d.first},
                       {NVB197_LOAD_CONSTANT_BUFFER(1), d.baseInstance},
                       {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, d.baseInstance},
                       {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, 0},
                       {NVB197_SET_VERTEX_ID_BASE, 0}});
    return m;
}

TEST(RawIcb, APatchCommandLoadsItsControlStageWordsWhereTheLayoutSaysThenDrawsItsPatches)
{
    const raw::icb::PatchDraw d = patchDraw();
    uint32_t slot[raw::icb::kPatchSlotWords];
    raw::icb::patches(slot, 4, d);
    EXPECT_EQ(slot[raw::icb::kIndexWord], 4u);
    EXPECT_EQ(slot[raw::icb::kCallWord],
              raw::Push::hdr(5, raw::SUBC_3D, 0x3800 + 8 * raw::mme::kIcbPatches, raw::mme::kIcbPatchesArguments));
    Fifo f;
    patchLayout(f);
    cpuRange(f, 4, 1);
    f.run(slot, raw::icb::kPatchSlotWords);
    std::vector<Method> want = patchLoads(d);
    const auto loop = drawn(NVB197_SET_VERTEX_ARRAY_START, 5, 2, 3);
    want.insert(want.end(), loop.begin(), loop.end());
    EXPECT_EQ(f.methods, want);
}

TEST(RawIcb, APatchCommandOutsideTheRangeLoadsItsWordsAndDrawsNothing)
{
    const raw::icb::PatchDraw d = patchDraw();
    uint32_t slot[raw::icb::kPatchSlotWords];
    raw::icb::patches(slot, 4, d);
    Fifo f;
    patchLayout(f);
    cpuRange(f, 0, 4);
    f.run(slot, raw::icb::kPatchSlotWords);
    EXPECT_EQ(f.methods, patchLoads(d));
}

TEST(RawIcb, CommandsOfAPatchBufferTakeItsSlotsWithTheirCallAtTheSameWord)
{
    std::vector<uint32_t> words(2 * raw::icb::kPatchSlotWords);
    raw::icb::draw(words.data(), 0, {3, 3, 1, 0, 0}, raw::icb::kPatchSlotWords);
    raw::icb::patches(words.data() + raw::icb::kPatchSlotWords, 1, patchDraw());
    EXPECT_EQ(words[raw::icb::kIndexWord], 0u);
    EXPECT_EQ(words[raw::icb::kPatchSlotWords + raw::icb::kIndexWord], 1u);
    Fifo f;
    patchLayout(f);
    cpuRange(f, 0, 2);
    f.run(words.data(), words.size());
    size_t begins = 0;
    for (const Method &m : f.methods)
        begins += m.first == NVB197_BEGIN;
    EXPECT_EQ(begins, 1u + 3u);
}

TEST(RawIcb, ACopyRewritesTheIndexWordAlone)
{
    std::vector<uint32_t> a(raw::icb::kSlotWords), b(raw::icb::kSlotWords);
    raw::icb::draw(a.data(), 3, {3, 3, 1, 9, 0});
    raw::icb::draw(b.data(), 0, {3, 3, 1, 9, 0});
    for (uint32_t i = 0; i < raw::icb::kSlotWords; i++)
        if (i != raw::icb::kIndexWord)
            EXPECT_EQ(a[i], b[i]) << i;
}

}  // namespace
