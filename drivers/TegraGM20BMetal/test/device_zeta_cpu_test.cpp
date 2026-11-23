// SPDX-License-Identifier: MIT
// replaceRegion and getBytes of a one-aspect depth or stencil texture go through the copy engine
// (gm20b::Device::transferTexture), as a colour texture's do: Apple's driver takes a CPU copy of a private depth texture
// (an M-series Mac with API validation off reads back every byte written). A combined depth/stencil texture moves its
// depth floats alone and a multisampled one its pixels' samples in turn (zeta_cpu.h, device_zeta_image_test).
#include "device_harness.h"

#include <utility>
#include <vector>

namespace {

constexpr uint32_t kDepth16 = 250, kDepth32Float = 252, kStencil8 = 253, kDepth32FloatStencil8 = 260;

class DeviceZetaCpu : public harness::DeviceTest {
protected:
    gm20b::TextureTransferResult write(const gm20b::Texture &t, uint32_t bpp)
    {
        std::vector<uint8_t> texels(size_t(t.width) * t.height * bpp, 0x3c);
        const uint32_t origin[3] = {0, 0, 0}, size[3] = {t.width, t.height, 1};
        return dev.transferTexture(t, 0, 0, origin, size, texels.data(), uint64_t(t.width) * bpp, 0, true, nullptr);
    }

    bool lastJobNames(const gm20b::Texture &t)
    {
        const uint64_t memory = kernel_model::model.resolve({t.va, TegraGM20BSurfaceGpuAddress, 0});
        for (const TegraGM20BAbiSurfaceUse &u : kernel_model::model.jobs.back().uses)
            if (u.handle == memory)
                return true;
        return false;
    }
};

TEST_F(DeviceZetaCpu, AOneAspectDepthOrStencilTextureMovesThroughTheCopyEngine)
{
    constexpr std::pair<uint32_t, uint32_t> kOneAspect[] = {{kDepth32Float, 4}, {kDepth16, 2}, {kStencil8, 1}};
    for (const auto &[format, bpp] : kOneAspect) {
        gm20b::Texture zeta;
        ASSERT_TRUE(dev.createOwnedZeta(64, 48, format, 1, true, zeta)) << format;
        const size_t jobs = kernel_model::model.jobs.size();
        EXPECT_EQ(write(zeta, bpp), gm20b::TextureTransferResult::Complete) << format;
        ASSERT_GT(kernel_model::model.jobs.size(), jobs) << format;
        EXPECT_TRUE(lastJobNames(zeta)) << format;
        dev.destroyTexture(zeta);
    }
}

TEST_F(DeviceZetaCpu, ACombinedZetaMovesItsDepthAndAMultisampledOneItsSamples)
{
    gm20b::Texture combined, multisampled;
    ASSERT_TRUE(dev.createOwnedZeta(64, 48, kDepth32FloatStencil8, 1, true, combined));
    EXPECT_EQ(write(combined, 4), gm20b::TextureTransferResult::Complete) << "four bytes a texel: its depth float";
    EXPECT_TRUE(lastJobNames(combined));
    ASSERT_TRUE(dev.createOwnedZeta(64, 48, kDepth32Float, 4, true, multisampled));
    EXPECT_EQ(write(multisampled, 4 * 4), gm20b::TextureTransferResult::Complete) << "a pixel's four samples in turn";
    EXPECT_TRUE(lastJobNames(multisampled));
    dev.destroyTexture(combined);
    dev.destroyTexture(multisampled);
}

}  // namespace
