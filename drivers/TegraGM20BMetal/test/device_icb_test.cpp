// SPDX-License-Identifier: MIT
// RenderEncoder::executeSlots on gm20b::Device, run off the pushes the device submitted through the host FIFO's decoding and
// the Fermi MME simulator with the channel's macros, constant buffer loads kept at their addresses: an indirect command
// buffer's patch commands each draw with their own factor descriptor and control-stage words in the set 0 and root table
// the inherited tessellation state reads, a command outside the range draws nothing, a draw after the execute reads the
// encoder's own again from a set 0 of its own, and the buffer executed again after that draw draws its commands again. Under vertex amplification an execute draws its range once per
// amplification, each with its id in the state's AmplificationId word.
#include "device_harness.h"
// the class headers' nv_push_compat.h and Mesa's util/macros.h spell MIN2 and MAX2 apart
#undef MIN2
#undef MAX2
#include "../pack/mesa/mme_fermi.h"
#include "../pack/mesa/mme_fermi_sim.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include "../icb.h"
#include "../record.h"
#include "../residual.h"
#include <map>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kControlWord = 5, kIdWord = 3, kViewportWord = 4;
constexpr uint32_t kControlAt = 0x700 + 4 * (kControlWord - 1), kIdAt = 0x700 + 4 * (kIdWord - 1),
                   kViewportAt = 0x700 + 4 * (kViewportWord - 1);

// The 3D methods a word stream sends, run as the channel would: macro calls through the simulator, constant buffer loads
// into `memory` at the selected buffer's address, and at each draw's first BEGIN what it reads
struct Channel {
    struct Draw {
        uint64_t set = 0;
        uint32_t factors[3] = {}, control[6] = {}, amplification = 0, viewport = 0;
    };

    std::map<uint64_t, uint32_t> memory;
    uint64_t selected = 0, root = 0, factorAt = 0;
    uint32_t selector[3] = {}, offset = 0;
    switch_gm20b_test::Scratch scratch;
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    std::vector<Draw> draws;
    std::vector<uint32_t> begins;        // every BEGIN's word
    uint32_t topology = 0;               // SET_PRIMITIVE_TOPOLOGY's, which a fetching execute's macro reads
    std::map<uint32_t, uint32_t> calls;  // macro slot: calls, for a failure's message
    size_t words = 0;

    std::string summary() const
    {
        std::string out = std::to_string(words) + " words; macro calls";
        for (const auto &[slot, n] : calls)
            out += " " + std::to_string(slot) + "x" + std::to_string(n);
        return out + "; range " + std::to_string(scratch.words[gm20b::mme::kScratchLocation]) + "+" +
               std::to_string(scratch.words[gm20b::mme::kScratchLength]);
    }

    // The word the GPU reads at `at`: the last constant buffer load there, else what the CPU wrote (the model's memory)
    uint32_t word(uint64_t at) const
    {
        if (const auto loaded = memory.find(at); loaded != memory.end())
            return loaded->second;
        for (const auto &[va, bound] : kernel_model::model.binds) {
            const auto bytes = kernel_model::model.memory.find(uint32_t(bound.first));
            if (at < va || at + 4 > va + bound.second || bytes == kernel_model::model.memory.end())
                continue;
            uint32_t w = 0;
            if (at - va + 4 <= bytes->second.size())
                memcpy(&w, bytes->second.data() + (at - va), 4);
            return w;
        }
        return 0;
    }

    static bool isScratch(uint32_t m)
    {
        return m >= NVB197_SET_MME_SHADOW_SCRATCH(0) && m < NVB197_CALL_MME_MACRO(0);
    }

