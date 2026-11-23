// SPDX-License-Identifier: MIT
// Buffer slots bound to nothing read the zero page (RenderEncoder::writeSetEntry, ComputeEncoder::prepareDescriptors).
// The zero page is a use of every job of the device (a member of its set, or one of the set's `always` BOs outside it),
// so no draw or dispatch names it: sixteen draws or dispatches that write a set of unbound slots again grow the command
// buffer's member uses no more than one does, and the job still names the zero page.
#include "device_harness.h"
#include "../compute.h"
#include "../compute_record.h"
#include "../record.h"

#include <algorithm>
#include <string>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kSlots = 8;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));

// A state of vertex storage buffers 0 to kSlots - 1, one at each 16 bytes of set 0
gm20b::RuntimeRecord drawReader(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 unbound " + std::to_string(key);
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.flags = gm20b::PipelinePushMetadata;
    pp.vertexSlot = 0xff;
    pp.setBytes = 16 * kSlots;
    for (gm20b::PackUniformRange &u : pp.uniform)
        u.entry = gm20b::kNoUniform;
    gm20b::setEntryCount(pp, kSlots);
    for (uint32_t k = 0; k < kSlots; k++)
        pp.entry[k] = {uint16_t(16 * k), gm20b::EntryUbo, 1, uint8_t(k), 1, 0, 0};
    pp.memoryEntries = ~0u;
    r.blob = {0x1a00, 0x1111};
    return r;
}

// A kernel of buffers 0 to kSlots - 1 at no residual word, one at each 16 bytes of set 0
const gm20b::ComputeRecord &dispatchReader()
{
    static const gm20b::ComputeRecord reader = [] {
        gm20b::ComputeRecord r;
        r.code.assign(64, 0);
        r.cbufs = 1;
        r.cbufType[0] = gm20b::CbufRoot;
        r.kernel.regionWord = -1;
        r.kernel.qmd[kQmdCbufValid] |= 1;
        r.kernel.setBytes = 16 * kSlots;
        r.kernel.entries = kSlots;
        for (uint32_t k = 0; k < kSlots; k++)
            r.kernel.entry[k] = {uint16_t(16 * k), gm20b::KernelBuffer, uint8_t(k), 0};
        return r;
    }();
    return reader;
}

class DeviceZeroPage : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer buffer;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
    }

    // Whether a job submitted from `first` on names the zero page, by its set or as a use
    bool aJobNamesTheZeroPage(size_t first) const
    {
        const uint64_t zero = gm20b::DeviceHarness::zeroPage(dev).handle;
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j)
            for (const TegraGM20BAbiSurfaceUse &u : kernel_model::model.jobs[j].uses)
                if (u.handle == zero)
                    return true;
        return false;
    }

    // The member uses `draws` draws after two add to a command buffer: each draw switches between two states, so it
    // writes its set whole again, buffer 0 bound and the rest bound to nothing
    size_t drawnUses(gm20b::Pipeline *a, gm20b::Pipeline *b, uint32_t draws)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        EXPECT_NE(e, nullptr);
        if (!e)
            return 0;
        e->setBuffer(1, 0, &buffer, 0);
        size_t first = 0;
        for (uint32_t i = 0; i < 2 + draws; i++) {
            if (i == 2)
                first = gm20b::DeviceHarness::memberUses(*cb);
            e->setPipeline(i % 2 ? b : a);
            const char *why = nullptr;
            EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        }
        const size_t grown = gm20b::DeviceHarness::memberUses(*cb) - first;
        e->end();
        const size_t before = kernel_model::model.jobs.size();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        EXPECT_TRUE(aJobNamesTheZeroPage(before));
        return grown;
    }

    // The same of `dispatches` dispatches after one, each after buffer 0 moved, so it writes its set whole again
    size_t dispatchedUses(const gm20b::ComputeState *k, uint32_t dispatches)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return 0;
        size_t first = 0;
        {
            gm20b::ComputeEncoder e(*cb, dev);
            e.setState(k);
            for (uint32_t i = 0; i < 1 + dispatches; i++) {
                if (i == 1)
                    first = gm20b::DeviceHarness::memberUses(*cb);
                e.setBuffer(0, &buffer, 256 * (i % 2));
                const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
                const char *why = nullptr;
                EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
            }
            e.end();
        }
        const size_t grown = gm20b::DeviceHarness::memberUses(*cb) - first;
        const size_t before = kernel_model::model.jobs.size();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        EXPECT_TRUE(aJobNamesTheZeroPage(before));
        return grown;
    }
};

TEST_F(DeviceZeroPage, DrawsOfSlotsBoundToNothingNameNoUseEach)
{
    std::string why;
    gm20b::Pipeline *a = dev.addRecord(drawReader(0x2e71), false, why);
    ASSERT_NE(a, nullptr) << why;
    gm20b::Pipeline *b = dev.addRecord(drawReader(0x2e72), false, why);
    ASSERT_NE(b, nullptr) << why;
    EXPECT_EQ(drawnUses(a, b, 16), drawnUses(a, b, 1));
}

TEST_F(DeviceZeroPage, DispatchesOfSlotsBoundToNothingNameNoUseEach)
{
    const gm20b::ComputeState *k = gm20b::placeCompute(dev, dispatchReader());
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(dispatchedUses(k, 16), dispatchedUses(k, 1));
}

}  // namespace
