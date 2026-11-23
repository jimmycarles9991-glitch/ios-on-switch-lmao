// SPDX-License-Identifier: MIT
// Stored depth/stencil textures of levels and layers (Device::createOwnedZeta, texture_shape.cpp zetaShape): NIL's
// 2D array of their texel size in zeta memory, a header naming every level and layer, the arena's footprint the same,
// their views, CPU copies and blits by aspect, and the usages and storages that make one.
#include "device_harness.h"
#include "../fx/fx_image.h"
#include "../fx/fx_mip_probe.h"
#include "../texture_shape.h"
#include "../zeta.h"
#include <map>

namespace {

using gm20b::shapes::TextureShape;

constexpr uint32_t kDepth16 = 250, kDepth32 = 252, kStencil8 = 253, kDepth32Stencil8 = 260;

std::array<uint32_t, 8> chainHeader(const gm20b::fx::MipChain &c, uint64_t va, uint32_t format, uint32_t type)
{
    std::array<uint32_t, 8> w{};
    gm20b::fx::mipChainTic(c, va, gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(format)].tic0, false, 0, c.levels,
                           w.data(), type);
    return w;
}

class DeviceZetaImage : public harness::DeviceTest {
protected:
    // The copy engine's methods of the last job submitted: SET_REMAP_COMPONENTS, the destination's origin and the copy's
    // extent, by method offset
    static std::map<uint32_t, uint32_t> lastCopy()
    {
        std::map<uint32_t, uint32_t> m;
        if (kernel_model::model.jobs.empty())
            return m;
        const std::vector<uint32_t> &w = kernel_model::model.jobs.back().words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4) {
                if (subc == gm20b::SUBC_COPY)
                    m[method] = count;
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); k++, i++)
                if (subc == gm20b::SUBC_COPY)
                    m[method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0)] = w[i];
        }
        return m;
    }

    // The 3D class's SET_ZT_A and SET_ZT_B of the jobs from `first` on: the zeta target's address
    static uint64_t zetaTarget(size_t first)
    {
        constexpr uint32_t kZtA = 0x0fe0;
        uint64_t va = 0;
        const auto &jobs = kernel_model::model.jobs;
        for (size_t j = first; j < jobs.size(); j++) {
            const std::vector<uint32_t> &w = jobs[j].words;
            for (size_t i = 0; i < w.size();) {
                const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                               method = (h & 0xfff) << 2;
                if (op == 4)
                    continue;
                for (uint32_t k = 0; k < count && i < w.size(); k++, i++) {
                    const uint32_t m = method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0);
                    if (subc == gm20b::SUBC_3D && m == kZtA)
                        va = (va & 0xffffffffull) | uint64_t(w[i]) << 32;
                    else if (subc == gm20b::SUBC_3D && m == kZtA + 4)
                        va = (va & ~0xffffffffull) | w[i];
                }
            }
        }
        return va;
    }

    gm20b::TextureTransferResult transfer(gm20b::Texture &t, uint32_t level, uint32_t x, uint32_t y, uint32_t w,
                                          uint32_t h, std::vector<uint8_t> &bytes, uint64_t row, bool write)
    {
        const uint32_t origin[3] = {x, y, 0}, size[3] = {w, h, 1};
        return dev.transferTexture(t, level, 0, origin, size, bytes.data(), row, 0, write, {});
    }
};

constexpr uint32_t kRemap = 0x0708, kDstOrigin = 0x070c + 5 * 4, kLineLength = 0x0418, kLineCount = 0x041c;

TEST_F(DeviceZetaImage, AChainIsNilsImageOfItsTexelSizeUnderAHeaderOfEveryLevel)
{
    for (const uint32_t format : {kDepth16, kDepth32, kStencil8, kDepth32Stencil8}) {
        gm20b::Texture t;
        ASSERT_TRUE(dev.createOwnedZeta(64, 40, format, 1, true, t, 1, gm20b::fx::kTexTwoD, 3)) << format;
        const uint32_t bpp = gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(format)].bpp;
        gm20b::fx::MipChain c;
        ASSERT_TRUE(gm20b::fx::arrayLayout(64, 40, 1, 3, bpp, c));
        ASSERT_NE(t.chain, nullptr) << format;
        EXPECT_EQ(t.chain->layout.levels, 3u);
        EXPECT_EQ(t.chain->layout.bytes, c.bytes);
        EXPECT_EQ(t.chain->layout.level[2].offset, c.level[2].offset);
        EXPECT_EQ(t.chain->va, t.va);
        EXPECT_EQ(t.chain->fmt2d, 0u) << "no 2D engine format: generateMipmaps refuses a depth chain";
        EXPECT_TRUE(t.zeta);
        EXPECT_EQ(t.bpp, bpp);
        EXPECT_EQ(t.pitch, c.level[0].rowBytes);
        EXPECT_EQ(t.blockHeight, c.level[0].yLog2);
        EXPECT_EQ(t.layerBytes, c.arrayStride);
        EXPECT_GE(t.storage.bytes, c.bytes);
        EXPECT_FALSE(t.comptags) << "a chain is never compressed";
        ASSERT_NE(t.tic, 0u);
        EXPECT_EQ(gm20b::DeviceHarness::header(dev, t.tic), chainHeader(c, t.va, format, gm20b::fx::kTexTwoD))
            << format;
        dev.destroyTexture(t);
    }
}

