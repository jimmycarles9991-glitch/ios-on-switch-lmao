// SPDX-License-Identifier: MIT
// A state that reads argument buffers on raw::Device (raw_device_harness.h): its push words name the device's root table and
// ExecutionStatus words at its creation, its set 1 holds every texture whose ID was named where its class fits, and its
// draws are submitted.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_record.h"

#include <set>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kSampled2d = 0x5, kSamplerClass = 0x2;

bool holdsEvery2d(uint32_t heapClass, const raw::HeapTextureFacts *facts)
{
    return heapClass == kSampled2d && facts->textureType == 2;
}

class DeviceArguments : public harness::DeviceTest {
protected:
    raw::Texture target, sampled;
    raw::Pipeline *pl = nullptr;

    void SetUp() override
    {
        dev.heapHolds(holdsEvery2d);
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createTexture(4, 4, kRGBA8, sampled));
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        r.programs[1].cbufTypes = {raw::CbufRoot, raw::kCbufHeapSet, raw::CbufSet};
        r.pipeline.keyHash = 0xab;
        r.pipeline.entries = 2;
        r.pipeline.entry[0] = {0, raw::EntryTextureTable, 2, 0, 0, 0, 0};
        r.pipeline.entry[1] = {16, raw::EntrySamplerTable, 2, 0, 0, 0, 0};
        r.pipeline.setBytes = 32;
        r.pipeline.rootTableWord[1] = 17;
        r.pipeline.statusWord[1] = 19;
        r.pipeline.pushMask = 0xF << 16;
        r.pipeline.heapArrays = 2;
        r.pipeline.heap[0] = {kSampled2d, 0};
        r.pipeline.heap[1] = {kSamplerClass, 0x8000};
        r.pipeline.heapBytes = 0x10000;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    uint32_t element(uint32_t array, uint32_t i) const
    {
        uint32_t word;
        memcpy(&word, pl->heapSet->memory.cpu + pl->heapSet->array[array].offset + 4 * i, 4);
        return word;
    }

    const raw::HeapTables &tables() const
    {
        return raw::DeviceHarness::tables(dev);
    }

    // A state whose fragment root table holds [[buffer(1)]] (m2v's Psb slot), its other words as SetUp's
    raw::Pipeline *slotState()
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 slot ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        r.pipeline.keyHash = 0xac;
        r.pipeline.rootTableWord[1] = 17;
        r.pipeline.statusWord[1] = 19;
        r.pipeline.rootSlots[1] = 1u << 1;
        r.pipeline.pushMask = 0xF << 16;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // `explicitly`: as the facade's command buffers (RawMetalEncode.mm), whose jobs name what the CPU wrote
    std::string draw(bool explicitly = false, raw::Pipeline *state = nullptr, const raw::Buffer *slot1 = nullptr,
                     uint64_t offset = 0)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        if (explicitly)
            cb->internalCompletion();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(state ? state : pl);
        if (slot1)
            e->setBuffer(2, 1, slot1, offset);
        const char *why = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceArguments, AStatesAddressWordsAreTheDevicesRootTableAndStatus)
{
    ASSERT_NE(pl->heapSet, nullptr);
    EXPECT_FALSE(pl->heapMissing);
    EXPECT_EQ(pl->heapSlot[0], 0);
    EXPECT_EQ(pl->heapSlot[1], 1);
    const uint64_t root = uint64_t(pl->push[17]) << 32 | pl->push[16];
    const uint64_t status = uint64_t(pl->push[19]) << 32 | pl->push[18];
    EXPECT_NE(root, 0u);
    EXPECT_EQ(status, root + 3 * sizeof(raw::RootEntry) + raw::kRootSinkBytes);
    EXPECT_EQ(pl->pushMask & 0xF0000u, 0xF0000u);
    EXPECT_EQ(dev.vaOwner(root).rfind("the argument buffers' root table", 0), 0u) << dev.vaOwner(root);
}

TEST_F(DeviceArguments, ANamedTextureIsInTheStatesSetAndItsDrawsAreSubmitted)
{
    raw::HeapTexture t;
    t.sampleTic = sampled.tic;
    t.facts.textureType = 2;
    ASSERT_TRUE(dev.heapTexture(sampled.tic, &t));
    EXPECT_EQ(element(0, sampled.tic), sampled.tic);
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler({}, s));
    const raw::HeapSampler sampler = {s.tsc, 0};
    ASSERT_TRUE(dev.heapSampler(s.tsc, &sampler));
    EXPECT_EQ(element(1, s.tsc), s.tsc << 20);
    EXPECT_EQ(draw(), "");
    ASSERT_TRUE(dev.heapTexture(sampled.tic, nullptr));
    EXPECT_EQ(element(0, sampled.tic), 0u);
    EXPECT_EQ(draw(), "");
}

