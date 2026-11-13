// SPDX-License-Identifier: MIT
// The headers a draw's framebuffer fetch binds on raw::Device (raw_attachment_view.h, RenderEncoder::fetchTic), read back
// from the device's header table: a layered pass's attachment through a slice-choosing state's layered storage form is a 2D
// array of the pass's layers from the attachment's slice, the one-layer form's a 2D view of that slice, the
// input-attachment form's an array of the pass's layers; the layered form in a pass of one layer is an array of one, a
// pitch target's over the block-linear backing the pass renders it through.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kStrip = 4;
constexpr uint32_t kR32UintWord0 = 0x6014920f;  // the storage view's word 0: R32_UINT, a 4-byte texel as RGBA8's
constexpr uint32_t kTicCount = 8192;

// A state whose fragment program fetches colour 0: through its storage form (a view of word 0 kR32UintWord0) or the
// input-attachment form; `layered` the layered form of a slice-choosing state (its vertex program writes Layer, the pass's
// length into push word 2)
raw::RuntimeRecord fetching(uint64_t key, bool storage, bool layered)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = layered ? "vs-layer" : "vs";
    r.programs[1].id = "fs";
    if (layered)
        r.programs[0].image[53] |= 2;
    raw::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.entries = 1;
    pp.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags = raw::PipelineFetches;
    if (layered) {
        pp.arrayLengthWord = 3;
        pp.pushMask = 1u << 2;
    }
    if (storage) {
        pp.flags |= raw::PipelineStorageMetadata;
        pp.storageMask = pp.storageWriteMask = 1;
        pp.storageSamples = 1;
        pp.storageTic[0] = kR32UintWord0;
        r.blob = {0x1224, 1, 0x1a00, 0};
    } else {
        r.blob = {0x1a00, 0x1111};
    }
    return r;
}

uint32_t field(const uint32_t *h, unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = lo; b <= hi; b++)
        v |= uint64_t(h[b / 32] >> (b % 32) & 1) << (b - lo);
    return uint32_t(v);
}

struct Header {
    uint64_t va = 0;
    uint32_t type = 0, width = 0, layers = 0, minView = 0;
};

class DeviceLayeredFetch : public harness::DeviceTest {
protected:
    raw::Pipeline *pipeline(bool storage, bool layered)
    {
        std::string why;
        raw::Pipeline *p =
            dev.addRecord(fetching(0x1f00 | uint64_t(storage) << 1 | uint64_t(layered), storage, layered), false, why);
        EXPECT_NE(p, nullptr) << why;
        return p;
    }

    // One draw of `pl` into attachment `a` in a pass of `layers` layers: empty, or why it was refused. `declared`: the pass
    // names the colors the layered record binds as arrays (RawMetalEncode.mm passArrayedStorageMask)
    std::string draw(raw::Pipeline *pl, const raw::ColorAttachment &a, uint32_t layers, bool declared = true)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        raw::PassDesc pass;
        pass.color[0] = a;
        pass.color[0].load = raw::ColorAttachment::Load;
        pass.count = 1;
        pass.layers = layers;
        pass.storageMask = pl->storageBits;
        pass.arrayedStorageMask = declared && raw::layeredRecord(pl->rec->arrayLengthWord) ? pl->storageBits : 0;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        const char *why = nullptr;
        const bool ok = e->draw(kStrip, 0, 4, 1, 0, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return {};
    }

    // The headers of word 0 `word0` in the device's table
    std::vector<Header> headers(uint32_t word0)
    {
        std::vector<Header> out;
        for (uint32_t tic = 1; tic < kTicCount; tic++) {
            const uint32_t *h = dev.headerWords(tic);
            if (h[0] != word0)
                continue;
            Header x;
            x.va = uint64_t(field(h, 63, 41)) << 9 | uint64_t(field(h, 79, 64)) << 32;
            x.type = field(h, 154, 151);
            x.width = field(h, 143, 128) + 1;
            x.layers = field(h, 189, 176) + 1;
            x.minView = field(h, 227, 224);
            out.push_back(x);
        }
        return out;
    }

    static raw::ColorAttachment part(raw::Texture &image, raw::Texture &target, uint32_t level, uint32_t slice)
    {
        raw::ColorAttachment a;
        EXPECT_TRUE(raw::attachmentTarget(image, level, slice, target));
        a.texture = &target;
        a.image = &image;
        a.level = level;
        a.slice = slice;
        return a;
    }
};