    void method(uint32_t m, uint32_t v)
    {
        switch_gm20b_test::expectMethod(switch_gm20b_test::subchannelClass(gm20b::SUBC_3D, m), m);
        if (isScratch(m))
            scratch.method(m) = v;
        else if (m >= NVB197_SET_CONSTANT_BUFFER_SELECTOR_A && m <= NVB197_SET_CONSTANT_BUFFER_SELECTOR_C) {
            selector[(m - NVB197_SET_CONSTANT_BUFFER_SELECTOR_A) / 4] = v;
            selected = uint64_t(selector[1]) << 32 | selector[2];
            if (selector[0] == 0x800)
                root = selected;
        } else if (m == NVB197_LOAD_CONSTANT_BUFFER_OFFSET)
            offset = v;
        else if (m >= NVB197_LOAD_CONSTANT_BUFFER(0) && m <= NVB197_LOAD_CONSTANT_BUFFER(15)) {
            EXPECT_LT(offset, selector[0]) << "a constant buffer load inside the selected buffer";
            memory[selected + offset] = v;
            offset += 4;
        } else if (m == NVB197_SET_PRIMITIVE_TOPOLOGY) {
            topology = v;
        } else if (m == NVB197_BEGIN) {
            begins.push_back(v);
            if (!(v >> 26 & 1))
                began();
        }
    }

    // set 0's factor entry at byte factorAt (the record below), from the set's address the root table holds at 0x38
    void began()
    {
        Draw d;
        d.set = uint64_t(word(root + 0x3c)) << 32 | word(root + 0x38);
        for (uint32_t w = 0; w < 3; w++)
            d.factors[w] = word(d.set + factorAt + 4 * w);
        for (uint32_t w = 0; w < 6; w++)
            d.control[w] = word(root + kControlAt + 4 * w);
        d.amplification = word(root + kIdAt);
        d.viewport = word(root + kViewportAt);
        draws.push_back(d);
    }

    static uint32_t load(void *opaque)
    {
        auto &c = *static_cast<Channel *>(opaque);
        return c.consumed < c.arguments.size() ? c.arguments[c.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t m)
    {
        auto &c = *static_cast<Channel *>(opaque);
        if (m == NVB197_SET_PRIMITIVE_TOPOLOGY)
            return c.topology;
        EXPECT_TRUE(isScratch(m));
        return isScratch(m) ? c.scratch.method(m) : 0;
    }

    static void emit(void *opaque, uint16_t m, uint32_t v)
    {
        static_cast<Channel *>(opaque)->method(m, v);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void macro(uint32_t slot)
    {
        calls[slot]++;
        namespace mme = gm20b::mme;
        const std::map<uint32_t, mme::Program> programs = {{mme::kDraw, mme::kDrawProgram},
                                                           {mme::kDrawIndexed, mme::kIndexedProgram},
                                                           {mme::kIndirect, mme::kIndirectProgram},
                                                           {mme::kIndexedIndirect, mme::kIndexedIndirectProgram},
                                                           {mme::kDrawParams, mme::kDrawParamsProgram},
                                                           {mme::kIndexedParams, mme::kIndexedParamsProgram},
                                                           {mme::kIcbRange, mme::kIcbRangeProgram},
                                                           {mme::kIcbDraw, mme::kIcbDrawProgram},
                                                           {mme::kIcbDrawIndexed, mme::kIcbDrawIndexedProgram},
                                                           {mme::kIcbPatches, mme::kIcbPatchesProgram},
                                                           {mme::kAmplify, mme::kAmplifyProgram},
                                                           {mme::kAmplifyIndexed, mme::kAmplifyIndexedProgram}};
        const auto found = programs.find(slot);
        ASSERT_NE(found, programs.end()) << "macro " << slot;
        std::vector<mme_fermi_inst> decoded(found->second.size);
        mme_fermi_decode(decoded.data(), found->second.words, found->second.size);
        consumed = 0;
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
        EXPECT_EQ(consumed, arguments.size()) << "macro " << slot;
    }

    // Headers as push.h writes them: 1 INC, 3 NON_INC, 4 IMMD, 5 ONE_INC; other subchannels' methods pass by
    void run(const std::vector<uint32_t> &w)
    {
        words += w.size();
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = h >> 16 & 0x1fff, subc = h >> 13 & 7, m = (h & 0xfff) << 2;
            if (op == 4) {
                if (subc == gm20b::SUBC_3D && m != NVB197_NO_OPERATION)
                    method(m, count);
                continue;
            }
            ASSERT_TRUE(op == 1 || op == 3 || op == 5) << std::hex << h << " at " << std::dec << i - 1;
            ASSERT_LE(i + count, w.size());
            if (subc == gm20b::SUBC_3D && m >= gm20b::mme::kMacroCall) {
                arguments.assign(w.begin() + ptrdiff_t(i), w.begin() + ptrdiff_t(i + count));
                macro((m - gm20b::mme::kMacroCall) / 8);
            } else if (subc == gm20b::SUBC_3D) {
                for (uint32_t k = 0; k < count; k++)
                    method(op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i + k]);
            }
            i += count;
        }
    }
};