TEST_F(DeviceZetaImage, ACubeArrayOfLevelsIsItsLayersOfLevels)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Stencil8, 1, true, t, 12, gm20b::fx::kTexCubeArray, 2));
    gm20b::fx::MipChain c;
    ASSERT_TRUE(gm20b::fx::arrayLayout(32, 32, 12, 2, 8, c));
    EXPECT_EQ(t.layers, 12u);
    EXPECT_EQ(t.type, gm20b::fx::kTexCubeArray);
    EXPECT_EQ(t.layerBytes, c.arrayStride);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, t.tic),
              chainHeader(c, t.va, kDepth32Stencil8, gm20b::fx::kTexCubeArray));
    dev.destroyTexture(t);
}

// The layers of one level lie as before levels: the zeta's own one-level image, layerBytes apart, the array header's,
// for every format and odd sizes up to the tallest block
TEST_F(DeviceZetaImage, OneLevelOfLayersKeepsTheZetasOwnImage)
{
    for (const uint32_t format : {kDepth16, kDepth32, kStencil8, kDepth32Stencil8})
        for (const auto size : {std::array<uint32_t, 2>{48, 30}, {1, 1}, {13, 7}, {33, 65}, {100, 130}, {257, 19}})
            for (const uint32_t layers : {2u, 6u, 7u}) {
                SCOPED_TRACE(testing::Message() << format << " " << size[0] << "x" << size[1] << " " << layers);
                gm20b::Texture one, t;
                uint64_t bytes = 0, align = 0;
                ASSERT_TRUE(gm20b::zetaShape(size[0], size[1], format, 1, one, bytes, align));
                ASSERT_TRUE(
                    dev.createOwnedZeta(size[0], size[1], format, 1, true, t, layers, gm20b::fx::kTexTwoDArray));
                EXPECT_EQ(t.chain, nullptr);
                EXPECT_EQ(t.layerBytes, one.layerBytes);
                EXPECT_EQ(t.pitch, one.pitch);
                EXPECT_EQ(t.blockHeight, one.blockHeight);
                EXPECT_EQ(t.storage.bytes, (one.layerBytes * layers + align - 1) / align * align);
                uint32_t w[8];
                ASSERT_TRUE(gm20b::zetaArrayHeader(t, gm20b::fx::kTexTwoDArray, w));
                const auto h = gm20b::DeviceHarness::header(dev, t.tic);
                EXPECT_TRUE(std::equal(h.begin(), h.end(), w));
                dev.destroyTexture(t);
            }
}

// A heap's (arena's) depth texture of levels or layers takes the bytes the device's own takes
TEST_F(DeviceZetaImage, TheArenasFootprintIsTheImages)
{
    gm20b::TextureAsk ask;
    ask.kind = gm20b::TextureAsk::Zeta;
    ask.width = 64;
    ask.height = 64;
    ask.format = kDepth32;
    ask.layers = 6;
    ask.levels = 4;
    ask.type = gm20b::fx::kTexCube;
    ask.sampled = true;
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(gm20b::Device::footprint(ask, bytes, align));
    gm20b::fx::MipChain c;
    ASSERT_TRUE(gm20b::fx::arrayLayout(64, 64, 6, 4, 4, c));
    EXPECT_GE(bytes, c.bytes);
    gm20b::TextureAsk one = ask;
    one.layers = one.levels = 1;
    uint64_t oneBytes = 0;
    ASSERT_TRUE(gm20b::Device::footprint(one, oneBytes, align));
    EXPECT_GT(bytes, 6 * oneBytes) << "every face's levels";
}

