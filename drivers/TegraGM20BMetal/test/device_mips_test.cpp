// SPDX-License-Identifier: MIT
// generateMipmapsForTexture's 3D-engine steps (mip_passes.cpp) as the methods gm20b::Device submitted: one pass a layer
// and level, each into level l + 1 of its layer, sampling a header of level l of that layer alone.
#include "device_harness.h"
#include "../fx/fx_image.h"
#include "../attachment_target.h"
#include "../record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"

namespace {

constexpr uint32_t kRGBA8 = 70;

class DeviceMips : public harness::DeviceTest {
protected:
    gm20b::Pipeline *record(uint64_t key, uint32_t format = kRGBA8)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=" + std::to_string(format) + " ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
        }
        r.programs[0].id = "vs";
        r.programs[1].id = "fs";
        r.pipeline.keyHash = key;
        gm20b::setEntryCount(r.pipeline, 1);
        r.pipeline.entry[0] = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerNone, 0, 0};
        r.pipeline.setBytes = 32;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        return dev.addRecord(r, false, why);
    }

    static std::vector<std::pair<uint32_t, uint32_t>> submitted(size_t first = 0)
    {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
            const auto &job = kernel_model::model.jobs[j];
            const std::vector<uint32_t> &w = job.words;
            for (size_t i = 0; i < w.size();) {
                const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                               method = (h & 0xfff) << 2;
                if (op == 4) {
                    if (subc == 0)
                        out.emplace_back(method, count);
                    continue;
                }
                if (op != 1 && op != 3 && op != 5)
                    continue;
                for (uint32_t k = 0; k < count && i < w.size(); k++, i++)
                    if (subc == 0)
                        out.emplace_back(method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), w[i]);
            }
        }
        return out;
    }

    static std::vector<uint32_t> targetAddresses(size_t first = 0)
    {
        std::vector<uint32_t> out;
        for (const auto &[method, value] : submitted(first))
            if (method == NVB197_SET_COLOR_TARGET_B(0))
                out.push_back(value);
        return out;
    }
};

TEST_F(DeviceMips, EachLayersLevelIsDrawnFromThatLayersLevelAbove)
{
    gm20b::Pipeline *mip = record(0xb1);
    ASSERT_NE(mip, nullptr);
    gm20b::Texture array;
    ASSERT_TRUE(dev.createArray(32, 32, 3, kRGBA8, gm20b::fx::kTexTwoDArray, array, 4));
    const gm20b::fx::MipChain &c = array.chain->layout;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    ASSERT_TRUE(cb->mipPasses(array, *mip, 1, 3, 0, 3, array.format, &why)) << (why ? why : "");
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    std::vector<uint32_t> want;
    for (uint32_t layer = 0; layer < 3; layer++)
        for (uint32_t l = 1; l < 3; l++)
            want.push_back(uint32_t(array.chain->va + layer * c.arrayStride + c.level[l + 1].offset));
    EXPECT_EQ(targetAddresses(), want);
    for (uint32_t layer = 0; layer < 3; layer++) {
        gm20b::Texture from;
        ASSERT_TRUE(gm20b::attachmentTarget(array, 2, layer, from));
        const auto h = gm20b::DeviceHarness::header(dev, dev.attachmentTic(array, from, 2, layer));
        const uint64_t at = uint64_t(h[1] >> 9 | (h[2] & 0xffff) << 23) << 9;
        EXPECT_EQ(at, array.chain->va + layer * c.arrayStride + c.level[2].offset) << "layer " << layer << "'s level 2";
    }
}