gm20b::RuntimeRecord tessellation(uint64_t key, uint16_t firstEntry = 0)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    for (gm20b::CapturedProgram &p : r.tessPrograms) {
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + (&p - r.tessPrograms));
        p.id = "icbtess" + std::to_string(&p - r.tessPrograms);
        p.gprs = 12;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    r.pipeline.controlWord = kControlWord;
    r.pipeline.controlPoints = 4;
    gm20b::setEntryCount(r.pipeline, gm20b::TessBufferCount);
    for (uint8_t k = 0; k < gm20b::TessBufferCount; k++)
        r.pipeline.entry[k] = {uint16_t(firstEntry + 16 * k),
                               gm20b::EntryTessBuffer,
                               1,
                               k,
                               uint8_t(1 | (kControlWord + gm20b::kTessResidualWord[k]) << 2),
                               0,
                               0};
    r.pipeline.pushMask = 0x3fu << (kControlWord - 1);
    r.extras = {{0x0320, 0x202}, {0x0dcc, 1}, {0x2080, 0x21}, {0x2090, 1}, {0x20c0, 0x31}, {0x20d0, 2}};
    r.restore = {{0x2080, 0x20}, {0x20c0, 0x30}};
    return r;
}

// A state of amplification 2 reading its id, and each amplification's viewport offset where `viewports`
gm20b::RuntimeRecord amplifying(uint64_t key, bool viewports = false)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    r.pipeline.amplificationMax = 2;
    r.pipeline.amplificationIdWord = kIdWord;
    r.pipeline.pushMask = 1u << (kIdWord - 1) | (viewports ? 1u << (kViewportWord - 1) : 0);
    r.pipeline.vertexSlot = 0xff;
    r.pipeline.amplificationViewportWord = viewports ? kViewportWord : 0;
    return r;
}

class DeviceIcb : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer slots, first, second, mine;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4 * gm20b::icb::kPatchSlotWords * 4, slots, true));
        ASSERT_TRUE(dev.createBuffer(4096, first));
        ASSERT_TRUE(dev.createBuffer(4096, second));
        ASSERT_TRUE(dev.createBuffer(4096, mine));
    }

    // Patch command `index`: patches [start, start + 2) of factors `factors` (instance stride `stride`)
    gm20b::icb::PatchDraw patches(uint32_t index, const gm20b::Buffer &factors, uint32_t start, uint32_t stride)
    {
        gm20b::icb::PatchDraw d{};
        const char *why = nullptr;
        EXPECT_TRUE(gm20b::RenderEncoder::icbPatchDraw(
            dev, {start, 2, 1, 0, &factors, 32, stride, 1.0f, nullptr, 0, nullptr, 0}, d, &why))
            << (why ? why : "");
        gm20b::icb::patches(reinterpret_cast<uint32_t *>(slots.cpu) + index * gm20b::icb::kPatchSlotWords, index, d);
        return d;
    }

    // One pass: `body` encodes into it; the channel model run over every word the device submitted, its context's first
    template <class Body> Channel pass(gm20b::Pipeline *pl, Body body, uint64_t factorAt = 0)
    {
        Channel c;
        c.factorAt = factorAt;
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return c;
        gm20b::PassDesc desc;
        desc.color[0].texture = &target;
        desc.count = 1;
        gm20b::RenderEncoder *e = cb->render(desc);
        EXPECT_NE(e, nullptr);
        if (!e) {
            cb->abandon();
            return c;
        }
        e->setPipeline(pl);
        body(*e);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> words;
        for (size_t j = 0; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        c.run(words);
        return c;
    }

    gm20b::RenderEncoder::IcbExecute execute(uint32_t at, uint32_t count, uint32_t words, uint32_t primitive)
    {
        return {&slots, at, count, words, nullptr, 0, primitive, false, false, reads, 2};
    }

    const gm20b::Buffer *reads[2] = {&first, &second};
};

