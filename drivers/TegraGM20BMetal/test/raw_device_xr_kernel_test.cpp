// SPDX-License-Identifier: MIT
// A kernel's RGB10A8_2P_XR10 texture held in Metal's two planes (raw_xr_pass.h) on raw::Device (raw_device_harness.h): the
// record's texture entry binds the color-word plane's own header (R32Uint) and its plane entry the alpha plane's (R8Uint),
// each read or written in place; a plane entry at a texture of one image fails the dispatch, never binding a sampler.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_xr_pass.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"

namespace {

constexpr uint32_t kSide = 64, kTwoPlanes = 550;
constexpr uint32_t kVkR8Uint = 13, kVkR32Uint = 98;

// The header word 0 the direct compile records for the translator's view of `vkFormat` (raw_direct_compute.cpp
// Entries::texture: viewWord0 of the view's format and its components, the texture's default swizzle composed in)
uint32_t viewWord0(uint32_t vkFormat, bool storage)
{
    const uint32_t rgba[4] = {3, 4, 5, 6};
    uint32_t word0 = 0;
    EXPECT_TRUE(switch_gm20b_view_word0(vkFormat, rgba, storage, &word0));
    return word0;
}

class DeviceXrKernel : public harness::DeviceTest {
protected:
    raw::Texture words, alphas, planes, oneImage;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kSide, kSide, raw::kXrWordFormat, words));
        ASSERT_TRUE(dev.createTexture(kSide, kSide, raw::kXrAlphaFormat, alphas));
        ASSERT_TRUE(dev.createTexture(kSide, kSide, 70, oneImage));
        planes = words;
        planes.format = kTwoPlanes;
        planes.tic = oneImage.tic;  // the sampled header pair's first, which no kernel entry binds
        planes.xrPlanes = true;
        planes.luma = &words;
        planes.chroma = &alphas;
    }

    // A placed kernel whose set 0 holds slot 0's texture entry, and its plane entry where `plane`; `readOnly`: bit i entry
    // i is only read (3: a sampled slot's views, else storage views)
    const raw::ComputeState *kernel(uint64_t key, bool plane, uint64_t readOnly)
    {
        raw::ComputeRecord r;
        r.code.assign(64, 0);
        r.keyString = "xr kernel " + std::to_string(key);
        r.kernel.keyHash = key;
        r.kernel.entries = plane ? 2 : 1;
        const bool storage = readOnly != 3;
        r.kernel.entry[0] = {0, raw::KernelTexture, 0, viewWord0(kVkR32Uint, storage)};
        r.kernel.entry[1] = {16, raw::KernelTexturePlane, 0, viewWord0(kVkR8Uint, storage)};
        r.kernel.readOnlyEntries = readOnly;
        r.kernel.setBytes = 32;
        r.cbufs = 2;
        r.cbufType[0] = raw::CbufRoot;
        r.cbufType[1] = raw::CbufSet;
        return raw::placeCompute(dev, r);
    }

    // The set-0 words of one dispatch of `k` with `texture` at slot 0, or why it failed
    std::string dispatch(const raw::ComputeState *k, const raw::Texture *texture, uint32_t out[8])
    {
        raw::CommandBuffer *c = dev.begin();
        if (!c)
            return "no command buffer";
        raw::ComputeEncoder enc(*c, dev);
        enc.setState(k);
        enc.setTexture(0, texture);
        const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
        const char *why = nullptr;
        const bool ran = enc.dispatch(groups, threads, &why);
        if (const uint32_t *set = raw::DeviceHarness::kernelSet(enc); ran && set)
            memcpy(out, set, 8 * sizeof(uint32_t));
        enc.end();
        uint32_t seqno = 0;
        c->commit(seqno);
        return ran ? "" : why ? why : "?";
    }
};

TEST_F(DeviceXrKernel, AStoreBindsEachPlanesOwnHeader)
{
    const raw::ComputeState *k = kernel(0x2a501, true, 0);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    ASSERT_EQ(dispatch(k, &planes, set), "");
    EXPECT_EQ(set[0] & 0xfffff, words.tic);
    EXPECT_EQ(set[4] & 0xfffff, alphas.tic);
}

TEST_F(DeviceXrKernel, ALoadBindsEachPlanesOwnHeaderToo)
{
    const raw::ComputeState *k = kernel(0x2a502, true, 3);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    ASSERT_EQ(dispatch(k, &planes, set), "");
    EXPECT_EQ(set[0] & 0xfffff, words.tic);
    EXPECT_EQ(set[4] & 0xfffff, alphas.tic);
}

TEST_F(DeviceXrKernel, APlaneEntryAtATextureOfOneImageFailsTheDispatch)
{
    const raw::ComputeState *k = kernel(0x2a503, true, 0);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    EXPECT_EQ(dispatch(k, &oneImage, set), "a kernel record's alpha plane at a texture held in one image");
}

}  // namespace
