// SPDX-License-Identifier: MIT
// Passes into levels, slices, cube faces and 3D planes, layered passes, and viewport and scissor arrays, read back as the
// methods raw::Device submitted (the kernel model keeps each job's push words).
#include "raw_device_harness.h"
#include "../raw/fx/fx_image.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_layers.h"
#include "../raw/runtime/raw_mme.h"
#include "../raw/runtime/raw_record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <map>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangleStrip = 4;

struct Method {
    uint32_t method, value;
};

// The 3D subchannel's methods of `words`, in order (INC, NON_INC, ONE_INC and IMMD headers)
std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (op == 4) {
            if (subc == 0)
                out.push_back({method, count});
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            continue;
        for (uint32_t k = 0; k < count && i < words.size(); k++, i++)
            if (subc == 0)
                out.push_back({method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]});
    }
    return out;
}

// The last value each method took in `m`
std::map<uint32_t, uint32_t> last(const std::vector<Method> &m)
{
    std::map<uint32_t, uint32_t> out;
    for (const Method &x : m)
        out[x.method] = x.value;
    return out;
}

std::vector<uint32_t> valuesOf(const std::vector<Method> &m, uint32_t method)
{
    std::vector<uint32_t> out;
    for (const Method &x : m)
        if (x.method == method)
            out.push_back(x.value);
    return out;
}

// The last word each root table push word took in `m` (LOAD_CONSTANT_BUFFER from its offset, the root left selected)
std::map<uint32_t, uint32_t> rootWords(const std::vector<Method> &m)
{
    std::map<uint32_t, uint32_t> out;
    uint32_t offset = 0;
    for (const Method &x : m) {
        if (x.method == NVB197_LOAD_CONSTANT_BUFFER_OFFSET) {
            offset = x.value;
        } else if (x.method >= NVB197_LOAD_CONSTANT_BUFFER(0) && x.method <= NVB197_LOAD_CONSTANT_BUFFER(15)) {
            out[offset] = x.value;
            offset += 4;
        }
    }
    return out;
}

class DeviceLayers : public harness::DeviceTest {
protected:
    raw::Pipeline *plain = nullptr, *choosing = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        plain = record(0xa1, false);
        choosing = record(0xa2, true);
        ASSERT_NE(plain, nullptr);
        ASSERT_NE(choosing, nullptr);
    }

    // A state of an RGBA8 target whose vertex program writes ViewportIndex (its header's OMAP bit 426) or not, and Layer
    // (bit 425) or not; the push words + 1 its draws write the pass's layer count and the encoder's viewport count into
    raw::Pipeline *record(uint64_t key, bool viewportIndex, bool layer = false, uint8_t arrayLengthWord = 0,
                          uint8_t viewportCountWord = 0)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        // placed programs are shared by id
        r.programs[0].id = std::string("vs") + (viewportIndex ? "-viewport" : "") + (layer ? "-layer" : "");
        r.programs[1].id = "fs";
        if (viewportIndex)
            r.programs[0].image[53] |= 4;
        if (layer)
            r.programs[0].image[53] |= 2;
        r.pipeline.keyHash = key;
        r.pipeline.arrayLengthWord = arrayLengthWord;
        r.pipeline.viewportCountWord = viewportCountWord;
        r.pipeline.pushMask = (arrayLengthWord && arrayLengthWord <= 32 ? 1u << (arrayLengthWord - 1) : 0) |
                              (viewportCountWord ? 1u << (viewportCountWord - 1) : 0);
        r.blob = {0x1a00, 0x1111};
        std::string why;
        return dev.addRecord(r, false, why);
    }

    // The methods of one pass over `pass`, with `body` encoding into it; empty where the pass or a draw was refused
    template <class Body> std::vector<Method> pass(const raw::PassDesc &p, Body body, std::string *why = nullptr)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        raw::RenderEncoder *e = cb->render(p);
        if (!e) {
            cb->abandon();
            if (why)
                *why = "no pass";
            return {};
        }
        const char *refused = nullptr;
        const bool ok = body(*e, &refused);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            if (why)
                *why = refused ? refused : "not committed";
            return {};
        }
        return methods(kernel_model::model.jobs.back().words);
    }

    static raw::PassDesc onto(raw::Texture &t, uint32_t layers = 1)
    {
        raw::PassDesc p;
        p.color[0].texture = &t;
        p.color[0].load = raw::ColorAttachment::Clear;
        p.color[0].clear[0] = 1;
        p.count = 1;
        p.layers = layers;
        return p;
    }

    static bool none(raw::RenderEncoder &, const char **)
    {
        return true;
    }
};