void expectDrawn(const Channel::Draw &d, const gm20b::icb::PatchDraw &command)
{
    for (uint32_t w = 0; w < 3; w++)
        EXPECT_EQ(d.factors[w], command.buffers[gm20b::TessFactors][w]) << w;
    for (uint32_t w = 0; w < 6; w++)
        EXPECT_EQ(d.control[w], command.control[w]) << w;
}

TEST_F(DeviceIcb, EachPatchCommandDrawsWithItsOwnFactorsAndControlWords)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(tessellation(0x1cb1), false, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_NE(pl->setSlot[2], 0u);
    const gm20b::icb::PatchDraw a = patches(0, first, 0, 12), b = patches(1, second, 4, 24);
    const Channel c = pass(pl, [&](gm20b::RenderEncoder &e) {
        e.setTessellationFactors(&mine, 6, 13);
        const char *refused = nullptr;
        EXPECT_TRUE(e.executeSlots(execute(0, 2, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused))
            << (refused ? refused : "");
        EXPECT_TRUE(e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, &refused)) << (refused ? refused : "");
        EXPECT_TRUE(e.executeSlots(execute(0, 2, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused))
            << (refused ? refused : "");
    });
    ASSERT_EQ(c.draws.size(), 5u) << c.summary();
    expectDrawn(c.draws[0], a);
    expectDrawn(c.draws[1], b);
    expectDrawn(c.draws[3], a);
    expectDrawn(c.draws[4], b);
    EXPECT_EQ(c.draws[0].set, c.draws[1].set);
    EXPECT_NE(c.draws[2].set, c.draws[0].set) << "the draw after the execute builds a set 0 of its own";
    gm20b::BufferDescriptor own;
    gm20b::bufferDescriptor(true, 0, 0, mine.va + 6, uint32_t(mine.length - 6), own);
    EXPECT_EQ(c.draws[2].factors[0], uint32_t(own.base));
    EXPECT_EQ(c.draws[2].factors[2], own.range);
    EXPECT_EQ(c.draws[2].control[gm20b::kTessResidualWord[gm20b::TessFactors]], own.residual);
    EXPECT_EQ(c.draws[2].control[gm20b::kTessInstanceStrideWord], 12u);
    EXPECT_EQ(c.draws[2].control[gm20b::kTessScaleWord], 0x3f800000u);
}

TEST_F(DeviceIcb, APatchCommandOutsideTheRangeDrawsNothing)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(tessellation(0x1cb2), false, why);
    ASSERT_NE(pl, nullptr) << why;
    patches(0, first, 0, 12);
    const gm20b::icb::PatchDraw b = patches(1, second, 4, 24);
    const Channel c = pass(pl, [&](gm20b::RenderEncoder &e) {
        const char *refused = nullptr;
        gm20b::RenderEncoder::IcbExecute x = execute(1, 1, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive);
        EXPECT_TRUE(e.executeSlots(x, &refused)) << (refused ? refused : "");
    });
    ASSERT_EQ(c.draws.size(), 1u) << c.summary();
    expectDrawn(c.draws[0], b);
}

