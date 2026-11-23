// SPDX-License-Identifier: MIT
// A kernel's RGB10A8_2P_XR10 texture held in Metal's two planes (xr_pass.h) on gm20b::Device (device_harness.h): the
// record's texture entry binds the color-word plane's own header (R32Uint) and its plane entry the alpha plane's (R8Uint),
// each read or written in place; a plane entry at a texture of one image fails the dispatch, never binding a sampler.
#include "device_harness.h"
#include "../compute.h"
#include "../compute_record.h"
#include "../xr_pass.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"

namespace {

constexpr uint32_t kSide = 64, kTwoPlanes = 550;
constexpr uint32_t kVkR8Uint = 13, kVkR32Uint = 98;

// The header word 0 the direct compile records for the translator's view of `vkFormat` (direct_compute.cpp
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
    gm20b::Texture words, alphas, planes, oneImage;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kSide, kSide, gm20b::kXrWordFormat, words));
        ASSERT_TRUE(dev.createTexture(kSide, kSide, gm20b::kXrAlphaFormat, alphas));
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
    const gm20b::ComputeState *kernel(uint64_t key, bool plane, uint64_t readOnly)
    {
        gm20b::ComputeRecord r;
        r.code.assign(64, 0);
        r.keyString = "xr kernel " + std::to_string(key);
        r.kernel.keyHash = key;
        r.kernel.entries = plane ? 2 : 1;
        const bool storage = readOnly != 3;
        r.kernel.entry[0] = {0, gm20b::KernelTexture, 0, viewWord0(kVkR32Uint, storage)};
        r.kernel.entry[1] = {16, gm20b::KernelTexturePlane, 0, viewWord0(kVkR8Uint, storage)};
        r.kernel.readOnlyEntries = readOnly;
        r.kernel.setBytes = 32;
        r.cbufs = 2;
        r.cbufType[0] = gm20b::CbufRoot;
        r.cbufType[1] = gm20b::CbufSet;
        return gm20b::placeCompute(dev, r);
    }

    // The set-0 words of one dispatch of `k` with `texture` at slot 0, or why it failed
    std::string dispatch(const gm20b::ComputeState *k, const gm20b::Texture *texture, uint32_t out[8])
    {
        gm20b::CommandBuffer *c = dev.begin();
        if (!c)
            return "no command buffer";
        gm20b::ComputeEncoder enc(*c, dev);
        enc.setState(k);
        enc.setTexture(0, texture);
        const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
        const char *why = nullptr;
        const bool ran = enc.dispatch(groups, threads, &why);
        if (const uint32_t *set = gm20b::DeviceHarness::kernelSet(enc); ran && set)
            memcpy(out, set, 8 * sizeof(uint32_t));
        enc.end();
        uint32_t seqno = 0;
        c->commit(seqno);
        return ran ? "" : why ? why : "?";
    }
};

TEST_F(DeviceXrKernel, AStoreBindsEachPlanesOwnHeader)
{
    const gm20b::ComputeState *k = kernel(0x2a501, true, 0);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    ASSERT_EQ(dispatch(k, &planes, set), "");
    EXPECT_EQ(set[0] & 0xfffff, words.tic);
    EXPECT_EQ(set[4] & 0xfffff, alphas.tic);
}

TEST_F(DeviceXrKernel, ALoadBindsEachPlanesOwnHeaderToo)
{
    const gm20b::ComputeState *k = kernel(0x2a502, true, 3);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    ASSERT_EQ(dispatch(k, &planes, set), "");
    EXPECT_EQ(set[0] & 0xfffff, words.tic);
    EXPECT_EQ(set[4] & 0xfffff, alphas.tic);
}

// Over a plane alone the alpha is memoryless and its header the pair's second, over a sparse range (xr_texture.cpp): a
// kernel's store binds that header, where the GMMU drops it as an M4 Max drops the store (m2v-textures9 probes/xr.txt 6a-6c)
TEST_F(DeviceXrKernel, AStoreIntoAPlaneAlonesAlphaBindsItsSink)
{
    const gm20b::ComputeState *k = kernel(0x2a504, true, 0);
    ASSERT_NE(k, nullptr);
    gm20b::Texture sink;
    ASSERT_TRUE(dev.createMemoryless(kSide, kSide, gm20b::kXrAlphaFormat, sink));
    sink.tic = alphas.tic;
    planes.chroma = &sink;
    uint32_t set[8] = {};
    ASSERT_EQ(dispatch(k, &planes, set), "");
    EXPECT_EQ(set[0] & 0xfffff, words.tic);
    EXPECT_EQ(set[4] & 0xfffff, alphas.tic);
}

TEST_F(DeviceXrKernel, APlaneEntryAtATextureOfOneImageFailsTheDispatch)
{
    const gm20b::ComputeState *k = kernel(0x2a503, true, 0);
    ASSERT_NE(k, nullptr);
    uint32_t set[8] = {};
    EXPECT_EQ(dispatch(k, &oneImage, set), "a kernel record's alpha plane at a texture held in one image");
}

}  // namespace
