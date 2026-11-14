// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"

namespace {

using kernel_model::model;

constexpr uint32_t kBgra8 = 80, kRgba16Float = 115;

class ChunkMemory : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::RenderServer)) << err;
        importsAtOpen = model.imports.size();
    }

    size_t importsSinceOpen() const
    {
        return model.imports.size() - importsAtOpen;
    }

    size_t importsAtOpen = 0;
};

// A render server's chunks (pool, block-linear) are cached BoAllocs, which the kernel serves from its zeroed spares: no
// import of anonymous pages, which the CPU zero-fills on the creating thread (boot 21: 9 ms per 16 MiB chunk)
TEST_F(ChunkMemory, TargetsOfAScaleFrameImportNoAnonymousPages)
{
    raw::Texture screen, scaled, blur;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, screen, false, true));
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, scaled, false, true));
    ASSERT_TRUE(dev.createTexture(1344, 768, kRgba16Float, blur, false, true));
    raw::Buffer shared;
    ASSERT_TRUE(dev.createBuffer(64 << 10, shared));
    EXPECT_EQ(importsSinceOpen(), 0u);
    ASSERT_NE(scaled.cpu, nullptr);
    scaled.cpu[0] = 1;
    dev.destroyTexture(scaled);
    raw::Texture again;
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, again, false, true));
    EXPECT_EQ(importsSinceOpen(), 0u);
    dev.destroyTexture(again);
    dev.destroyTexture(blur);
    dev.destroyTexture(screen);
}

TEST_F(ChunkMemory, AChunkGivenBackIsFreedNotLeftMapped)
{
    raw::Texture big;
    ASSERT_TRUE(dev.createTexture(2048, 2160, kBgra8, big, false, true));
    const size_t live = model.bos.size();
    dev.destroyTexture(big);
    dev.trim();
    EXPECT_LT(model.bos.size(), live);
}

// A client's chunks stay imports of its own pages: m2v twins a client's raw memory by importing its CPU range
TEST(ClientChunkMemory, AClientsChunkIsAnImportOfItsOwnPages)
{
    kernel_model::model = {};
    raw::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    const size_t before = model.imports.size();
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, t, false, true));
    EXPECT_GT(model.imports.size(), before);
    dev.destroyTexture(t);
}

}  // namespace