TEST_F(DeviceLayers, ALevelOfAnArraySliceIsTheLevelsImageAtThatLayer)
{
    raw::Texture t, part;
    ASSERT_TRUE(dev.createArray(64, 64, 3, kRGBA8, raw::fx::kTexTwoDArray, t, 4));
    ASSERT_NE(t.chain, nullptr);
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::arrayLayout(64, 64, 3, 4, 4, c));
    EXPECT_EQ(t.chain->layout.bytes, c.bytes);
    ASSERT_TRUE(raw::attachmentTarget(t, 2, 1, part));
    const uint64_t va = t.va + c.level[2].offset + c.arrayStride;
    EXPECT_EQ(part.va, va);
    EXPECT_EQ(part.layers, 2u);
    const auto m = last(pass(onto(part), none));
    ASSERT_FALSE(m.empty());
    const uint32_t ct = NVB197_SET_COLOR_TARGET_A(0);
    EXPECT_EQ(m.at(ct), uint32_t(va >> 32));
    EXPECT_EQ(m.at(ct + 4), uint32_t(va));
    EXPECT_EQ(m.at(ct + 8), c.level[2].rowBytes / 4);  // WIDTH: the level's row stride in texels
    EXPECT_EQ(m.at(ct + 12), 16u);
    EXPECT_EQ(m.at(ct + 20), uint32_t(c.level[2].yLog2) << 4);  // MEMORY: ONE_GOB wide, the level's height, array size
    EXPECT_EQ(m.at(ct + 24), 1u);                               // THIRD_DIMENSION
    EXPECT_EQ(m.at(ct + 28), uint32_t(c.arrayStride >> 2));     // ARRAY_PITCH
    EXPECT_EQ(m.at(ct + 32), 0u);                               // LAYER
}

TEST_F(DeviceLayers, ACubeFaceIsItsLayer)
{
    raw::Texture t, face;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, t));
    ASSERT_TRUE(raw::attachmentTarget(t, 0, 3, face));
    EXPECT_EQ(face.va, t.va + 3 * t.layerBytes);
    const auto m = last(pass(onto(face), none));
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_B(0)), uint32_t(t.va + 3 * t.layerBytes));
}

TEST_F(DeviceLayers, A3DPlaneIsTheLevelsPlaneUnderDepthSize)
{
    raw::Texture t, plane;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, true, t, 2));
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::volumeLayout(16, 16, 8, 2, 4, true, c));
    ASSERT_TRUE(raw::attachmentTarget(t, 1, 3, plane));
    const raw::fx::MipLevel &l = c.level[1];
    EXPECT_EQ(plane.va, t.va + l.offset + 3ull * l.rowBytes * l.rows);
    EXPECT_EQ(plane.layers, l.depth - 3);
    const auto m = last(pass(onto(plane), none));
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 16 & 1, 1u);  // THIRD_DIMENSION_DEFINES_DEPTH_SIZE
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 8 & 15, 0u);  // one plane a block
    raw::Texture deep;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, deep));
    EXPECT_FALSE(raw::attachmentTarget(deep, 0, 1, plane)) << "a plane inside a block of several is no base address";
}

TEST_F(DeviceLayers, PitchMemoryRendersOneLayerOnly)
{
    raw::Texture t, part;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, false));
    ASSERT_FALSE(t.blockLinear);
    EXPECT_FALSE(raw::attachmentTarget(t, 0, 1, part));
    std::string why;
    EXPECT_TRUE(pass(onto(t, 2), none, &why).empty());
    EXPECT_EQ(why, "no pass");
}

