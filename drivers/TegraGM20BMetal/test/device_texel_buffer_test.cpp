// SPDX-License-Identifier: MIT
// gm20b::Device::createTexelBuffer (HW Test gpu.gm20b.texture_buffer, gpu.x1.linear_align's texture buffer at offset 64): a
// MTLTextureTypeTextureBuffer over a buffer is a texel buffer header of its own over the buffer's bytes, as NIL fills
// one for NVK's buffer views, from 16-byte offsets (NVK's minTexelBufferOffsetAlignment).
#include "device_harness.h"
#include "../fx/fx_mip.h"

namespace {

constexpr uint32_t kRGBA8 = 70;

class DeviceTexelBuffer : public harness::DeviceTest {};

TEST_F(DeviceTexelBuffer, ATextureBufferIsATexelBufferHeaderOverItsBytes)
{
    gm20b::Buffer buffer;
    ASSERT_TRUE(dev.createBuffer(64 + 128 * 4, buffer));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexelBuffer(buffer, 64, 128, kRGBA8, t));
    EXPECT_EQ(t.va, buffer.va + 64);
    EXPECT_EQ(t.type, gm20b::fx::kTexOneDBuffer);
    const std::array<uint32_t, 8> got = gm20b::DeviceHarness::header(dev, t.tic);
    EXPECT_EQ(got[0], gm20b::formatInfo(kRGBA8)->tic0) << "word 0: the format's, as every header";
    EXPECT_EQ(got[1], uint32_t(t.va)) << "ADDRESS_BITS31TO0";
    EXPECT_EQ(got[2], uint32_t(t.va >> 32) & 0xffff) << "ADDRESS_BITS47TO32, HEADER_VERSION SELECT_ONE_D_BUFFER (0)";
    EXPECT_EQ(got[3] & 0xffff, 0u) << "WIDTH_MINUS_ONE_BITS31TO16";
    EXPECT_EQ(got[4] & 0xffff, 127u) << "WIDTH_MINUS_ONE_BITS15TO0";
    EXPECT_EQ(got[4] >> 23 & 0xf, 6u) << "TEXTURE_TYPE ONE_D_BUFFER";
    EXPECT_EQ(got[5] | got[6] | got[7], 0u);
}

TEST_F(DeviceTexelBuffer, AnOffsetOffSixteenBytesOrPastTheBufferIsRefused)
{
    gm20b::Buffer buffer;
    ASSERT_TRUE(dev.createBuffer(1024, buffer));
    gm20b::Texture t;
    EXPECT_TRUE(dev.createTexelBuffer(buffer, 16, 8, kRGBA8, t)) << "16 bytes: NVK's texel buffer alignment";
    EXPECT_FALSE(dev.createTexelBuffer(buffer, 8, 8, kRGBA8, t));
    EXPECT_FALSE(dev.createTexelBuffer(buffer, 1024 - 16, 8, kRGBA8, t)) << "texels past the buffer";
    EXPECT_FALSE(dev.createTexelBuffer(buffer, 0, 0, kRGBA8, t));
}

}  // namespace
