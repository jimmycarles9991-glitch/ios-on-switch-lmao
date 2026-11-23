// SPDX-License-Identifier: MIT
// State segment binds on gm20b::Device (device_harness.h) in a command buffer of more binds than the old fixed 128:
// every bind of a state with a segment runs its segment while the job's push budget has room for it
// (state_segment.h segmentBindBudget), and none is copied into the push ring.
#include "device_harness.h"
#include "../record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

class DeviceSegments : public harness::DeviceTest {
protected:
    gm20b::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    // A state whose register stream is long enough for a segment (state_segment.h kSegmentBindWords)
    gm20b::Pipeline *state(uint64_t key, uint32_t value)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = "vs" + std::to_string(key);
        r.programs[1].id = "fs" + std::to_string(key);
        r.pipeline.keyHash = key;
        for (uint32_t m = 0; m < 8; ++m) {  // NVB197_SET_CT_WRITE(m)
            r.blob.push_back(0x1a00 + 4 * m);
            r.blob.push_back((value + m) & 0x1111);
        }
        std::string why;
        gm20b::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    void fold()
    {
        if (gm20b::CommandBuffer *cb = dev.begin())
            cb->abandon();
    }
};

TEST_F(DeviceSegments, ACommandBufferWithinItsJobsPushBudgetBindsEveryStateBySegment)
{
    gm20b::Pipeline *a = state(0xc1, 0x100), *b = state(0xc2, 0x200);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->segment && b->segment);
    fold();
    const uint64_t binds = dev.stats.segmentBinds, copies = dev.stats.segmentCopies;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc pass;
    pass.color[0].texture = &target;
    pass.count = 1;
    gm20b::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    constexpr int kBinds = 300;
    for (int i = 0; i < kBinds; ++i) {
        e->setPipeline(i % 2 ? b : a);
        const char *why = nullptr;
        ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "?");
    }
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    fold();
    EXPECT_EQ(dev.stats.segmentCopies - copies, 0u);
    EXPECT_GE(dev.stats.segmentBinds - binds, uint64_t(kBinds));
}

// A pass of more push words than the ring holds, as gpu.path.lock_between_jobs' 36,000 draws: the ring is replaced by a
// larger one mid-pass (Device::outgrow), and every push the pass's jobs name lies where its words were written, those of
// the rings it grew out of included (the device: PBENTRY, the PBDMA fetching words that were no pushes at the ring's
// addresses its pass end had worked out against the ring that replaced it)
TEST_F(DeviceSegments, APassThatOutgrowsThePushRingNamesEachPushWhereItWasWritten)
{
    gm20b::Pipeline *a = state(0xc1, 0x100);
    ASSERT_NE(a, nullptr);
    fold();
    const size_t jobs = kernel_model::model.jobs.size();
    const uint64_t outgrown = dev.stats.ringsOutgrown.load();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc pass;
    pass.color[0].texture = &target;
    pass.count = 1;
    gm20b::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    e->setPipeline(a);
    for (int i = 0; i < 40000; ++i) {
        const char *why = nullptr;
        ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "?");
    }
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    ASSERT_GT(dev.stats.ringsOutgrown.load(), outgrown) << "the pass outgrew the ring";
    size_t pushes = 0, unbound = 0;
    for (size_t j = jobs; j < kernel_model::model.jobs.size(); ++j)
        for (const TegraGM20BAbiPush &p : kernel_model::model.jobs[j].pushes) {
            const uint8_t *first = kernel_model::model.host(p.va),
                          *last = kernel_model::model.host(p.va + uint64_t(p.dwords) * 4 - 1);
            unbound += !first || last != first + uint64_t(p.dwords) * 4 - 1;
            pushes++;
        }
    EXPECT_GT(pushes, 0u);
    EXPECT_EQ(unbound, 0u) << "of " << pushes << " pushes lie in no memory bound whole";
}

}  // namespace
