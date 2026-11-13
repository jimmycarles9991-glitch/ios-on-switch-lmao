// SPDX-License-Identifier: MIT
// raw/runtime/raw_icb_compute.h and raw_mme.h's gate macro: an indirect command buffer's dispatch commands, run through the
// host FIFO's method decoding and the Fermi MME simulator with the macros the channel loads, after the head an execute sends
// (the CPU's range as scratch words, or kIcbRange reading one from memory) and before its tail. The model of the compute
// class keeps the global render enable the gate sets and counts the launches it lets through: exactly the regions of the
// commands inside the range launch, a barrier flushes before its command whatever the range, an empty command is no-ops,
// and the tail leaves launches enabled. A command's stage-in words (raw_stage_in.h) are its own region and bind strides. A
// buffer that inherits the encoder's buffers: the execute's buffers in scratch and the commands' bind slots through
// kIcbBind, whose inline-to-memory writes the model of the 3D class lands in memory, write every descriptor and residual
// word as the CPU's raw_residual.h bufferDescriptor does for the same buffers, and each stage-in stride word the encoder's
// bind stride.
#include <gtest/gtest.h>
#include "../../TegraGM20B/TegraGM20BGpu.h"
#include "../raw/runtime/raw_icb_compute.h"
#include "../raw/runtime/raw_residual.h"
#include "../raw/runtime/raw_stage_in.h"
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace {

struct Method {
    uint32_t subc;
    uint16_t method;
    uint32_t value;
};

struct Channel {
    std::vector<Method> methods;
    std::vector<uint32_t> arguments, launched;  // the QMD addresses (>> 8) launched while enabled
    size_t consumed = 0;
    switch_gm20b_test::Scratch scratch;
    uint32_t enable = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE, pendingQmd = 0, flushes = 0;
    // the 3D class's inline-to-memory: its destination and length, then each LOAD_INLINE_DATA word into memory
    std::map<uint64_t, uint32_t> memory;
    uint64_t out = 0;
    uint32_t lineBytes = 0, lines = 0, inlineLeft = 0, waits3d = 0;

    static bool isScratch(uint16_t method)
    {
        return method >= NVB197_SET_MME_SHADOW_SCRATCH(0) && method < NVB197_CALL_MME_MACRO(0);
    }

    static uint32_t load(void *opaque)
    {
        auto &c = *static_cast<Channel *>(opaque);
        EXPECT_LT(c.consumed, c.arguments.size()) << "a macro read past the words its call sent";
        return c.consumed < c.arguments.size() ? c.arguments[c.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        EXPECT_TRUE(isScratch(method)) << "the gate reads no state but its scratch";
        return isScratch(method) ? static_cast<Channel *>(opaque)->scratch.method(method) : 0;
    }

    static void emit(void *opaque, uint16_t m, uint32_t value)
    {
        static_cast<Channel *>(opaque)->method(raw::SUBC_3D, m, value);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void method(uint32_t subc, uint16_t m, uint32_t value)
    {
        switch_gm20b_test::expectMethod(switch_gm20b_test::subchannelClass(subc, m), m);
        if (subc == raw::SUBC_3D && isScratch(m)) {
            scratch.method(m) = value;
            return;
        }
        methods.push_back({subc, m, value});
        if (subc == raw::SUBC_3D)
            inlineToMemory(m, value);
        if (m == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C)
            enable = value;
        if (subc != raw::SUBC_COMPUTE)
            return;
        if (m == NVB1C0_SEND_PCAS_A)
            pendingQmd = value;
        if (m == NVB1C0_SEND_SIGNALING_PCAS_B && enable == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE)
            launched.push_back(pendingQmd);
        if (m == NVB1C0_INVALIDATE_SHADER_CACHES)
            flushes++;
    }

    void inlineToMemory(uint16_t m, uint32_t value)
    {
        switch (m) {
        case NVB197_OFFSET_OUT_UPPER:
            out = (out & 0xffffffffull) | uint64_t(value & 0xff) << 32;
            break;
        case NVB197_OFFSET_OUT:
            out = (out & ~0xffffffffull) | value;
            break;
        case NVB197_LINE_LENGTH_IN:
            lineBytes = value;
            break;
        case NVB197_LINE_COUNT:
            lines = value;
            break;
        case NVB197_LAUNCH_DMA:
            EXPECT_EQ(value & 1, uint32_t(NVB197_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH));
            EXPECT_EQ(lines, 1u);
            EXPECT_EQ(lineBytes % 4, 0u);
            inlineLeft = lineBytes / 4;
            break;
        case NVB197_LOAD_INLINE_DATA:
            ASSERT_GT(inlineLeft, 0u) << "inline data past its LAUNCH_DMA's length";
            memory[out] = value;
            out += 4;
            inlineLeft--;
            break;
        case NVB197_WAIT_FOR_IDLE:
            waits3d++;
            break;
        default:
            break;
        }
    }

    void macro(uint32_t slot)
    {
        const raw::mme::Program &p = slot == raw::mme::kIcbRange  ? raw::mme::kIcbRangeProgram
                                     : slot == raw::mme::kIcbBind ? raw::mme::kIcbBindProgram
                                                                  : raw::mme::kIcbGateProgram;
        ASSERT_TRUE(slot == raw::mme::kIcbRange || slot == raw::mme::kIcbGate || slot == raw::mme::kIcbBind) << slot;
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
            const uint32_t header = w[i++], op = header >> 29, count = header >> 16 & 0x1fff, subc = header >> 13 & 7,
                           method = (header & 0xfff) << 2;
            if (method >= raw::mme::kMacroCall) {
                ASSERT_EQ(subc, uint32_t(raw::SUBC_3D));
                ASSERT_EQ(op, 5u);
                ASSERT_LE(i + count, n);
                arguments.assign(w + i, w + i + count);
                i += count;
                macro((method - raw::mme::kMacroCall) / 8);
                continue;
            }
            if (op == 4) {
                if (method != NVB197_NO_OPERATION)
                    this->method(subc, uint16_t(method), count);
                continue;
            }
            ASSERT_TRUE(op == 1 || op == 3);
            ASSERT_LE(i + count, n);
            for (uint32_t k = 0; k < count; k++)
                this->method(subc, uint16_t(op == 1 ? method + 4 * k : method), w[i++]);
        }
    }

    void head(bool written, bool fromMemory, uint32_t first, uint32_t count, const uint32_t *memoryRange = nullptr)
    {
        uint32_t words[16];
        raw::Push p{words, words + 16};
        raw::icb::computeExecuteHead(p, written, fromMemory, first, count);
        if (!fromMemory) {
            run(words, size_t(p.at - words));
            return;
        }
        // the call's header ends the head; its two data words are the GPFIFO continuation at the range buffer
        std::vector<uint32_t> all(words, p.at);
        all.insert(all.end(), memoryRange, memoryRange + 2);
        run(all.data(), all.size());
    }

    void tail()
    {
        uint32_t words[4];
        raw::Push p{words, words + 4};
        raw::icb::computeExecuteTail(p);
        run(words, size_t(p.at - words));
    }
};

// Five commands: command i has i % 3 + 1 regions at QMDs 0x10000 * (i + 1) + 0x100 * r; command 2 also has a barrier
std::vector<uint32_t> commands(uint32_t n = 5)
{
    std::vector<uint32_t> slots(n * raw::icb::kComputeSlotWords);
    for (uint32_t i = 0; i < n; i++) {
        raw::icb::ComputeCommand c{};
        c.regions = i % 3 + 1;
        c.barrier = i == 2;
        for (uint32_t r = 0; r < c.regions; r++)
            c.qmd[r] = 0x10000ull * (i + 1) + 0x100 * r;
        raw::icb::dispatch(slots.data() + i * raw::icb::kComputeSlotWords, i, c);
    }
    return slots;
}

std::vector<uint32_t> launches(uint32_t location, uint32_t length, uint32_t n = 5)
{
    std::vector<uint32_t> out;
    for (uint32_t i = location; i < location + length && i < n; i++)
        for (uint32_t r = 0; r < i % 3 + 1; r++)
            out.push_back(uint32_t((0x10000ull * (i + 1) + 0x100 * r) >> 8));
    return out;
}

TEST(RawIcbCompute, ACommandIsItsBarrierItsGateThenALaunchPerRegion)
{
    const std::vector<uint32_t> slots = commands();
    for (uint32_t i = 0; i < 5; i++) {
        const uint32_t *slot = slots.data() + i * raw::icb::kComputeSlotWords;
        EXPECT_EQ(slot[raw::icb::kComputeCallWord],
                  raw::Push::hdr(5, raw::SUBC_3D, raw::mme::kMacroCall + 8 * raw::mme::kIcbGate, 1));
        EXPECT_EQ(slot[raw::icb::kComputeIndexWord], i);
        Channel c;
        c.head(false, false, i, 1);
        c.run(slot, raw::icb::kComputeSlotWords);
        EXPECT_EQ(c.launched, launches(i, 1));
        EXPECT_EQ(c.flushes, i == 2 ? 1u : 0u);
    }
}

TEST(RawIcbCompute, ACpuRangeLaunchesItsCommandsAlone)
{
    const std::vector<uint32_t> slots = commands();
    const std::pair<uint32_t, uint32_t> ranges[] = {{0, 5}, {1, 2}, {3, 1}, {2, 0}, {4, 3}};
    for (const auto &[location, length] : ranges) {
        Channel c;
        c.head(false, false, location, length);
        c.run(slots.data(), slots.size());
        EXPECT_EQ(c.launched, launches(location, length)) << location << "+" << length;
        EXPECT_EQ(c.flushes, 1u) << "a barrier flushes whether or not its command launches";
        c.tail();
        EXPECT_EQ(c.enable, uint32_t(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    }
}

TEST(RawIcbCompute, ARangeFromMemoryGatesEveryCommand)
{
    const std::vector<uint32_t> slots = commands();
    const std::pair<uint32_t, uint32_t> ranges[] = {{0, 4}, {1, 2}, {3, 1}, {2, 0}, {4, 3}, {3, 9}};
    for (const auto &[location, length] : ranges) {
        Channel c;
        const uint32_t range[2] = {location, length};
        c.head(false, true, 0, 0, range);
        EXPECT_EQ(c.scratch[raw::mme::kScratchLocation], location);
        EXPECT_EQ(c.scratch[raw::mme::kScratchLength], length);
        c.run(slots.data(), slots.size());
        EXPECT_EQ(c.launched, launches(location, length)) << location << "+" << length;
        c.tail();
        EXPECT_EQ(c.enable, uint32_t(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    }
}

TEST(RawIcbCompute, WordsTheGpuWroteAreWaitedForBeforeTheyAreFetched)
{
    for (const bool fromMemory : {false, true}) {
        Channel c;
        const uint32_t range[2] = {0, 1};
        c.head(true, fromMemory, 0, 1, range);
        ASSERT_GE(c.methods.size(), 3u);
        EXPECT_EQ(c.methods[0].method, NVB1C0_WAIT_FOR_IDLE);
        EXPECT_EQ(c.methods[0].subc, uint32_t(raw::SUBC_COMPUTE));
        EXPECT_EQ(c.methods[1].method, NVB1C0_INVALIDATE_SHADER_CACHES);
        EXPECT_EQ(c.methods[2].method, 0x0050u);
    }
    Channel plain;
    plain.head(false, false, 0, 1);
    EXPECT_TRUE(plain.methods.empty()) << "a CPU-written buffer's CPU range is scratch words alone";
}

TEST(RawIcbCompute, AnEmptyCommandIsNoOperations)
{
    uint32_t slot[raw::icb::kComputeSlotWords];
    raw::icb::computeEmpty(slot);
    Channel c;
    c.run(slot, raw::icb::kComputeSlotWords);
    EXPECT_TRUE(c.methods.empty());
}

TEST(RawIcbCompute, ACopyToAnotherIndexDiffersInItsIndexWordAlone)
{
    raw::icb::ComputeCommand c{};
    c.regions = raw::icb::kComputeRegions;
    c.barrier = true;
    for (uint32_t r = 0; r < c.regions; r++)
        c.qmd[r] = 0x7700000000ull + 0x100 * r;
    uint32_t at3[raw::icb::kComputeSlotWords], at9[raw::icb::kComputeSlotWords];
    raw::icb::dispatch(at3, 3, c);
    raw::icb::dispatch(at9, 9, c);
    for (uint32_t w = 0; w < raw::icb::kComputeSlotWords; w++)
        EXPECT_EQ(at3[w] == at9[w], w != raw::icb::kComputeIndexWord) << w;
    Channel all;
    all.head(false, false, 3, 1);
    all.run(at3, raw::icb::kComputeSlotWords);
    EXPECT_EQ(all.launched.size(), size_t(raw::icb::kComputeRegions));
}

TEST(RawIcbCompute, ARegionsBlockHoldsItsQmdRootTableAndSetOnTheirAlignment)
{
    EXPECT_EQ(raw::icb::computeRegionBytes(0), 256u + 0x800u + 256u);
    EXPECT_EQ(raw::icb::computeRegionBytes(0x1f0), 256u + 0x800u + 512u);
    EXPECT_EQ(raw::icb::computeRegionBytes(0x1000), 256u + 0x800u + 0x1000u);
    for (const uint32_t set : {0u, 16u, 0x1f0u, 0x1000u})
        EXPECT_EQ(raw::icb::computeRegionBytes(set) % raw::icb::kComputeBlockAlign, 0u);
}

TEST(RawIcbCompute, ACommandsStageInWordsAreItsRegionAndItsBindStrides)
{
    raw::PackKernel k{};
    k.stageInRegionWord = 4;
    k.stageInStride[0][0] = 1;
    k.stageInStride[0][1] = 13;
    k.stageInStride[1][0] = 2;
    k.stageInStride[1][1] = 14;
    raw::StageIn s;
    s.setRegion({3, 5, 7, 0x100000040ull, 2, 1});
    s.strides[1] = 24;
    uint32_t push[32];
    std::fill(std::begin(push), std::end(push), 0xeeeeeeeeu);
    raw::stageInWords(k, s, push);
    const uint32_t region[8] = {3, 5, 7, 0, 0x40, 2, 1, 0};
    for (uint32_t w = 0; w < 8; w++)
        EXPECT_EQ(push[3 + w], region[w]) << w;
    EXPECT_EQ(push[12], 24u);
    EXPECT_EQ(push[13], 0u) << "a bind of no stride reads at 0";
    EXPECT_EQ(push[2], 0xeeeeeeeeu);
    EXPECT_EQ(push[14], 0xeeeeeeeeu);
}

TEST(RawIcbCompute, AKernelOfNoStageInTakesNoStageInWords)
{
    const raw::PackKernel k{};
    raw::StageIn s;
    s.setRegion({1, 2, 3, 4, 5, 6});
    uint32_t push[32];
    std::fill(std::begin(push), std::end(push), 0x5au);
    raw::stageInWords(k, s, push);
    for (uint32_t w = 0; w < 32; w++)
        EXPECT_EQ(push[w], 0x5au) << w;
}

}  // namespace

// An execute of commands that inherit the encoder's buffers: each command's descriptors, as bufferDescriptor makes them for
// the buffers bound now, land where the command's regions read them, and the residual of each base in its root word; a
// buffer bound again between two executes takes its new descriptor at the second, through the same bind slots.
TEST(RawIcbCompute, BindSlotsWriteTheCpusDescriptorsForTheEncodersBuffers)
{
    std::mt19937_64 random(0xb1d5);
    const uint64_t blocks = 0xc0'1234'0000ull;
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<std::vector<uint32_t>> slots;
    std::vector<int> words;
    const uint32_t windows[] = {0, 16, 0x400, 0x10000};
    for (uint32_t command = 0; command < 6; command++) {
        std::vector<raw::icb::ComputeBind> own;
        for (uint32_t i = 0; i < 3 + command % 3; i++) {
            const int word = (command + i) % 4 == 3 ? -1 : int((command * 7 + i) % 32);
            const uint64_t set = blocks + command * 0x2000 + 0x900;
            own.push_back({set + 16 * i, word < 0 ? 0 : set - 0x800 + 0x700 + 4 * uint32_t(word),
                           uint32_t((command * 5 + i * 3) % 31), windows[(command + i) % 4]});
        }
        slots.emplace_back(raw::icb::kComputeBindSlotWords);
        ASSERT_TRUE(raw::icb::binds(slots.back().data(), own.data(), uint32_t(own.size())));
        binds.insert(binds.end(), own.begin(), own.end());
    }
    uint32_t used = 0;
    for (const raw::icb::ComputeBind &b : binds)
        used |= 1u << b.slot;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t va[31] = {};
        uint32_t size[31] = {};
        for (uint32_t s = 0; s < 31; s++) {
            if (s % 11 == 4)
                continue;  // nothing bound
            va[s] = 0xd0'0000'0000ull + (random() % 0x1000'0000ull) * 16 + (s % 3 == 0 ? 0 : random() % 16 & ~3ull);
            size[s] = uint32_t(1 + random() % 0x40000);
        }
        Channel c;
        const uint32_t strides[31] = {};
        uint32_t head[8 + 3 * 31];
        raw::Push p{head, head + std::size(head)};
        raw::icb::computeBindHead(p, va, size, strides, used);
        c.run(head, size_t(p.at - head));
        for (const std::vector<uint32_t> &slot : slots)
            c.run(slot.data(), slot.size());
        uint32_t tail[4];
        raw::Push t{tail, tail + 4};
        raw::icb::computeBindTail(t);
        c.run(tail, size_t(t.at - tail));
        EXPECT_EQ(c.waits3d, 1u);
        for (const raw::icb::ComputeBind &b : binds) {
            raw::BufferDescriptor d;
            const int word = b.residual ? int((b.residual - (b.descriptor & ~0xfffull) - 0x100 - 0x700) / 4) : -1;
            const bool cpu = raw::bufferDescriptor(true, b.window, word < 0 ? -1 : word, va[b.slot], size[b.slot], d);
            if (!cpu)
                continue;  // the execute refuses a binding off 16 bytes of a descriptor with no residual word
            ASSERT_EQ(c.memory.at(b.descriptor), uint32_t(d.base)) << "slot " << b.slot << " pass " << pass;
            ASSERT_EQ(c.memory.at(b.descriptor + 4), uint32_t(d.base >> 32)) << "slot " << b.slot;
            ASSERT_EQ(c.memory.at(b.descriptor + 8), va[b.slot] ? d.range : 0u)
                << "slot " << b.slot << " window " << b.window << " size " << size[b.slot] << " va " << va[b.slot];
            if (b.residual)
                ASSERT_EQ(c.memory.at(b.residual), d.residual) << "slot " << b.slot;
        }
    }
}

// Every GPU address lies below the bits an inherited buffer's scratch word keeps its bind stride in
static_assert(TGM_VA_LIMIT <= 1ull << (32 + SWITCH_GM20B_ICB_STRIDE_SHIFT));

// An execute under the encoder's bind strides: each stage-in stride word of a command takes its buffer's, two executes under
// two strides through the same bind slot, while the descriptor of the same buffer keeps its address alone; an unbound stride
// is 0
TEST(RawIcbCompute, BindSlotsWriteTheEncodersBindStrides)
{
    const uint64_t descriptor = 0xc0'0000'0900ull, word = 0xc0'0000'0100ull + 0x700 + 4 * 12,
                   other = 0xc0'0000'0100ull + 0x700 + 4 * 13;
    const raw::icb::ComputeBind bind = {descriptor, 0, 1, 0};
    const raw::icb::ComputeStride strides[] = {{word, 1}, {other, 2}};
    std::vector<uint32_t> slot(raw::icb::kComputeBindSlotWords);
    ASSERT_TRUE(raw::icb::binds(slot.data(), &bind, 1, strides, 2));
    for (const uint32_t stride : {24u, 40u, raw::icb::kComputeStrideLimit}) {
        uint64_t va[31] = {};
        uint32_t size[31] = {}, stride31[31] = {};
        va[1] = 0xff'1234'5670ull;
        size[1] = 0x1000;
        stride31[1] = stride;
        Channel c;
        uint32_t head[8 + 3 * 31];
        raw::Push p{head, head + std::size(head)};
        raw::icb::computeBindHead(p, va, size, stride31, 0b110);
        c.run(head, size_t(p.at - head));
        c.run(slot.data(), slot.size());
        EXPECT_EQ(c.memory.at(word), stride);
        EXPECT_EQ(c.memory.at(other), 0u);
        EXPECT_EQ(c.memory.at(descriptor), 0x1234'5670u);
        EXPECT_EQ(c.memory.at(descriptor + 4), 0xffu);
    }
}

TEST(RawIcbCompute, ABindSlotHoldsItsCommandsDescriptorsAlone)
{
    std::vector<raw::icb::ComputeBind> many(raw::icb::kComputeBindsMax + 1, {0xc0'0000'0100ull, 0, 1, 0});
    std::vector<uint32_t> slot(raw::icb::kComputeBindSlotWords);
    EXPECT_FALSE(raw::icb::binds(slot.data(), many.data(), uint32_t(many.size())));
    EXPECT_TRUE(raw::icb::binds(slot.data(), many.data(), raw::icb::kComputeBindsMax));
    const std::vector<raw::icb::ComputeStride> strides((raw::icb::kComputeBindSlotWords - 3) / 3,
                                                       {0xc0'0000'0700ull, 1});
    EXPECT_TRUE(raw::icb::binds(slot.data(), nullptr, 0, strides.data(), uint32_t(strides.size())));
    EXPECT_FALSE(raw::icb::binds(slot.data(), many.data(), 1, strides.data(), uint32_t(strides.size())));
    raw::icb::bindsEmpty(slot.data());
    Channel c;
    c.run(slot.data(), slot.size());
    EXPECT_TRUE(c.memory.empty());
    EXPECT_TRUE(c.methods.empty());
}