// A one-layer chain from level 0, as gpu.mipmaps's 3D-engine steps were before layers: the same passes
TEST_F(DeviceMips, AOneLayerChainIsEachLevelFromTheOneAbove)
{
    gm20b::Pipeline *mip = record(0xb3);
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMipTexture(32, 32, kRGBA8, 4, t));
    const gm20b::fx::MipChain &c = t.chain->layout;
    gm20b::CommandBuffer *cb = dev.begin();
    const char *why = nullptr;
    ASSERT_TRUE(cb->mipPasses(t, *mip, 0, 3, 0, 1, t.format, &why)) << (why ? why : "");
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    std::vector<uint32_t> want;
    for (uint32_t l = 0; l < 3; l++)
        want.push_back(uint32_t(t.chain->va + c.level[l + 1].offset));
    EXPECT_EQ(targetAddresses(), want);
    EXPECT_EQ(dev.attachmentTic(t, t, 1, 0), dev.levelTic(t, 1)) << "level 1's own header, as before layers";
}

TEST_F(DeviceMips, AVolumesLevelsAreNoLayers)
{
    gm20b::Pipeline *mip = record(0xb2);
    gm20b::Texture volume;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, volume, 3));
    gm20b::CommandBuffer *cb = dev.begin();
    const char *why = nullptr;
    EXPECT_FALSE(cb->mipPasses(volume, *mip, 0, 2, 0, 1, volume.format, &why)) << "a 3D level averages planes in pairs";
    cb->abandon();
}

TEST_F(DeviceMips, AViewRangeUsesItsFormatAndOnlyItsParentLevelsAndLayers)
{
    for (uint32_t format : {70u, 71u}) {
        SCOPED_TRACE(format);
        gm20b::Pipeline *mip = record(0xc0 + format, format);
        ASSERT_NE(mip, nullptr);
        gm20b::Texture parent;
        ASSERT_TRUE(dev.createArray(100, 60, 3, kRGBA8, gm20b::fx::kTexTwoDArray, parent, 6));
        const auto &c = parent.chain->layout;
        const size_t firstJob = kernel_model::model.jobs.size();
        auto *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const char *why = nullptr;
        ASSERT_TRUE(cb->mipPasses(parent, *mip, 1, 3, 1, 2, format, &why)) << (why ? why : "");
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> targets;
        std::vector<uint64_t> sources;
        for (uint32_t layer = 1; layer < 3; ++layer)
            for (uint32_t level = 1; level < 3; ++level) {
                targets.push_back(uint32_t(parent.chain->va + layer * c.arrayStride + c.level[level + 1].offset));
                sources.push_back(parent.chain->va + layer * c.arrayStride + c.level[level].offset);
            }
        EXPECT_EQ(targetAddresses(firstJob), targets);
        uint64_t selector = 0;
        size_t source = 0, target = 0;
        for (const auto &[method, value] : submitted(firstJob)) {
            if (method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_B)
                selector = uint64_t(value) << 32;
            else if (method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_C)
                selector = (selector & ~uint64_t(0xffffffff)) | value;
            else if (method == NVB197_BIND_GROUP_CONSTANT_BUFFER(4) && (value & 1) && (value >> 4 & 0x1f) == 1) {
                const uint8_t *set = gm20b::DeviceHarness::upload(dev, selector, 4);
                ASSERT_NE(set, nullptr);
                uint32_t handle = 0;
                memcpy(&handle, set, sizeof handle);
                const auto h = gm20b::DeviceHarness::header(dev, handle & 0xfffff);
                ASSERT_LT(source, sources.size());
                const uint64_t address = uint64_t(h[1] >> 9 | (h[2] & 0xffff) << 23) << 9;
                EXPECT_EQ(address, sources[source]);
                const uint32_t level = uint32_t(source % 2 + 1);
                EXPECT_EQ(h[0], gm20b::formatInfo(kRGBA8)->tic0);
                EXPECT_EQ(h[4] & (1u << 22), 0u) << "the mip shader decodes sRGB itself";
                EXPECT_EQ(h[4] & 0xffff, c.level[level].width - 1);
                EXPECT_EQ(h[5] & 0xffff, c.level[level].height - 1);
                ++source;
            } else if (method == NVB197_SET_COLOR_TARGET_FORMAT(0) && value) {
                EXPECT_EQ(value, gm20b::formatInfo(format)->rt);
                ++target;
            }
        }
        EXPECT_EQ(source, sources.size());
        EXPECT_EQ(target, targets.size());
    }
}

}  // namespace