TEST_F(DeviceIcb, PatchCommandsTakeATessellationStateAlone)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(amplifying(0x1cb3), false, why);
    ASSERT_NE(pl, nullptr) << why;
    patches(0, first, 0, 12);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc desc;
    desc.color[0].texture = &target;
    desc.count = 1;
    gm20b::RenderEncoder *e = cb->render(desc);
    ASSERT_NE(e, nullptr);
    e->setPipeline(pl);
    const char *refused = nullptr;
    EXPECT_FALSE(e->executeSlots(execute(0, 1, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused));
    e->end();
    cb->abandon();
}

// The patch macro selects set 0 at the state's own size: a set of two 256-byte blocks takes the control-stage buffers its
// second block holds
TEST_F(DeviceIcb, PatchCommandsLoadTheirBuffersPastASetZerosFirstBlock)
{
    gm20b::RuntimeRecord r = tessellation(0x1cb6, 0x100);
    r.pipeline.setBytes = 0x100 + 16 * gm20b::TessBufferCount;
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    const gm20b::icb::PatchDraw a = patches(0, first, 0, 12), b = patches(1, second, 4, 24);
    const Channel c = pass(
        pl,
        [&](gm20b::RenderEncoder &e) {
            const char *refused = nullptr;
            EXPECT_TRUE(
                e.executeSlots(execute(0, 2, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused))
                << (refused ? refused : "");
        },
        0x100);
    ASSERT_EQ(c.draws.size(), 2u) << c.summary();
    expectDrawn(c.draws[0], a);
    expectDrawn(c.draws[1], b);
}

// A set past the encoder's 512 bytes (its wide path): the patch macro selects it at its own size (SET_CONSTANT_BUFFER_SELECTOR_A's
// 17-bit size) and loads the control-stage buffers at their bytes past 0x600 (LOAD_CONSTANT_BUFFER_OFFSET's 16 bits)
TEST_F(DeviceIcb, PatchCommandsLoadTheirBuffersInASetPast512Bytes)
{
    gm20b::RuntimeRecord r = tessellation(0x1cb8, 0x600);
    r.pipeline.setBytes = 0x600 + 16 * gm20b::TessBufferCount;
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    const gm20b::icb::PatchDraw a = patches(0, first, 0, 12), b = patches(1, second, 4, 24);
    const Channel c = pass(
        pl,
        [&](gm20b::RenderEncoder &e) {
            const char *refused = nullptr;
            EXPECT_TRUE(
                e.executeSlots(execute(0, 2, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused))
                << (refused ? refused : "");
        },
        0x600);
    ASSERT_EQ(c.draws.size(), 2u) << c.summary();
    expectDrawn(c.draws[0], a);
    expectDrawn(c.draws[1], b);
}

// The stack's control stage reads set 0 through its constant buffer (m2v's synthesized module, three storage buffers NVK
// maps there): a record whose control program reads it by its address is the driver's error, never drawn
TEST_F(DeviceIcb, AControlStageReadingSetZeroByItsAddressIsTheDriversError)
{
    gm20b::RuntimeRecord r = tessellation(0x1cb7);
    r.tessPrograms[0].cbufTypes = {gm20b::CbufRoot};
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_EQ(pl->setSlot[2], 0u);
    patches(0, first, 0, 12);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc desc;
    desc.color[0].texture = &target;
    desc.count = 1;
    gm20b::RenderEncoder *e = cb->render(desc);
    ASSERT_NE(e, nullptr);
    e->setPipeline(pl);
    const char *refused = nullptr;
    EXPECT_FALSE(e->executeSlots(execute(0, 1, gm20b::icb::kPatchSlotWords, gm20b::icb::kPatchPrimitive), &refused));
    EXPECT_EQ(cb->refusal.id, gm20b::RefPatchCommands);
    e->end();
    cb->abandon();
}

TEST_F(DeviceIcb, AnExecuteUnderAmplificationDrawsItsRangeOncePerAmplification)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(amplifying(0x1cb4), false, why);
    ASSERT_NE(pl, nullptr) << why;
    for (uint32_t i = 0; i < 2; i++)
        gm20b::icb::draw(reinterpret_cast<uint32_t *>(slots.cpu) + i * gm20b::icb::kSlotWords, i,
                         {kTriangle, 3, 1, 3 * i, 0});
    const Channel c = pass(pl, [&](gm20b::RenderEncoder &e) {
        const uint32_t none[gm20b::RenderEncoder::kAmplificationMaps] = {};
        e.setAmplification(2, none, none);
        const char *refused = nullptr;
        EXPECT_TRUE(e.executeSlots(execute(0, 2, gm20b::icb::kSlotWords, kTriangle), &refused))
            << (refused ? refused : "");
        EXPECT_TRUE(e.draw(kTriangle, 0, 3, 1, 0, &refused)) << (refused ? refused : "");
    });
    ASSERT_EQ(c.draws.size(), 4u + 2u) << c.summary();
    const uint32_t ids[] = {0, 0, 1, 1, 0, 1};
    for (size_t i = 0; i < c.draws.size(); i++)
        EXPECT_EQ(c.draws[i].amplification, ids[i]) << i;
}

// An execute of a state that fetches the colour it writes through the texture unit: each command a primitive at a time
// behind the fetch barrier, its sizes from the topology it sets (a triangle list of 6, a line strip of 3), one split draw
// a command; the execute leaves the commands' macros whole for the executes after it
TEST_F(DeviceIcb, AFetchingStatesCommandsAreDrawnAPrimitiveAtATime)
{
    gm20b::RuntimeRecord r = amplifying(0x1cb7);
    r.pipeline.amplificationMax = 0;
    r.pipeline.amplificationIdWord = 0;
    r.pipeline.pushMask = 0;
    gm20b::setEntryCount(r.pipeline, 1);
    r.pipeline.entry[0] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    r.pipeline.flags = gm20b::PipelineFetches;
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(gm20b::fetchesOwnColor(pl->fetchBits, pl->writeBits, pl->storageBits));
    auto *words = reinterpret_cast<uint32_t *>(slots.cpu);
    gm20b::icb::draw(words, 0, {kTriangle, 6, 1, 0, 0});
    gm20b::icb::draw(words + gm20b::icb::kSlotWords, 1, {2, 3, 1, 0, 0});
    const uint64_t sequential = dev.stats.fetch.sequential;
    const Channel c = pass(pl, [&](gm20b::RenderEncoder &e) {
        const char *refused = nullptr;
        EXPECT_TRUE(e.executeSlots(execute(0, 2, gm20b::icb::kSlotWords, kTriangle), &refused))
            << (refused ? refused : "");
    });
    using gm20b::mme::kFetchBeginContinued, gm20b::mme::kFetchBeginFirst;
    const uint32_t last = kFetchBeginContinued ^ kFetchBeginFirst, opNone = 0xffff;
    std::vector<uint32_t> split;
    for (const uint32_t v : c.begins)
        split.push_back(v & ~opNone);
    EXPECT_EQ(split, (std::vector<uint32_t>{kFetchBeginFirst, kFetchBeginContinued, last, kFetchBeginFirst,
                                            kFetchBeginContinued, last}))
        << c.summary();
    EXPECT_EQ(c.scratch.words[SWITCH_GM20B_SCRATCH_FETCH_ICB], 0u) << "the executes after it draw whole";
    EXPECT_EQ(dev.stats.fetch.sequential - sequential, 1u) << "one fetching call, as an indirect draw is";
}

// An execute under view mappings that offset the viewport: each pass of its range has its amplification's offset in the
// state's word, as its id, and a draw after it each amplification's through the amplify macro
TEST_F(DeviceIcb, AnExecuteUnderViewportOffsetsDrawsEachAmplificationsOffset)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(amplifying(0x1cb5, true), false, why);
    ASSERT_NE(pl, nullptr) << why;
    gm20b::icb::draw(reinterpret_cast<uint32_t *>(slots.cpu), 0, {kTriangle, 3, 1, 0, 0});
    const Channel c = pass(pl, [&](gm20b::RenderEncoder &e) {
        const uint32_t viewports[gm20b::RenderEncoder::kAmplificationMaps] = {3, 6};
        const uint32_t layers[gm20b::RenderEncoder::kAmplificationMaps] = {};
        e.setAmplification(2, viewports, layers);
        const char *refused = nullptr;
        EXPECT_TRUE(e.executeSlots(execute(0, 1, gm20b::icb::kSlotWords, kTriangle), &refused))
            << (refused ? refused : "");
        EXPECT_TRUE(e.draw(kTriangle, 0, 3, 1, 0, &refused)) << (refused ? refused : "");
    });
    ASSERT_EQ(c.draws.size(), 4u) << c.summary();
    const uint32_t ids[] = {0, 1, 0, 1}, offsets[] = {3, 6, 3, 6};
    for (size_t i = 0; i < c.draws.size(); i++) {
        EXPECT_EQ(c.draws[i].amplification, ids[i]) << i;
        EXPECT_EQ(c.draws[i].viewport, offsets[i]) << i;
    }
}