TEST_F(DeviceLayeredFetch, TheLayeredStorageFormIsAnArrayOfThePassesLayersFromTheSlice)
{
    raw::Texture array, target;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexTwoDArray, array));
    EXPECT_EQ(draw(pipeline(true, true), part(array, target, 0, 1), 4), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].va, array.va + array.layerBytes);
    EXPECT_EQ(made[0].type, raw::fx::kTexTwoDArray);
    EXPECT_EQ(made[0].layers, 4u);
    EXPECT_EQ(made[0].width, 32u);
}

TEST_F(DeviceLayeredFetch, AMipmappedArraysLevelIsItsChainAtTheSlice)
{
    raw::Texture array, target;
    ASSERT_TRUE(dev.createArray(64, 64, 6, kRGBA8, raw::fx::kTexTwoDArray, array, 4));
    ASSERT_NE(array.chain, nullptr);
    EXPECT_EQ(draw(pipeline(true, true), part(array, target, 2, 3), 2), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].va, array.chain->va + 3 * array.chain->layout.arrayStride);
    EXPECT_EQ(made[0].minView, 2u);
    EXPECT_EQ(made[0].width >> 2, target.width);
    EXPECT_EQ(made[0].layers, 2u);
}

TEST_F(DeviceLayeredFetch, TheOneLayerFormInALayeredPassReadsTheFirstLayer)
{
    raw::Texture array, target;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexTwoDArray, array));
    EXPECT_EQ(draw(pipeline(true, false), part(array, target, 0, 2), 3), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].va, array.va + 2 * array.layerBytes);
    EXPECT_EQ(made[0].type, raw::fx::kTexTwoD);
    EXPECT_EQ(made[0].layers, 1u);
}

TEST_F(DeviceLayeredFetch, TheInputAttachmentFormInALayeredPassIsAnArray)
{
    raw::Texture array, target;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexTwoDArray, array));
    EXPECT_EQ(draw(pipeline(false, true), part(array, target, 0, 1), 5), "");
    std::vector<Header> arrays;
    for (const Header &h : headers(raw::formatInfo(kRGBA8)->tic0))
        if (h.type == raw::fx::kTexTwoDArray && h.va == array.va + array.layerBytes)
            arrays.push_back(h);
    ASSERT_EQ(arrays.size(), 1u);
    EXPECT_EQ(arrays[0].layers, 5u);
}

TEST_F(DeviceLayeredFetch, TheLayeredFormInAPassOfOneLayerIsAnArrayOfOne)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, true));
    ASSERT_TRUE(t.blockLinear);
    raw::ColorAttachment a;
    a.texture = &t;
    EXPECT_EQ(draw(pipeline(true, true), a, 1), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].va, t.va);
    EXPECT_EQ(made[0].type, raw::fx::kTexTwoDArray);
    EXPECT_EQ(made[0].layers, 1u);
}

TEST_F(DeviceLayeredFetch, APitchTargetOfTheLayeredFormRendersThroughABlockLinearBacking)
{
    raw::Texture pitch;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, pitch));
    ASSERT_FALSE(pitch.blockLinear);
    raw::ColorAttachment linear;
    linear.texture = &pitch;
    raw::Pipeline *layered = pipeline(true, true);
    EXPECT_EQ(draw(layered, linear, 1, false), "framebuffer fetch from a target the raw path cannot sample")
        << "a pitch header has no array form";
    EXPECT_EQ(draw(layered, linear, 1), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_NE(made[0].va, pitch.va);
    EXPECT_EQ(made[0].type, raw::fx::kTexTwoDArray);
    EXPECT_EQ(made[0].layers, 1u);
    EXPECT_EQ(made[0].width, 64u);
}

TEST_F(DeviceLayeredFetch, AMemorylessArraysBackingIsAnArrayOfThePassesLayers)
{
    raw::Texture color;
    ASSERT_TRUE(dev.createMemoryless(32, 32, kRGBA8, color, 1, 6));
    raw::ColorAttachment a;
    a.texture = &color;
    EXPECT_EQ(draw(pipeline(true, true), a, 4), "");
    const auto made = headers(kR32UintWord0);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].type, raw::fx::kTexTwoDArray);
    EXPECT_EQ(made[0].layers, 4u);
    EXPECT_EQ(made[0].width, 32u);
}

}  // namespace
