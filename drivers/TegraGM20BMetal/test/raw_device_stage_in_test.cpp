// SPDX-License-Identifier: MIT
// A stage-in kernel's words on raw::Device: a compute encoder's dispatch writes setStageInRegion:'s origin and size and a
// bind's stride into the StageInRegion and StageInStride push words of the root table its QMD names (read off the
// device's submitted words and the memory they point at), and an indirect dispatch command's region writes its own into
// its region's root table in ICB memory, or, inheriting the encoder's buffers, its bind slot writes the encoder's stride
// there at each execute; a kernel of no stage-in takes neither.
#include "raw_device_harness.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
// the class headers' nv_push_compat.h and Mesa's util/macros.h spell MIN2 and MAX2 apart
#undef MIN2
#undef MAX2
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_icb_compute.h"
#include <algorithm>
#include <map>
#include <vector>

namespace {

constexpr uint32_t kRootPush = 0x700, kRegionWord = 4, kStrideWord = 13, kQmdRootLo = 29;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));

// A kernel whose constant buffer 0 is the root table, reading a stage-in region and buffer 1's bind stride where
// `stageIn`
raw::ComputeRecord kernel(bool stageIn)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufType[0] = raw::CbufRoot;
    r.cbufs = 1;
    r.kernel.regionWord = -1;
    r.kernel.qmd[kQmdCbufValid] |= 1;
    if (stageIn) {
        r.kernel.stageInRegionWord = kRegionWord;
        r.kernel.stageInStride[0][0] = 1;
        r.kernel.stageInStride[0][1] = kStrideWord;
    }
    return r;
}

// placeCompute keeps a state by its record's address: one record of each form for every test
const raw::ComputeRecord &stageInKernel()
{
    static const raw::ComputeRecord r = kernel(true);
    return r;
}

const raw::ComputeRecord &plainKernel()
{
    static const raw::ComputeRecord r = kernel(false);
    return r;
}

// What the CPU wrote at `va` (the kernel model's memory of the BO bound there)
uint32_t word(uint64_t va)
{
    for (const auto &[at, bound] : kernel_model::model.binds) {
        const auto bytes = kernel_model::model.memory.find(uint32_t(bound.first));
        if (va < at || va + 4 > at + bound.second || bytes == kernel_model::model.memory.end() ||
            va - at + 4 > bytes->second.size())
            continue;
        uint32_t w;
        memcpy(&w, bytes->second.data() + (va - at), 4);
        return w;
    }
    return 0xdeadbeef;
}

class DeviceStageIn : public harness::DeviceTest {
protected:
    // One dispatch of `r` at region {3, 5, 7} + {64, 2, 1}, buffer 1 bound at stride 24: the root table's push words its
    // QMD names
    std::vector<uint32_t> dispatched(const raw::ComputeRecord &r)
    {
        const raw::ComputeState *k = raw::placeCompute(dev, r);
        EXPECT_NE(k, nullptr);
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!k || !cb)
            return {};
        const size_t before = kernel_model::model.jobs.size();
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            e.setBufferStride(1, 24);
            e.setStageInRegion({3, 5, 7, 64, 2, 1});
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *why = nullptr;
            EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        uint64_t qmd = 0;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
            const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
            for (size_t i = 0; i + 1 < w.size(); i++) {
                const uint32_t h = w[i], op = h >> 29, subc = h >> 13 & 7, m = (h & 0xfff) << 2;
                if (subc == raw::SUBC_COMPUTE && m == NVB1C0_SEND_PCAS_A)
                    qmd = uint64_t(op == 4 ? h >> 16 & 0x1fff : w[i + 1]) << 8;
            }
        }
        EXPECT_NE(qmd, 0u) << "no launch";
        const uint64_t root = uint64_t(word(qmd + 4 * (kQmdRootLo + 1)) & 0xff) << 32 | word(qmd + 4 * kQmdRootLo);
        std::vector<uint32_t> push(32);
        for (uint32_t i = 0; i < 32; i++)
            push[i] = word(root + kRootPush + 4 * i);
        return push;
    }
};

TEST_F(DeviceStageIn, ADispatchWritesItsRegionAndABindsStrideIntoItsRootTable)
{
    const std::vector<uint32_t> push = dispatched(stageInKernel());
    ASSERT_EQ(push.size(), 32u);
    const uint32_t region[8] = {3, 5, 7, 0, 64, 2, 1, 0};
    for (uint32_t w = 0; w < 8; w++)
        EXPECT_EQ(push[kRegionWord - 1 + w], region[w]) << w;
    EXPECT_EQ(push[kStrideWord - 1], 24u);
}