TEST_F(DeviceIcb, APatchCommandsWordsAreAnEncodersDrawsOfItsBuffers)
{
    gm20b::icb::PatchDraw d{};
    const char *why = nullptr;
    ASSERT_TRUE(
        gm20b::RenderEncoder::icbPatchDraw(dev, {3, 7, 2, 1, &first, 0x21, 30, 1.0f, &second, 8, nullptr, 0}, d, &why));
    gm20b::BufferDescriptor factors, indices;
    gm20b::bufferDescriptor(true, 0, 0, first.va + 0x20, uint32_t(first.length - 0x20), factors);
    gm20b::bufferDescriptor(true, 0, 0, second.va + 8, uint32_t(second.length - 8), indices);
    EXPECT_EQ(d.buffers[gm20b::TessFactors][0], uint32_t(factors.base));
    EXPECT_EQ(d.buffers[gm20b::TessFactors][1], uint32_t(factors.base >> 32));
    EXPECT_EQ(d.buffers[gm20b::TessFactors][2], factors.range);
    EXPECT_EQ(d.buffers[gm20b::TessPatchIndices][2], indices.range);
    EXPECT_EQ(d.control[gm20b::kTessResidualWord[gm20b::TessPatchIndices]], 8u);
    EXPECT_EQ(d.control[gm20b::kTessInstanceStrideWord], 28u);
    EXPECT_EQ(d.control[gm20b::kTessScaleWord], 0x3f800000u);
    EXPECT_EQ(d.control[gm20b::kTessIndexedWord], 1u);
    EXPECT_NE(d.buffers[gm20b::TessControlPointIndices][2], 0u) << "no control-point indices read the zero page";
    EXPECT_EQ(d.count, 7u);
    EXPECT_EQ(d.first, 3u);
    EXPECT_FALSE(
        gm20b::RenderEncoder::icbPatchDraw(dev, {0, 1, 1, 0, &first, 0, 0, 0.0f, nullptr, 0, nullptr, 0}, d, &why));
    ASSERT_TRUE(
        gm20b::RenderEncoder::icbPatchDraw(dev, {0, 1, 1, 0, &first, 0, 0, 1.0f, &second, 2, nullptr, 0}, d, &why));
    EXPECT_EQ(d.control[gm20b::kTessResidualWord[gm20b::TessPatchIndices]], 0u) << "patch indices from 2 read from 0";
}

}  // namespace
