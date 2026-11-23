// SPDX-License-Identifier: MIT
// CommandBuffer::reach (reach.h) on gm20b::Device over the kernel model. A pass whose descriptor names no size (Metal's
// renderTargetWidth and renderTargetHeight left 0), as the range encode hands on a loaded YCbCr target's pass, reaches over
// its attachments' area.
#include "device_harness.h"
#include "../pack.h"
#include "../reach.h"
#include "../record.h"
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kRGBA8 = 70;

// A pass with draws whose replay draws none: the reach target is cleared and reduced to its tiles
class UndrawnReplay final : public gm20b::DrawReplay {
public:
    bool any() const override
    {
        return true;
    }

    bool atFrameResolution() const override
    {
        return false;
    }

    bool replay(gm20b::RenderEncoder &, const char **) override
    {
        return true;
    }
};

class DeviceReach : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        DeviceTest::SetUp();
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=13 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = gm20b::Pack::fnv(gm20b::kReachReduceState, strlen(gm20b::kReachReduceState));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }
};

TEST_F(DeviceReach, APassNamingNoSizeReachesOverItsAttachment)
{
    gm20b::Texture target;
    ASSERT_TRUE(dev.createTexture(64, 48, kRGBA8, target));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = gm20b::ColorAttachment::Load;
    p.color[0].store = true;
    p.count = 1;
    UndrawnReplay draws;
    gm20b::PassReach reach;
    const char *why = "";
    ASSERT_TRUE(cb->reach(p, p.width, p.height, draws, reach, &why)) << why;
    ASSERT_NE(reach.mask, nullptr);
    EXPECT_FALSE(reach.memory);
    EXPECT_EQ(reach.tilesX, 2u);
    EXPECT_EQ(reach.tilesY, 2u);
    EXPECT_EQ(reach.mask->width, 2u) << "a texel a 32x32 tile";
    EXPECT_EQ(reach.mask->height, 2u);
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}

}  // namespace