TEST_F(DeviceLayers, ALayeredPassBindsAndClearsEveryLayer)
{
    raw::Texture cube, view;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, cube));
    ASSERT_TRUE(raw::attachmentTarget(cube, 0, 0, view));
    raw::PassDesc p = onto(view, 6);
    p.hiddenZeta = true;
    const auto m = pass(p, none);
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 6u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 6u);
    const uint32_t call = NVB197_CALL_MME_MACRO(raw::mme::kClearLayers);
    const auto payloads = valuesOf(m, call), counts = valuesOf(m, call + 4);
    ASSERT_EQ(payloads.size(), 1u) << "one call clears color and the hidden zeta in all six";
    EXPECT_EQ(counts, std::vector<uint32_t>{6});
    EXPECT_EQ(payloads[0] & 3, 3u);
    EXPECT_EQ(payloads[0] >> 10, 0u) << "from layer 0";
    EXPECT_TRUE(valuesOf(m, NVB197_CLEAR_SURFACE).empty());
}

TEST_F(DeviceLayers, ALayeredPassPastItsTargetsLayersIsRefused)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 1, view));
    EXPECT_FALSE(pass(onto(view, 3), none).empty());
    std::string why;
    EXPECT_TRUE(pass(onto(view, 4), none, &why).empty());
}

// A layered pass draws a state whose vertex program writes no Layer into its first layer (SET_RT_LAYER 0), and one that
// writes Layer into the slice it chooses (SET_RT_LAYER's control the shader's), the pass's length in its push word
TEST_F(DeviceLayers, ALayeredPassDrawsEachStateIntoItsLayer)
{
    raw::Pipeline *layering = record(0xa3, false, true, 3);
    ASSERT_NE(layering, nullptr);
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 0, view));
    std::string why;
    const auto m = pass(
        onto(view, 4),
        [&](raw::RenderEncoder &e, const char **refused) {
            e.setPipeline(plain);
            if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
                return false;
            e.setPipeline(layering);
            return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        },
        &why);
    ASSERT_FALSE(m.empty()) << why;
    const uint32_t shader = NVDEF(NVB197, SET_RT_LAYER, CONTROL, GEOMETRY_SHADER_SELECTS_LAYER);
    EXPECT_EQ(valuesOf(m, NVB197_SET_RT_LAYER), (std::vector<uint32_t>{0, shader}));
    EXPECT_EQ(rootWords(m).at(0x700 + 4 * 2), 4u);
}

TEST_F(DeviceLayers, ViewportsAndScissorsPastTheFirstAreWrittenAtTheirIndex)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, true));
    double viewports[raw::kMaxViewports][6];
    uint32_t rects[raw::kMaxViewports][4];
    for (uint32_t j = 0; j < raw::kMaxViewports; j++) {
        const double v[6] = {double(j % 4) * 16, double(j / 4) * 16, 16, 16, 0, 1};
        std::copy_n(v, 6, viewports[j]);
        const uint32_t r[4] = {(j % 4) * 16 + 1, (j / 4) * 16 + 2, 12, 10};
        std::copy_n(r, 4, rects[j]);
    }
    const auto m = pass(onto(t), [&](raw::RenderEncoder &e, const char **refused) {
        e.setPipeline(plain);
        e.setViewports(viewports, raw::kMaxViewports);
        e.setScissors(rects, raw::kMaxViewports);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    for (uint32_t j = 1; j < raw::kMaxViewports; j++) {
        uint32_t w[8];
        raw::viewportWords(viewports[j], w);
        const uint32_t vp = NVB197_SET_VIEWPORT_SCALE_X(j);
        for (uint32_t i = 0; i < 6; i++)
            EXPECT_EQ(at.at(vp + 4 * i), w[i]) << "viewport " << j << " word " << i;
        EXPECT_EQ(at.at(NVB197_SET_VIEWPORT_CLIP_HORIZONTAL(j)), w[6]) << j;
        EXPECT_EQ(at.at(NVB197_SET_VIEWPORT_CLIP_VERTICAL(j)), w[7]) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_ENABLE(j)), 1u) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_HORIZONTAL(j)), (rects[j][0] + 12) << 16 | rects[j][0]) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_VERTICAL(j)), (rects[j][1] + 10) << 16 | rects[j][1]) << j;
    }
}