// The tables and the state's set 1 are members of the device's resource set: no job names them as a use, and the job after
// their CPU writes says so by its flag, whatever its visibility, so the kernel invalidates the L2 before it
TEST_F(DeviceArguments, AJobAfterTheTablesChangedIsHostWrittenAndTheNextIsNot)
{
    raw::HeapTexture t;
    t.sampleTic = sampled.tic;
    t.facts.textureType = 2;
    ASSERT_TRUE(dev.heapTexture(sampled.tic, &t));
    for (bool explicitly : {false, true}) {
        ASSERT_TRUE(dev.heapTexture(sampled.tic, explicitly ? &t : nullptr));
        ASSERT_EQ(draw(explicitly), "");
        const kernel_model::Job &changed = kernel_model::model.jobs.back();
        EXPECT_TRUE(changed.flags & TegraGM20BJobHostWritten) << explicitly;
        EXPECT_NE(kernel_model::model.bos.at(raw::DeviceHarness::handleTables(dev).handle).set, 0u);
        ASSERT_EQ(draw(explicitly), "");
        EXPECT_FALSE(kernel_model::model.jobs.back().flags & TegraGM20BJobHostWritten) << explicitly;
    }
}

TEST_F(DeviceArguments, ATexturesIdSamplesItAsASlotBindsIt)
{
    raw::Texture standIn = sampled, planar = sampled;
    standIn.tic = 77;
    raw::Texture held = sampled;
    held.standIn = &standIn;
    planar.addressMap = 1;
    const raw::HeapTextureFacts facts = {70, 2, 1, 0, 1, 1, {2, 3, 4, 5}};
    EXPECT_EQ(raw::Device::heapTextureOf(sampled, facts, 9).sampleTic, sampled.tic);
    EXPECT_EQ(raw::Device::heapTextureOf(held, facts, 9).sampleTic, 77u);
    EXPECT_EQ(raw::Device::heapTextureOf(planar, facts, 9).sampleTic, 0u);
    EXPECT_EQ(raw::Device::heapTextureOf(sampled, facts, 9).flags, 9u);
    EXPECT_EQ(raw::Device::heapTextureOf(sampled, facts, 9).facts.usage,
              raw::kTextureUsageEvery);  // MTLTextureUsageUnknown
}

TEST_F(DeviceArguments, ANamedTextureIsInTheTablesUntilItIsForgotten)
{
    const raw::HeapTextureFacts facts = {70, 2, 1, 1, 1, 1, {2, 3, 4, 5}};
    ASSERT_TRUE(dev.nameTexture(sampled, facts, 0x250000));
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).element, sampled.tic);
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).flags, 0x250000u);
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).idLo, sampled.tic);
    EXPECT_EQ(element(0, sampled.tic), sampled.tic);
    dev.forgetTexture(sampled);
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).element, 0u);
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).flags, 0u);
    EXPECT_EQ(tables().textureEntryAt(sampled.tic).idLo, sampled.tic);  // ids never change: the table stays sorted
    EXPECT_EQ(element(0, sampled.tic), 0u);
}

TEST_F(DeviceArguments, APixelCoordinateSamplerSamplesThroughANormalizedTwinUntilItIsForgotten)
{
    raw::SamplerDesc desc;
    desc.pixel = true;
    raw::Sampler s, twin;
    ASSERT_TRUE(dev.createSampler(desc, s));
    ASSERT_TRUE(dev.nameSampler(s, raw::kSamplerWordPixelCoordinates, twin));
    ASSERT_NE(twin.tsc, 0u);
    EXPECT_NE(twin.tsc, s.tsc);
    EXPECT_FALSE(twin.desc.pixel);
    EXPECT_EQ(element(1, s.tsc), twin.tsc << 20);
    EXPECT_EQ(tables().samplerEntryAt(s.tsc).element, s.tsc);
    EXPECT_EQ(tables().samplerEntryAt(s.tsc).flags, raw::kSamplerWordPixelCoordinates);
    const std::set<uint32_t> twinHeaders = {twin.tsc, twin.compareTsc};
    dev.forgetSampler(s, twin);
    EXPECT_EQ(twin.tsc, 0u);
    EXPECT_EQ(element(1, s.tsc), 0u);
    EXPECT_EQ(tables().samplerEntryAt(s.tsc).element, 0u);
    raw::Sampler next;
    ASSERT_TRUE(dev.createSampler({}, next));
    EXPECT_EQ((std::set<uint32_t>{next.tsc, next.compareTsc}), twinHeaders);  // the twin's headers were given back
}