// A view of the whole of a sampled depth texture of levels or layers names them as its header does
TEST_F(DeviceZetaImage, AWholeViewOfAChainOrArrayKeepsItsHeader)
{
    gm20b::Texture chain, array, view, stencil;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Stencil8, 1, true, chain, 1, gm20b::fx::kTexTwoD, 3));
    ASSERT_TRUE(dev.createView(chain, kDepth32Stencil8, view));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, view.tic), gm20b::DeviceHarness::header(dev, chain.tic));
    ASSERT_TRUE(dev.createView(chain, 261, stencil));
    const auto s = gm20b::DeviceHarness::header(dev, stencil.tic), own = gm20b::DeviceHarness::header(dev, chain.tic);
    EXPECT_NE(s[0], own[0]) << "the stencil view's word 0";
    EXPECT_TRUE(std::equal(s.begin() + 1, s.end(), own.begin() + 1));
    dev.destroyTexture(stencil);
    dev.destroyTexture(view);
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32, 1, true, array, 6, gm20b::fx::kTexCube));
    ASSERT_TRUE(dev.createView(array, kDepth32, view));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, view.tic), gm20b::DeviceHarness::header(dev, array.tic));
    dev.destroyTexture(view);
    dev.destroyTexture(array);
    dev.destroyTexture(chain);
}

// replaceRegion/getBytes of a depth texture: the copy engine through its zeta kind, as Metal's CPU copies move it
TEST_F(DeviceZetaImage, ACpuCopyOfADepthStencilTextureMovesItsDepthWord)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createOwnedZeta(16, 8, kDepth32Stencil8, 1, true, t));
    std::vector<uint8_t> bytes(4 * 2 * 4);
    ASSERT_EQ(transfer(t, 0, 2, 3, 4, 2, bytes, 16, true), gm20b::TextureTransferResult::Complete);
    auto m = lastCopy();
    EXPECT_EQ(m[kRemap], 0x01036660u) << std::hex << m[kRemap];
    EXPECT_EQ(m[kDstOrigin], 2u | 3u << 16);
    EXPECT_EQ(m[kLineLength], 4u);
    ASSERT_EQ(transfer(t, 0, 0, 0, 4, 2, bytes, 16, false), gm20b::TextureTransferResult::Complete);
    EXPECT_EQ(lastCopy()[kRemap], 0x00136660u);
    dev.destroyTexture(t);
}

TEST_F(DeviceZetaImage, ACpuCopyOfAMultisampledDepthTextureCopiesItsSampleCells)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createOwnedZeta(16, 8, kDepth16, 4, t));
    std::vector<uint8_t> bytes(3 * 2 * 4 * 2);
    ASSERT_EQ(transfer(t, 0, 1, 2, 3, 2, bytes, 3 * 4 * 2, true), gm20b::TextureTransferResult::Complete);
    auto m = lastCopy();
    EXPECT_EQ(m[kDstOrigin], 2u | 4u << 16) << "pixel (1, 2)'s cell of the 2X2 grid";
    EXPECT_EQ(m[kLineLength], 6u);
    EXPECT_EQ(m[kLineCount], 4u);
    dev.destroyTexture(t);
}

TEST_F(DeviceZetaImage, ACpuCopyOfADepthChainsLevel)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32, 1, true, t, 1, gm20b::fx::kTexTwoD, 3));
    std::vector<uint8_t> bytes(32 * 32 * 4);
    EXPECT_EQ(transfer(t, 0, 0, 0, 32, 32, bytes, 128, true), gm20b::TextureTransferResult::Complete)
        << "level 0, the texture's own image";
    EXPECT_EQ(lastCopy()[0x070c + 4], t.pitch / 4) << "its row stride in texels";
    EXPECT_EQ(transfer(t, 2, 0, 0, 8, 8, bytes, 32, false), gm20b::TextureTransferResult::Complete);
    EXPECT_EQ(transfer(t, 3, 0, 0, 1, 1, bytes, 4, false), gm20b::TextureTransferResult::Unsupported)
        << "past its chain";
    dev.destroyTexture(t);
}