// A state whose vertex function chooses its slice but whose record is one layer's (m2v had no layered form of it) draws
// in a pass of one layer and is refused in a layered pass
TEST_F(DeviceLayers, AOneLayerRecordOfASliceChoosingStateIsRefusedInALayeredPass)
{
    raw::Pipeline *dropped = record(0xa5, false, false, raw::kLayerDropped);
    ASSERT_NE(dropped, nullptr);
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 0, view));
    const auto drawn = [&](uint32_t layers) {
        std::string why;
        const auto m = pass(
            onto(view, layers),
            [&](raw::RenderEncoder &e, const char **refused) {
                e.setPipeline(dropped);
                return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
            },
            &why);
        return m.empty() ? why : std::string();
    };
    EXPECT_EQ(drawn(1), "");
    EXPECT_NE(drawn(4).find("layered pass"), std::string::npos);
}

// The m2v guard's rule (RawMetalM2vGuard.mm fails such a command buffer rather than replay it): only a one-layer record of
// a slice-choosing state, only in a layered pass
TEST(RawLayers, AOneLayerRecordInALayeredPassHasNoRenderer)
{
    EXPECT_NE(raw::layeredDrawMissing(raw::kLayerDropped, 2, raw::kLayerDropped), nullptr);
    EXPECT_EQ(raw::layeredDrawMissing(raw::kLayerDropped, 1, raw::kLayerDropped), nullptr);
    EXPECT_EQ(raw::layeredDrawMissing(3, 6, raw::kLayerDropped), nullptr);
    EXPECT_EQ(raw::layeredDrawMissing(0, 6, raw::kLayerDropped), nullptr);
}

// A vertex function choosing among the encoder's viewports draws below its count, which its draws write: a record made
// without that word, whose function would draw viewport 0, is refused past one viewport
TEST_F(DeviceLayers, AVertexFunctionChoosesAmongTheEncodersViewports)
{
    raw::Pipeline *counted = record(0xa4, true, false, 0, 5);
    ASSERT_NE(counted, nullptr);
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, true));
    double viewports[2][6] = {{0, 0, 32, 32, 0, 1}, {32, 32, 32, 32, 0, 1}};
    std::vector<Method> drawn;
    const auto draw = [&](raw::Pipeline *pl, uint32_t count) {
        std::string why;
        drawn = pass(
            onto(t),
            [&](raw::RenderEncoder &e, const char **refused) {
                e.setPipeline(pl);
                e.setViewports(viewports, count);
                return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
            },
            &why);
        return drawn.empty() ? why : std::string();
    };
    EXPECT_EQ(draw(choosing, 1), "");
    EXPECT_NE(draw(choosing, 2).find("viewports"), std::string::npos);
    EXPECT_EQ(draw(counted, 2), "");
    EXPECT_EQ(rootWords(drawn).at(0x700 + 4 * 4), 2u);
}

TEST_F(DeviceLayers, MipmappedArraysCubesAndVolumesAreChainsOfTheirLayout)
{
    raw::Texture cubes, volume, level;
    ASSERT_TRUE(dev.createArray(32, 32, 12, kRGBA8, raw::fx::kTexCubeArray, cubes, 6));
    ASSERT_NE(cubes.chain, nullptr);
    EXPECT_EQ(cubes.chain->layout.layers, 12u);
    EXPECT_EQ(cubes.chain->layout.levels, 6u);
    EXPECT_NE(cubes.chain->fmt2d, 0u) << "every layer's levels are the 2D engine's";
    ASSERT_TRUE(raw::chainLevelImage(cubes, 3, level));
    EXPECT_EQ(level.va, cubes.va + cubes.chain->layout.level[3].offset);
    EXPECT_EQ(level.layers, 12u);
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, volume, 3));
    ASSERT_NE(volume.chain, nullptr);
    EXPECT_EQ(volume.chain->fmt2d, 0u) << "the 2D engine halves no depth";
    ASSERT_TRUE(raw::chainLevelImage(volume, 1, level));
    EXPECT_EQ(level.type, raw::fx::kTexThreeD);
    EXPECT_EQ(level.depth, 4u);
    EXPECT_EQ(level.blockDepth, volume.chain->layout.level[1].zLog2);
    raw::Texture flat;
    EXPECT_FALSE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, flat, 7)) << "past 32's full chain";
}

