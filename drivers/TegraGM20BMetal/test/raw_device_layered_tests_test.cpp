// SPDX-License-Identifier: MIT
// A slice-choosing state whose fragment program runs the depth test itself (raw_shader_tests.h), drawn in a layered pass:
// the depth stand-in holds every layer of the pass's zeta, copied in layer by layer, and the state's set 0 names it as a
// 2D array of the pass's layers, which its program addresses at each primitive's layer. A state that chooses no slice
// takes the stand-in's first layer as before.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_fragment_tests.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_shader_tests.h"
#include "../raw/runtime/raw_zeta.h"
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kDepth32Float = 252, kTriangle = 3, kLayers = 3;
// R32_FLOAT's texture header word 0 (raw_view.h texelBytes: 4 bytes), as the record's depthTic names the view
constexpr uint32_t kR32FloatWord0 = 0x0f;
constexpr uint32_t kR32UintWord0 = 0x6014920f;  // colour 0's storage view: R32_UINT, a 4-byte texel as RGBA8's

class DeviceLayeredTests : public harness::DeviceTest {
protected:
    raw::Texture array, target, zeta;
    uint64_t zetaLayerBytes = 0;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createArray(32, 32, kLayers, kRGBA8, raw::fx::kTexTwoDArray, array));
        ASSERT_TRUE(raw::attachmentTarget(array, 0, 0, target));
        raw::Texture shape;
        uint64_t bytes = 0, align = 0;
        ASSERT_TRUE(raw::zetaShape(32, 32, kDepth32Float, 1, shape, bytes, align));
        raw::ZetaMemory m;
        m.va = 0x7000000000;
        m.bytes = shape.layerBytes * kLayers;
        m.layerBytes = shape.layerBytes;
        m.pitch = shape.pitch;
        m.bpp = shape.bpp;
        m.blockHeight = shape.blockHeight;
        m.tiled = true;
        const char *why = nullptr;
        ASSERT_TRUE(dev.createZetaOver(m, 32, 32, kDepth32Float, true, zeta, &why, kLayers)) << why;
        zetaLayerBytes = shape.layerBytes;
    }

    // A state of colour 0's storage form testing depth in its program: set 0's entry 0 the colour, entry 1 the depth
    // stand-in; layered: its vertex program chooses its slice (writes Layer, the pass's layer count at push word 1)
    raw::Pipeline *state(uint64_t key, bool layered)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=252/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = layered ? "vs-layer" : "vs";
        r.programs[1].id = "fs";
        if (layered)
            r.programs[0].image[53] |= 2;
        r.programs[1].cbufTypes = {raw::CbufRoot, raw::CbufSet};
        raw::PackPipeline &pp = r.pipeline;
        pp.keyHash = key;
        pp.vertexSlot = 0xff;
        raw::setEntryCount(pp, 2);
        pp.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
        pp.entry[1] = {16, raw::EntryDepthTest, 2, 0, raw::SamplerNone, 0, 0};
        pp.setBytes = 32;
        pp.flags = raw::PipelineFetches | raw::PipelineStorageMetadata | raw::PipelinePushMetadata;
        pp.storageMask = pp.storageWriteMask = 1;
        pp.storageSamples = 1;
        pp.storageTests = uint8_t(raw::FragmentTests::DepthWrite);
        pp.storageTic[0] = kR32UintWord0;
        pp.shaderTests.flags = raw::kShaderTestDepth;
        pp.shaderTests.depthWord = 2;
        pp.shaderTests.depthTic = kR32FloatWord0;
        pp.pushMask = 0xfu << 1;
        if (layered) {
            pp.arrayLengthWord = 1;
            pp.pushMask |= 1;
        }
        r.blob = {0x1224, 1, 0x1a00, 0};
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    struct Drawn {
        std::string why;
        uint32_t standInHeader[8] = {};
        std::vector<uint64_t> zetaReads;  // the copy engine's source addresses inside the zeta
    };

    Drawn draw(raw::Pipeline *pl, uint32_t layers = kLayers)
    {
        Drawn out;
        const size_t first = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].image = &array;
        pass.color[0].load = raw::ColorAttachment::Load;
        pass.count = 1;
        pass.layers = layers;
        pass.storageMask = pl->storageBits;
        pass.arrayedStorageMask = layers > 1 && raw::layeredRecord(pl->rec->arrayLengthWord) ? pl->storageBits : 0;
        pass.depthStencil = {&zeta, false, false, 1.0f, 0};
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            out.why = "no pass";
            return out;
        }
        e->setPipeline(pl);
        raw::DepthStencilDesc less;
        less.depthCompare = 1;
        less.depthWrite = true;
        const raw::DepthStencilState tested(less);
        e->setDepthStencil(&tested);
        const char *why = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        uint32_t handle = 0;
        if (ok)
            if (const uint8_t *set = raw::DeviceHarness::upload(dev, raw::DeviceHarness::preparedSet(*e), 32))
                memcpy(&handle, set + 16, 4);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            out.why = why ? why : "not committed";
            return out;
        }
        const auto header = raw::DeviceHarness::header(dev, handle & 0xfffff);
        memcpy(out.standInHeader, header.data(), sizeof out.standInHeader);
        for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
            const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
            for (size_t i = 0; i < w.size();) {
                const uint32_t h = w[i++], op = h >> 29, count = h >> 16 & 0x1fff, subc = h >> 13 & 7,
                               m = (h & 0xfff) << 2;
                if (op == 4 || !h)
                    continue;
                if (op == 7)
                    break;
                // NVB0B5_OFFSET_IN_UPPER, then LOWER
                if (subc == raw::SUBC_COPY && m == 0x400 && count >= 2 && i + 1 < w.size()) {
                    const uint64_t va = uint64_t(w[i]) << 32 | w[i + 1];
                    if (va >= zeta.va && va < zeta.va + kLayers * zetaLayerBytes)
                        out.zetaReads.push_back(va);
                }
                i += op == 3 || op == 1 || op == 5 ? count : 0;
            }
        }
        return out;
    }
};