// A view of a depth chain's level is that level's zeta image (a pass renders into it as its depth attachment) under a
// header of the levels and layers it names, from that level
TEST_F(DeviceZetaImage, AViewOfADepthChainsLevelsAndLayers)
{
    gm20b::Texture chain, level, cubes, cube;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32, 1, true, chain, 1, gm20b::fx::kTexTwoD, 3));
    ASSERT_TRUE(dev.createPartView(chain, kDepth32, gm20b::fx::kTexTwoD, 1, 1, 0, 1, level));
    const gm20b::fx::MipChain &c = chain.chain->layout;
    EXPECT_TRUE(level.zeta);
    EXPECT_EQ(level.va, chain.va + c.level[1].offset);
    EXPECT_EQ(level.width, 16u);
    EXPECT_EQ(level.pitch, c.level[1].rowBytes);
    EXPECT_EQ(level.blockHeight, c.level[1].yLog2);
    std::array<uint32_t, 8> w{};
    gm20b::fx::mipChainTic(c, chain.va, gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth32)].tic0, false, 1, 1,
                           w.data(), gm20b::fx::kTexTwoD);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, level.tic), w);
    ASSERT_TRUE(dev.createOwnedZeta(16, 16, kDepth16, 1, true, cubes, 12, gm20b::fx::kTexCubeArray, 2));
    ASSERT_TRUE(dev.createPartView(cubes, kDepth16, gm20b::fx::kTexCube, 0, 2, 6, 6, cube));
    gm20b::fx::MipChain six = cubes.chain->layout;
    six.layers = 6;
    gm20b::fx::mipChainTic(six, cubes.va + 6 * six.arrayStride,
                           gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth16)].tic0, false, 0, 2, w.data(),
                           gm20b::fx::kTexCube);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, cube.tic), w);
    EXPECT_EQ(cube.va, cubes.va + 6 * six.arrayStride);
    dev.destroyTexture(cube);
    dev.destroyTexture(level);
    dev.destroyTexture(cubes);
    dev.destroyTexture(chain);
}

// A pass into a depth chain's level through a 2D view of it binds that level's image as its zeta target
TEST_F(DeviceZetaImage, APassIntoALevelsViewRendersThatLevel)
{
    gm20b::Texture chain, level;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32, 1, true, chain, 1, gm20b::fx::kTexTwoD, 3));
    ASSERT_TRUE(dev.createPartView(chain, kDepth32, gm20b::fx::kTexTwoD, 1, 1, 0, 1, level));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    gm20b::PassDesc pass;
    pass.depthStencil.texture = &level;
    pass.depthStencil.clearDepth = true;
    gm20b::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(zetaTarget(first), chain.va + chain.chain->layout.level[1].offset);
    dev.destroyTexture(level);
    dev.destroyTexture(chain);
}

// A blit between a buffer and a later level of a Depth32Float_Stencil8 chain moves the aspect Metal names, as level 0's
// does: the level's image is a zeta's (gm20b::chainLevelImage)
TEST_F(DeviceZetaImage, ABlitOfADepthChainsLevelMovesItsAspect)
{
    gm20b::Texture t, level;
    gm20b::Buffer rows;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Stencil8, 1, true, t, 1, gm20b::fx::kTexTwoD, 3));
    ASSERT_TRUE(dev.createBuffer(4096, rows));
    ASSERT_TRUE(gm20b::chainLevelImage(t, 1, level));
    EXPECT_TRUE(level.zeta);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::BlitEncoder *be = cb->blit();
    cb->use(rows, true, false);
    be->copyBufferTexels(rows.va, 16 * 4, level, 0, 0, 0, 16, 16, false, gm20b::kAspectDepth);
    be->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(lastCopy()[kRemap], 0x00136660u) << std::hex << lastCopy()[kRemap];
    dev.destroyTexture(t);
    dev.destroyBuffer(rows);
}

// A shared heap's depth cube of levels lies in the arena's pages under its zeta kind (a view of the pitch arena's BO), NIL's
// chain under a header of every level and face, and the CPU's copy of a later level's face is the copy engine's by aspect
TEST_F(DeviceZetaImage, ASharedHeapsDepthChainIsItsImageInTheArena)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4u << 20, false, arena));
    gm20b::TextureAsk ask;
    ask.kind = gm20b::TextureAsk::Zeta;
    ask.width = ask.height = 32;
    ask.format = kDepth32Stencil8;
    ask.type = gm20b::fx::kTexCube;
    ask.layers = 6;
    ask.levels = 3;
    ask.sampled = true;
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, ask, t));
    ASSERT_NE(t.chain, nullptr);
    gm20b::fx::MipChain c;
    ASSERT_TRUE(gm20b::fx::arrayLayout(32, 32, 6, 3, 8, c));
    EXPECT_EQ(t.chain->layout.bytes, c.bytes);
    EXPECT_EQ(kernel_model::model.resolve({t.va, TegraGM20BSurfaceGpuAddress, 0}),
              gm20b::DeviceHarness::arena(dev, arena).handle);
    EXPECT_EQ(gm20b::DeviceHarness::arenaViews(dev, arena), 1u) << "the zeta kind's";
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, t.tic), chainHeader(c, t.va, kDepth32Stencil8, gm20b::fx::kTexCube));
    std::vector<uint8_t> bytes(16 * 16 * 4);
    const uint32_t origin[3] = {0, 0, 0}, size[3] = {16, 16, 1};
    ASSERT_EQ(dev.transferTexture(t, 1, 4, origin, size, bytes.data(), 64, 0, true, {}),
              gm20b::TextureTransferResult::Complete);
    EXPECT_EQ(lastCopy()[kRemap], 0x01036660u);
    dev.destroyTexture(t);
    dev.destroyArena(arena);
}

