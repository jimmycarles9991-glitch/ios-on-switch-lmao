// SPDX-License-Identifier: MIT
// State segment binds on raw::Device (raw_device_harness.h) in a command buffer of more binds than the old fixed 128:
// every bind of a state with a segment runs its segment while the job's push budget has room for it
// (raw_state_segment.h segmentBindBudget), and none is copied into the push ring.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

class DeviceSegments : public harness::DeviceTest {
protected:
    raw::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    // A state whose register stream is long enough for a segment (raw_state_segment.h kSegmentBindWords)
    raw::Pipeline *state(uint64_t key, uint32_t value)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
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
        raw::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    void fold()
    {
        if (raw::CommandBuffer *cb = dev.begin())
            cb->abandon();
    }
};

TEST_F(DeviceSegments, ACommandBufferWithinItsJobsPushBudgetBindsEveryStateBySegment)
{
    raw::Pipeline *a = state(0xc1, 0x100), *b = state(0xc2, 0x200);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->segment && b->segment);
    fold();
    const uint64_t binds = dev.stats.segmentBinds, copies = dev.stats.segmentCopies;
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassDesc pass;
    pass.color[0].texture = &target;
    pass.count = 1;
    raw::RenderEncoder *e = cb->render(pass);
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

}  // namespace