TEST_F(DeviceLayers, BlockCompressedTexturesOfLevelsAndLayersAreNilsImage)
{
    constexpr uint32_t kASTC4x4 = 204, kETC2RGB8 = 180;
    raw::Texture astc, etc;
    ASSERT_TRUE(dev.createBlockTexture(100, 60, 7, 1, kASTC4x4, raw::fx::kTexTwoD, astc));
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::blockChainLayout(100, 60, 7, 1, 4, 4, 16, c));
    ASSERT_NE(astc.chain, nullptr);
    EXPECT_EQ(astc.chain->layout.bytes, c.bytes);
    EXPECT_EQ(astc.width, 25u) << "its blocks";
    ASSERT_TRUE(dev.createBlockTexture(64, 64, 1, 3, kETC2RGB8, raw::fx::kTexTwoDArray, etc));
    ASSERT_TRUE(raw::fx::blockChainLayout(64, 64, 1, 3, 4, 4, 8, c));
    EXPECT_EQ(etc.chain, nullptr);
    EXPECT_EQ(etc.layers, 3u);
    EXPECT_EQ(etc.layerBytes, c.arrayStride);
    EXPECT_NE(etc.tic, 0u);
    raw::Texture none;
    EXPECT_FALSE(dev.createBlockTexture(64, 64, 1, 1, kRGBA8, raw::fx::kTexTwoD, none)) << "not block-compressed";
}

TEST_F(DeviceLayers, ALayerOfADepthArrayIsItsZetaFromThere)
{
    constexpr uint32_t kDepth32Float = 252;
    raw::Texture shape, zeta, cube, face;
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(raw::zetaShape(32, 32, kDepth32Float, 1, shape, bytes, align));
    raw::ZetaMemory m;
    m.va = 0x7000000000;
    m.bytes = shape.layerBytes * 6;
    m.layerBytes = shape.layerBytes;
    m.pitch = shape.pitch;
    m.bpp = shape.bpp;
    m.blockHeight = shape.blockHeight;
    m.tiled = true;
    const char *why = nullptr;
    ASSERT_TRUE(dev.createZetaOver(m, 32, 32, kDepth32Float, true, zeta, &why, 6)) << why;
    EXPECT_EQ(zeta.layers, 6u);
    EXPECT_EQ(zeta.tic, 0u) << "an array's zeta is rendered, its samples m2v's";
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, cube));
    ASSERT_TRUE(raw::attachmentTarget(cube, 0, 2, face));
    raw::Texture part = zeta;
    part.va += 2 * zeta.layerBytes;
    part.layers -= 2;
    raw::PassDesc p = onto(face, 4);
    p.depthStencil = {&part, true, false, 1.0f, 0};
    const auto at = last(pass(p, none));
    ASSERT_FALSE(at.empty());
    EXPECT_EQ(at.at(NVB197_SET_ZT_B), uint32_t(m.va + 2 * zeta.layerBytes));
    EXPECT_EQ(at.at(NVB197_SET_ZT_ARRAY_PITCH), uint32_t(zeta.layerBytes >> 2));
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 4u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_LAYER), 0u);
    p.layers = 5;
    EXPECT_TRUE(pass(p, none).empty()) << "past the zeta's layers";
}