TEST(ZetaShapes, EveryUsageAndStoredStorageIsAStoredZeta)
{
    for (const uint32_t storage : {gm20b::shapes::kPrivate, gm20b::shapes::kShared})
        for (const uint64_t usage : {0ull, 1ull, 3ull, 4ull, 5ull})
            for (const uint32_t type : {gm20b::kType2D, gm20b::kType2DArray, gm20b::kTypeCube, gm20b::kTypeCubeArray})
                for (const uint64_t levels : {1ull, 3ull}) {
                    TextureShape s;
                    s.format = kDepth32;
                    s.type = type;
                    s.storage = storage;
                    s.usage = usage;
                    s.levels = levels;
                    s.array = type == gm20b::kType2DArray || type == gm20b::kTypeCubeArray ? 3 : 1;
                    EXPECT_EQ(gm20b::shapes::route(s), gm20b::shapes::Route::StoredZeta)
                        << storage << " " << usage << " " << type << " " << levels;
                }
}

TEST(ZetaShapes, AMemorylessChainOrCubeArrayIsHeldAsAPrivateOne)
{
    TextureShape chain;
    chain.format = kDepth32Stencil8;
    chain.storage = gm20b::shapes::kMemoryless;
    chain.levels = 3;
    chain.usage = gm20b::shapes::kUsageTarget;
    EXPECT_EQ(gm20b::shapes::route(chain), gm20b::shapes::Route::StoredZeta);
    TextureShape cubes = chain;
    cubes.levels = 1;
    cubes.type = gm20b::kTypeCubeArray;
    cubes.array = 2;
    EXPECT_EQ(gm20b::shapes::route(cubes), gm20b::shapes::Route::StoredZeta);
    TextureShape plain = chain;
    plain.levels = 1;
    EXPECT_EQ(gm20b::shapes::route(plain), gm20b::shapes::Route::Memoryless);
}

TEST(ZetaShapes, MultisampledOnesOfEveryUsageAndStoredStorage)
{
    for (const uint32_t storage : {gm20b::shapes::kPrivate, gm20b::shapes::kShared})
        for (const uint64_t usage : {0ull, 1ull, 3ull, 5ull}) {
            TextureShape s;
            s.format = kDepth16;
            s.type = gm20b::kType2DMultisample;
            s.samples = 4;
            s.storage = storage;
            s.usage = usage;
            EXPECT_EQ(gm20b::shapes::storedSamples(s), gm20b::shapes::StoredSamples::Zeta) << storage << " " << usage;
        }
}

// A stored depth/stencil texture has its header from its creation, whatever its usage: recording threads read it unlocked
// (a turn copy, a depth load, a resolve sample one made only to be rendered), so nothing may make it later
class ZetaHeaders : public harness::DeviceTest {};

TEST_F(ZetaHeaders, EveryShapeHasItsHeaderFromCreation)
{
    for (const uint64_t usage : {0ull, 1ull, 3ull, 4ull, 5ull, 6ull}) {
        TextureShape s;
        s.format = kDepth32;
        s.width = 16;
        s.height = 8;
        s.storage = gm20b::shapes::kPrivate;
        s.usage = usage;
        gm20b::Texture t;
        ASSERT_TRUE(gm20b::shapes::create(dev, s, gm20b::shapes::route(s), true, 0, t)) << usage;
        EXPECT_NE(t.tic, 0u) << usage;
        dev.destroyTexture(t);
    }
}

TEST(ZetaShapes, NoColourFormatOrTwoDEngineFormatIsADepthOnes)
{
    for (const uint32_t format : {kDepth16, kDepth32, kStencil8, kDepth32Stencil8}) {
        EXPECT_EQ(gm20b::formatInfo(format), nullptr) << format;
        EXPECT_EQ(gm20b::fx::twodChainFormat(format), 0u) << format;
    }
}

}  // namespace