TEST_F(DeviceStageIn, AKernelOfNoStageInTakesNeither)
{
    const std::vector<uint32_t> push = dispatched(plainKernel());
    ASSERT_EQ(push.size(), 32u);
    for (uint32_t w = 0; w < 32; w++)
        EXPECT_EQ(push[w], 0u) << w;
}

TEST_F(DeviceStageIn, AnIndirectDispatchCommandsRegionIsInItsOwnRootTable)
{
    const raw::ComputeState *k = raw::placeCompute(dev, stageInKernel());
    ASSERT_NE(k, nullptr);
    std::vector<uint8_t> block(raw::icb::computeRegionBytes(0));
    raw::ComputeRegion region;
    region.local = {1, 1, 1};
    region.groups = {1, 1, 1};
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    raw::StageIn stageIn;
    stageIn.setRegion({2, 4, 6, 8, 10, 12});
    stageIn.strides[1] = 40;
    const char *why = nullptr;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, region, {1, 1, 1}, {1, 1, 1}, buffers, stageIn, block.data(),
                                               0x123400, &why))
        << (why ? why : "");
    const auto *push = reinterpret_cast<const uint32_t *>(block.data() + raw::icb::kComputeQmdBytes + kRootPush);
    const uint32_t words[8] = {2, 4, 6, 0, 8, 10, 12, 0};
    for (uint32_t w = 0; w < 8; w++)
        EXPECT_EQ(push[kRegionWord - 1 + w], words[w]) << w;
    EXPECT_EQ(push[kStrideWord - 1], 40u);
}

// A kernel whose buffer's residual word lies past the 32 push words (an argument table's): a command's root table has no
// such word, so its region and its binds are refused before either writes anything
TEST_F(DeviceStageIn, AnIndirectDispatchCommandOfASpilledBindOffsetIsRefused)
{
    static const raw::ComputeRecord spilled = [] {
        raw::ComputeRecord r = kernel(false);
        r.kernel.setBytes = 16;
        r.kernel.entries = 1;
        r.kernel.entry[0].kind = raw::KernelBuffer;
        r.kernel.entry[0].window = uint32_t(40 + 1) << 24;
        return r;
    }();
    const raw::ComputeState *k = raw::placeCompute(dev, spilled);
    ASSERT_NE(k, nullptr);
    std::vector<uint8_t> block(raw::icb::computeRegionBytes(k->rec->setBytes), 0xa5);
    raw::ComputeRegion region;
    region.local = {1, 1, 1};
    region.groups = {1, 1, 1};
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    buffers[0] = {0x200008, 64};
    const char *why = nullptr;
    EXPECT_FALSE(raw::ComputeEncoder::icbRegion(dev, *k, region, {1, 1, 1}, {1, 1, 1}, buffers, raw::StageIn{},
                                                block.data(), 0x123400, &why));
    EXPECT_NE(why, nullptr);
    EXPECT_EQ(std::count(block.begin(), block.end(), uint8_t(0xa5)), ptrdiff_t(block.size()));
    std::vector<raw::icb::ComputeBind> descriptors;
    std::vector<raw::icb::ComputeStride> strides;
    why = nullptr;
    EXPECT_FALSE(raw::ComputeEncoder::icbBinds(*k, 0x123400, descriptors, strides, &why));
    EXPECT_NE(why, nullptr);
    EXPECT_TRUE(descriptors.empty());
}

// The 3D methods of the words raw::Device submitted, as the channel runs them: scratch writes, kIcbBind through the
// Fermi simulator, and the 3D class's inline-to-memory writes into `memory`
struct BindChannel {
    std::map<uint64_t, uint32_t> memory;
    switch_gm20b_test::Scratch scratch;
    uint32_t inlineLeft = 0;
    uint64_t out = 0;
    std::vector<uint32_t> arguments;
    size_t consumed = 0;

    static bool isScratch(uint32_t m)
    {
        return m >= NVB197_SET_MME_SHADOW_SCRATCH(0) && m < NVB197_CALL_MME_MACRO(0);
    }

    void method(uint32_t m, uint32_t v)
    {
        switch_gm20b_test::expectMethod(switch_gm20b_test::subchannelClass(raw::SUBC_3D, m), m);
        if (isScratch(m))
            scratch.method(m) = v;
        else if (m == NVB197_OFFSET_OUT_UPPER)
            out = (out & 0xffffffffull) | uint64_t(v & 0xff) << 32;
        else if (m == NVB197_OFFSET_OUT)
            out = (out & ~0xffffffffull) | v;
        else if (m == NVB197_LINE_LENGTH_IN)
            inlineLeft = v / 4;
        else if (m == NVB197_LOAD_INLINE_DATA && inlineLeft) {
            memory[out] = v;
            out += 4;
            inlineLeft--;
        }
    }