TEST_F(DeviceLayers, MemorylessArraysAreBackedWithThePassesLayers)
{
    constexpr uint32_t kDepth32Float = 252;
    raw::Texture color, depth, shape;
    ASSERT_TRUE(dev.createMemoryless(32, 32, kRGBA8, color, 1, 6));
    ASSERT_TRUE(dev.createMemoryless(32, 32, kDepth32Float, depth, 1, 6));
    EXPECT_FALSE(dev.createMemoryless(32, 32, kRGBA8, shape, 4, 2)) << "no multisampled array";
    EXPECT_EQ(color.layers, 6u);
    EXPECT_TRUE(raw::wholeAttachment(color, 0, 5));
    EXPECT_FALSE(raw::wholeAttachment(color, 0, 6));
    raw::PassDesc p = onto(color, 4);
    p.depthStencil = {&depth, true, false, 1.0f, 0};
    const auto m = pass(p, none);
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 4u);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 12 & 1, 0u) << "block-linear";
    EXPECT_NE(at.at(NVB197_SET_COLOR_TARGET_ARRAY_PITCH(0)), 0u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 4u);
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(raw::zetaShape(32, 32, kDepth32Float, 1, shape, bytes, align));
    EXPECT_EQ(at.at(NVB197_SET_ZT_ARRAY_PITCH), uint32_t(shape.layerBytes >> 2));
    const uint32_t call = NVB197_CALL_MME_MACRO(raw::mme::kClearLayers);
    EXPECT_EQ(valuesOf(m, call + 4), std::vector<uint32_t>{4});
    raw::PassDesc alone = onto(color, 3);
    EXPECT_FALSE(pass(alone, none).empty()) << "a layered pass of memoryless color alone backs it block-linear";
}

// A pass's backing is answered from what the pass before it kept, with no lock: a layered pass of a shape a one-layer
// pass was backed for takes memory of its own layers, and the one-layer pass after it its one-layer backing again
TEST_F(DeviceLayers, ALayeredPassTakesNoOneLayerBackingOfItsShape)
{
    raw::Texture color;
    ASSERT_TRUE(dev.createMemoryless(32, 32, kRGBA8, color, 1, 6));
    const auto address = [](const std::map<uint32_t, uint32_t> &at) {
        return uint64_t(at.at(NVB197_SET_COLOR_TARGET_A(0))) << 32 | at.at(NVB197_SET_COLOR_TARGET_B(0));
    };
    const auto one = last(pass(onto(color, 1), none));
    const auto four = last(pass(onto(color, 4), none));
    const auto again = last(pass(onto(color, 1), none));
    ASSERT_FALSE(one.empty() || four.empty() || again.empty());
    EXPECT_EQ(four.at(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 4u);
    EXPECT_NE(address(four), address(one)) << "a one-layer backing has no second layer";
    EXPECT_EQ(address(again), address(one));
}

// The reverse, for the color and the hidden zeta alike: a one-layer pass of a shape a layered pass was backed for takes
// memory of its own, and the passes after them each their own backing again. At 256 x 256 four layers outgrow the
// one-layer size class; at 32 x 32 they fit in it.
TEST_F(DeviceLayers, AOneLayerPassTakesNoLayeredBackingOfItsShape)
{
    for (uint32_t size : {32u, 256u}) {
        SCOPED_TRACE(size);
        raw::Texture color;
        ASSERT_TRUE(dev.createMemoryless(size, size, kRGBA8, color, 1, 6));
        const auto hidden = [&](uint32_t layers) {
            raw::PassDesc p = onto(color, layers);
            p.hiddenZeta = true;
            return last(pass(p, none));
        };
        const auto four = hidden(4), one = hidden(1), fourAgain = hidden(4), oneAgain = hidden(1);
        ASSERT_FALSE(four.empty() || one.empty() || fourAgain.empty() || oneAgain.empty());
        EXPECT_EQ(four.at(NVB197_SET_ZT_SIZE_C), 4u);
        const auto ownBackings = [&](uint32_t methodA, uint32_t methodB, const char *what) {
            const auto address = [&](const std::map<uint32_t, uint32_t> &at) {
                return uint64_t(at.at(methodA)) << 32 | at.at(methodB);
            };
            EXPECT_NE(address(one), address(four)) << what << ": a one-layer pass's backing is no layered one";
            EXPECT_EQ(address(fourAgain), address(four)) << what;
            EXPECT_EQ(address(oneAgain), address(one)) << what;
        };
        ownBackings(NVB197_SET_COLOR_TARGET_A(0), NVB197_SET_COLOR_TARGET_B(0), "color");
        ownBackings(NVB197_SET_ZT_A, NVB197_SET_ZT_B, "hidden zeta");
    }
}

// viewTic answers from a cache with no lock: a storage view of a multisampled texture spans its sample grid, so it and a
// sampling view of the same word 0 are two headers, each answered again as itself
TEST_F(DeviceLayers, AStorageAndASamplingViewOfAMultisampledTextureAreTwoHeaders)
{
    raw::Texture ms;
    ASSERT_TRUE(dev.createStoredMultisample(64, 64, kRGBA8, 4, ms));
    ASSERT_NE(ms.tic, 0u);
    constexpr uint32_t kWord0 = 0x2a;
    const uint32_t sampled = dev.viewTic(ms, kWord0, raw::ViewAccess::Texture);
    const uint32_t storage = dev.viewTic(ms, kWord0, raw::ViewAccess::Storage);
    ASSERT_NE(sampled, 0u);
    ASSERT_NE(storage, 0u);
    EXPECT_NE(storage, sampled);
    EXPECT_EQ(dev.viewTic(ms, kWord0, raw::ViewAccess::Texture), sampled);
    EXPECT_EQ(dev.viewTic(ms, kWord0, raw::ViewAccess::Storage), storage);
}

// bits [lo, hi] of a 256-bit texture header
uint64_t field(const uint32_t w[8], unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = hi + 1; b-- > lo;)
        v = v << 1 | (w[b / 32] >> (b % 32) & 1);
    return v;
}

