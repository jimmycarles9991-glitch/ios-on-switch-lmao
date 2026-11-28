// SPDX-License-Identifier: MIT
// A probe draw whose words write registers no state's bind sets (HW Test gpu.gm20b.mixed_merge_probe,
// gpu.gm20b.constant_colour) leaves none of them behind (gm20b::Device over the kernel model): after the draw the push
// sets each back to the golden context's value (probe_restores.h), and the next draw writes the raster masks it wants.
#include "device_forms.h"
#include "../client_errors.h"
#include "../probe_restores.h"
#include "../record.h"

namespace {

constexpr uint32_t kSampleMask = 0x0fa4, kColorTargetMask = 0x0fa8, kRasterMask = 0x0fbc;

class DeviceProbeMasks : public DevicePassForms {
protected:
    gm20b::Texture target;
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DevicePassForms::SetUp();
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, target));
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = "vs-masks";
        r.programs[1].id = "fs-masks";
        r.pipeline.keyHash = 0x6b06;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    void arm(std::initializer_list<std::pair<uint32_t, uint32_t>> before)
    {
        auto words = std::make_shared<gm20b::Device::ProbeWords>();
        for (const auto &[method, value] : before) {
            words->before.push_back(gm20b::Push::hdr(1, gm20b::SUBC_3D, method, 1));
            words->before.push_back(value);
            words->restores |= gm20b::probeRestoreBits(method);
        }
        {
            std::lock_guard<gm20b::Mutex> hold(dev.probeLock);
            dev.probeWords = std::move(words);
        }
        dev.probeArmed.store(true, std::memory_order_release);
    }
};

TEST_F(DeviceProbeMasks, AProbeDrawLeavesTheContextsSampleMasksBehindIt)
{
    gm20b::PassDesc desc;
    desc.color[0] = kept(target, gm20b::ColorAttachment::Clear);
    desc.count = 1;
    const auto ms = pass(desc, [&](gm20b::RenderEncoder &e, const char **why) {
        e.setPipeline(pl);
        arm({{kSampleMask, 0x11}, {kColorTargetMask, 0x1}, {kRasterMask, 0x1111}});
        const bool probed = e.draw(3, 0, 3, 1, 0, why);
        dev.probeArmed.store(false, std::memory_order_release);
        return probed && e.draw(3, 0, 3, 1, 0, why);
    });
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(dev.probeEmitted.load(), 1u);
    EXPECT_EQ(values(ms, kSampleMask), (std::vector<uint32_t>{0x11, 0x1}));
    EXPECT_EQ(values(ms, kColorTargetMask), (std::vector<uint32_t>{0x1, 0xffff}));
    const std::vector<uint32_t> raster = values(ms, kRasterMask);
    ASSERT_GE(raster.size(), 2u);
    EXPECT_EQ(raster.back(), gm20b::kSampleMaskAll) << "the next draw writes the raster masks it wants";
    EXPECT_EQ(lastOf(ms, kRasterMask + 12), gm20b::kSampleMaskAll);
}

TEST_F(DeviceProbeMasks, AConstantColourProbeDrawTurnsThePixelStageBackOn)
{
    constexpr uint32_t kConstantColour = 0x0f40, kPixelStage = 0x2140;
    gm20b::PassDesc desc;
    desc.color[0] = kept(target, gm20b::ColorAttachment::Clear);
    desc.count = 1;
    const auto ms = pass(desc, [&](gm20b::RenderEncoder &e, const char **why) {
        e.setPipeline(pl);
        arm({{kPixelStage, 0x50}, {kConstantColour, 1}});
        const bool probed = e.draw(3, 0, 3, 1, 0, why);
        dev.probeArmed.store(false, std::memory_order_release);
        return probed && e.draw(3, 0, 3, 1, 0, why);
    });
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(values(ms, kConstantColour), (std::vector<uint32_t>{1, 0}));
    EXPECT_EQ(lastOf(ms, kPixelStage), 0x51u);
}

}  // namespace