    static uint32_t load(void *opaque)
    {
        auto &c = *static_cast<BindChannel *>(opaque);
        return c.consumed < c.arguments.size() ? c.arguments[c.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t m)
    {
        return isScratch(m) ? static_cast<BindChannel *>(opaque)->scratch.method(m) : 0;
    }

    static void emit(void *opaque, uint16_t m, uint32_t v)
    {
        static_cast<BindChannel *>(opaque)->method(m, v);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void run(const std::vector<uint32_t> &w)
    {
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = h >> 16 & 0x1fff, subc = h >> 13 & 7, m = (h & 0xfff) << 2;
            if (op == 4) {
                if (subc == raw::SUBC_3D)
                    method(m, count);
                continue;
            }
            if (i + count > w.size())
                return;
            if (subc == raw::SUBC_3D && m == NVB197_CALL_MME_MACRO(raw::mme::kIcbBind)) {
                arguments.assign(w.begin() + ptrdiff_t(i), w.begin() + ptrdiff_t(i + count));
                consumed = 0;
                const raw::mme::Program &p = raw::mme::kIcbBindProgram;
                std::vector<mme_fermi_inst> decoded(p.size);
                mme_fermi_decode(decoded.data(), p.words, p.size);
                const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
                mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
                EXPECT_EQ(consumed, arguments.size());
            } else if (subc == raw::SUBC_3D && m < NVB197_CALL_MME_MACRO(0)) {
                for (uint32_t k = 0; k < count; k++)
                    method(op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i + k]);
            }
            i += count;
        }
    }
};

// A buffer that inherits the encoder's buffers writes a stage-in command's region once, its stride word 0: each execute's
// bind slot writes the encoder's bind stride there, 24 at the first and 40 at the second, and 0 once none is bound
TEST_F(DeviceStageIn, AnInheritingCommandTakesEachExecutesBindStride)
{
    const raw::ComputeState *k = raw::placeCompute(dev, stageInKernel());
    ASSERT_NE(k, nullptr);
    // write-combined: the kernel model reads a submission's words from memory a BoAlloc made, as an ICB's are
    raw::Buffer block, slots, binds, bound;
    ASSERT_TRUE(dev.createBuffer(raw::icb::computeRegionBytes(0), block));
    ASSERT_TRUE(dev.createBuffer(raw::icb::kComputeSlotWords * 4, slots, true));
    ASSERT_TRUE(dev.createBuffer(raw::icb::kComputeBindSlotWords * 4, binds, true));
    ASSERT_TRUE(dev.createBuffer(4096, bound));
    raw::ComputeRegion region;
    region.local = {1, 1, 1};
    region.groups = {1, 1, 1};
    const raw::ComputeEncoder::IcbBuffer none[31] = {};
    const char *why = nullptr;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, region, {1, 1, 1}, {1, 1, 1}, none, raw::StageIn{}, block.cpu,
                                               block.va, &why))
        << (why ? why : "");
    std::vector<raw::icb::ComputeBind> descriptors;
    std::vector<raw::icb::ComputeStride> strides;
    ASSERT_TRUE(raw::ComputeEncoder::icbBinds(*k, block.va, descriptors, strides, &why)) << (why ? why : "");
    ASSERT_EQ(strides.size(), 1u);
    const uint64_t word = block.va + raw::icb::kComputeQmdBytes + kRootPush + 4 * (kStrideWord - 1);
    EXPECT_EQ(strides[0].word, word);
    raw::icb::ComputeCommand command{};
    command.qmd[0] = block.va;
    command.regions = 1;
    raw::icb::dispatch(reinterpret_cast<uint32_t *>(slots.cpu), 0, command);
    ASSERT_TRUE(raw::icb::binds(reinterpret_cast<uint32_t *>(binds.cpu), descriptors.data(),
                                uint32_t(descriptors.size()), strides.data(), uint32_t(strides.size())));
    for (const uint32_t stride : {24u, 40u, raw::kNoBindStride}) {
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t before = kernel_model::model.jobs.size();
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setBuffer(1, &bound, 0);
            e.setBufferStride(1, stride);
            raw::ComputeEncoder::IcbExecute x = {&slots, 0, 0, 1, nullptr, 0, false};
            x.binds = &binds;
            x.strideSlots = 1u << 1;
            EXPECT_TRUE(e.executeSlots(x, &why)) << (why ? why : "");
            e.end();
        }
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        BindChannel channel;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            channel.run(kernel_model::model.jobs[j].words);
        ASSERT_EQ(channel.memory.count(word), 1u) << stride;
        EXPECT_EQ(channel.memory.at(word), stride == raw::kNoBindStride ? 0u : stride);
    }
}

}  // namespace