// A fetch from a slice or plane part reads a 2D header of that part's memory alone: its base, block height and extent.
// attachmentTic makes one a part, in the part's format, answered again from the view cache, and none for a plane inside
// deeper blocks; a 2D chain's level in its own format is levelTic's
TEST_F(DeviceLayers, AFetchFromAPartReadsAHeaderOfThatPartAlone)
{
    raw::Texture array, volume, chain, slice, plane, level;
    ASSERT_TRUE(dev.createArray(48, 40, 3, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, true, volume));
    ASSERT_TRUE(dev.createMipTexture(64, 64, kRGBA8, 3, chain));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 2, slice));
    ASSERT_TRUE(raw::attachmentTarget(volume, 0, 5, plane));
    ASSERT_TRUE(raw::attachmentTarget(chain, 2, 0, level));
    const raw::FormatInfo *f = raw::formatInfo(kRGBA8);
    for (const raw::Texture *part : {&slice, &plane}) {
        uint32_t w[8];
        ASSERT_TRUE(raw::attachmentHeader(*part, f->tic0, f->srgb, w));
        EXPECT_EQ(field(w, 79, 64) << 32 | field(w, 63, 41) << 9, part->va);
        EXPECT_EQ(field(w, 101, 99), part->blockHeight);
        EXPECT_EQ(field(w, 143, 128) + 1, part->width);
        EXPECT_EQ(field(w, 175, 160) + 1, part->height);
        EXPECT_EQ(field(w, 154, 151), raw::fx::kTexTwoD);
        EXPECT_EQ(field(w, 189, 176), 0u);
        EXPECT_EQ((part->width * part->bpp + 63) / 64 * 64, part->pitch) << "the header's rows are the part's";
    }
    const uint32_t sliceTic = dev.attachmentTic(array, slice, 0, 2), planeTic = dev.attachmentTic(volume, plane, 0, 5);
    ASSERT_NE(sliceTic, 0u);
    ASSERT_NE(planeTic, 0u);
    EXPECT_EQ(dev.attachmentTic(array, slice, 0, 2), sliceTic);
    raw::Texture other;
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 1, other));
    EXPECT_NE(dev.attachmentTic(array, other, 0, 1), sliceTic);
    EXPECT_EQ(dev.attachmentTic(chain, level, 2, 0), dev.levelTic(chain, 2));
    raw::Texture deep, inside;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, deep));
    inside = plane;
    inside.blockDepth = 2;
    EXPECT_EQ(dev.attachmentTic(deep, inside, 0, 4), 0u);
}

}  // namespace
