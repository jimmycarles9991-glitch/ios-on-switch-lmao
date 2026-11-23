// SPDX-License-Identifier: MIT
// A resolve into a multisampled texture as the device submits it (resolve_twod.cpp): the one-sample
// resolve written into each of its samples by the 2D engine's point-sampled scale onto its sample grid.
#include "device_harness.h"
#include "../push.h"
#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include <map>
#include <vector>

namespace {

// The 2D subchannel's methods from job `first` on, the last value each took
std::map<uint32_t, uint32_t> twodMethods(size_t first)
{
    std::map<uint32_t, uint32_t> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        const auto method = [&](uint32_t v) {
            if (subc == gm20b::SUBC_2D)
                out[m] = v;
        };
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(w);
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                method(w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
    return out;
}

using DeviceResolveTwod = harness::DeviceTest;

// Each pixel's two samples sit side by side in the grid: the scale covers 2w x h of it at half a source texel a step,
// point sampled, so both read their pixel's texel
TEST_F(DeviceResolveTwod, AResolveIntoAMultisampledTextureFillsEverySample)
{
    gm20b::Texture ms, one;
    ASSERT_TRUE(dev.createStoredMultisample(16, 8, 70, 2, ms));
    ASSERT_TRUE(dev.createTexture(16, 8, 70, one, false, true));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    ASSERT_TRUE(cb->spreadSamples(one, ms, 12, 8));
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    const auto m = twodMethods(first);
    EXPECT_EQ(m.at(NV902D_SET_DST_FORMAT), uint32_t(NV902D_SET_DST_FORMAT_V_A8B8G8R8)) << "RGBA8Unorm's own";
    EXPECT_EQ(m.at(NV902D_SET_DST_WIDTH), 32u) << "the sample grid: two columns a pixel";
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH), 24u) << "the render area's 12 pixels, two samples each";
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT), 8u);
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC), 0x80000000u);
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT), 0u);
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT), 1u) << "one sample row a pixel at 2x";
    EXPECT_EQ(m.at(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE) >> 4 & 1,
              uint32_t(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_POINT));
    EXPECT_EQ(uint64_t(m.at(NV902D_SET_DST_OFFSET_UPPER)) << 32 | m.at(NV902D_SET_DST_OFFSET_LOWER), ms.va);
    EXPECT_EQ(uint64_t(m.at(NV902D_SET_SRC_OFFSET_UPPER)) << 32 | m.at(NV902D_SET_SRC_OFFSET_LOWER), one.va);
}

// A one-sample image of another format, or a multisampled texture of none the engine holds, is not spread
TEST_F(DeviceResolveTwod, ASpreadOfAnotherFormatRecordsNothing)
{
    gm20b::Texture ms, one;
    ASSERT_TRUE(dev.createStoredMultisample(16, 8, 70, 4, ms));
    ASSERT_TRUE(dev.createTexture(16, 8, 80, one, false, true));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    EXPECT_FALSE(cb->spreadSamples(one, ms, 16, 8));
    cb->abandon();
}

}  // namespace