TEST_F(DeviceArguments, ANormalizedSamplerSamplesThroughItsOwnHeader)
{
    raw::Sampler s, twin;
    ASSERT_TRUE(dev.createSampler({}, s));
    ASSERT_TRUE(dev.nameSampler(s, 0x40, twin));
    EXPECT_EQ(twin.tsc, 0u);
    EXPECT_EQ(element(1, s.tsc), s.tsc << 20);
}

// The draw's own fragment root table (RenderEncoder::rootTable) in the upload ring, read where its push words point: the
// writer's entries over the bound buffer, and the buffer named to the job as written
TEST_F(DeviceArguments, ASlotStatesDrawWritesItsRootTableOverTheBoundBuffer)
{
    raw::Pipeline *state = slotState();
    ASSERT_NE(state, nullptr);
    raw::Buffer buffer;
    ASSERT_TRUE(dev.createBuffer(100, buffer));
    ASSERT_EQ(draw(true, state, &buffer, 48), "");
    const uint64_t va = uint64_t(raw::DeviceHarness::rootPush(dev, 17)) << 32 | raw::DeviceHarness::rootPush(dev, 16);
    ASSERT_NE(va, 0u);
    EXPECT_NE(va, tables().rootTableVa());
    const auto *table = reinterpret_cast<const raw::RootEntry *>(raw::DeviceHarness::upload(dev, va, 4 * 32));
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table[0].host | table[0].guest | table[0].size, 0u);
    EXPECT_EQ(table[1].host, tables().sinkVa());
    EXPECT_EQ(va + 32 * table[1].guest, tables().identityVa());
    EXPECT_EQ(table[2].host | table[2].size, 0u);
    EXPECT_EQ(table[3].host, buffer.va);
    EXPECT_EQ(table[3].guest, buffer.va);
    EXPECT_EQ(table[3].size, (buffer.length + 15) & ~uint64_t(15));
    EXPECT_EQ(table[3].bindOffset, 48u);
    const uint64_t bo = kernel_model::model.resolve({buffer.va, TegraGM20BSurfaceGpuAddress, 0});
    ASSERT_NE(bo, 0u);
    bool written = false;
    for (const auto &use : kernel_model::model.jobs.back().uses)
        written |= use.handle == bo && (use.access & TegraGM20BSurfaceWrite);
    EXPECT_TRUE(written);
}

// A kernel that reads argument buffers (PackKernel's argument tail): its root-table and status words the device's own at its
// placement, its set 1 the device's for its arrays, and its dispatches submitted with the tables' descriptors
TEST_F(DeviceArguments, AnArgumentKernelTakesTheDevicesTablesRootsAndSet1)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.keyString = "argument kernel";
    r.kernel.keyHash = 0xcd;
    r.kernel.entries = 2;
    r.kernel.entry[0] = {0, raw::KernelTextureTable, 0, 0};
    r.kernel.entry[1] = {16, raw::KernelSamplerTable, 0, 0};
    r.kernel.setBytes = 32;
    r.kernel.rootTableWord = 9;
    r.kernel.statusWord = 11;
    r.kernel.heapArrays = 2;
    r.kernel.heap[0] = {kSampled2d, 0};
    r.kernel.heap[1] = {kSamplerClass, 0x8000};
    r.kernel.heapBytes = 0x10000;
    r.cbufs = 3;
    r.cbufType[0] = raw::CbufRoot;
    r.cbufType[1] = raw::kCbufHeapSet;
    r.cbufType[2] = raw::CbufSet;
    const raw::ComputeState *k = raw::placeCompute(dev, r);
    ASSERT_NE(k, nullptr);
    EXPECT_FALSE(k->heapMissing);
    EXPECT_EQ(k->heapSet, pl->heapSet);  // the state's arrays: one set for both
    EXPECT_EQ(uint64_t(k->argumentPush[9]) << 32 | k->argumentPush[8], tables().rootTableVa());
    EXPECT_EQ(uint64_t(k->argumentPush[11]) << 32 | k->argumentPush[10], tables().statusVa());
    EXPECT_EQ(k->argumentMask, 0xFu << 8);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->internalCompletion();
    {
        raw::ComputeEncoder e(*cb, dev);
        e.setState(k);
        const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
        const char *why = nullptr;
        EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
        e.end();
    }
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}

}  // namespace