uint32_t textureType(const uint32_t header[8])
{
    return header[4] >> 23 & 15;
}

uint32_t depthMinusOne(const uint32_t header[8])
{
    return header[5] >> 16 & 0x3fff;
}

TEST_F(DeviceLayeredTests, ASliceChoosingStatesDepthStandInIsAnArrayOfThePassLayers)
{
    const Drawn d = draw(state(0x1a7e1, true));
    ASSERT_EQ(d.why, "");
    EXPECT_EQ(textureType(d.standInHeader), uint32_t(raw::fx::kTexTwoDArray));
    EXPECT_EQ(depthMinusOne(d.standInHeader), kLayers - 1);
    std::vector<uint64_t> layers;
    for (uint32_t l = 0; l < kLayers; l++)
        layers.push_back(zeta.va + l * zetaLayerBytes);
    EXPECT_EQ(d.zetaReads, layers) << "each layer copied into the stand-in";
}

TEST_F(DeviceLayeredTests, AStateThatChoosesNoSliceTakesTheStandInsFirstLayer)
{
    const Drawn d = draw(state(0x1a7e2, false));
    ASSERT_EQ(d.why, "");
    EXPECT_EQ(textureType(d.standInHeader), uint32_t(raw::fx::kTexTwoD));
}

// A one-layer pass of the same size before leaves no one-layer stand-in for the layered pass to take (backings match on
// their layers)
TEST_F(DeviceLayeredTests, AOneLayerPassBeforeLendsTheLayeredPassNoStandIn)
{
    const Drawn flat = draw(state(0x1a7e3, false), 1);
    ASSERT_EQ(flat.why, "");
    EXPECT_EQ(textureType(flat.standInHeader), uint32_t(raw::fx::kTexTwoD));
    const Drawn d = draw(state(0x1a7e4, true));
    ASSERT_EQ(d.why, "");
    EXPECT_EQ(textureType(d.standInHeader), uint32_t(raw::fx::kTexTwoDArray));
    EXPECT_EQ(depthMinusOne(d.standInHeader), kLayers - 1);
    EXPECT_EQ(d.zetaReads.size(), size_t(kLayers));
}

}  // namespace
