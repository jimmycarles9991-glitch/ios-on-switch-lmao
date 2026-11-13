// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "TegraVICProbe.h"
#include "vic_model.h"

namespace {
constexpr uint64_t kSource = 0x10000000, kTarget = 0x18000000;
constexpr uint32_t kPitch = 512, kWidth = 84, kHeight = 70, kX = 6, kY = 10;

uint32_t pixel(uint32_t x, uint32_t y)
{
    return 0xff000000u | (x * 17u & 255u) << 16 | (y * 29u & 255u) << 8 | ((x + y) * 11u & 255u);
}

struct Surface {
    std::vector<uint8_t> source = std::vector<uint8_t>(256 * 48);
    std::vector<uint8_t> target = std::vector<uint8_t>(kPitch * kHeight * 2, TEGRA_VIC_POISON_BYTE);
    VicModel model{};

    Surface()
    {
        for (uint32_t y = 0; y != 48; ++y)
            for (uint32_t x = 0; x != 56; ++x) {
                const auto value = pixel(x, y);
                memcpy(source.data() + y * 256 + x * 4, &value, 4);
            }
        model.region[0] = {kSource, source.data(), source.size()};
        model.region[1] = {kTarget, target.data(), target.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
    }

    TegraVICImage output(bool nv12)
    {
        auto image = tegra_vic_probe_bgra(kTarget, target.size(), kWidth, kHeight);
        image.pitch[0] = kPitch;
        if (nv12) {
            image.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'f'));
            image.pitch[1] = kPitch;
            image.rows[1] = kHeight / 2;
            image.span[0] = kPitch * kHeight;
            image.base[1] = kTarget + image.span[0];
            image.span[1] = kPitch * kHeight / 2;
            image.standard = TEGRA_VIC_YCC_709;
        }
        return image;
    }
};

void check(uint32_t transform, uint32_t scale, bool nv12, bool bilinear)
{
    SCOPED_TRACE(testing::Message() << transform << '/' << scale << '/' << nv12 << '/' << bilinear);
    Surface surface;
    auto source = tegra_vic_probe_bgra(kSource, surface.source.size(), 56, 48);
    auto target = surface.output(nv12);
    constexpr uint32_t width = 24, height = 16, sourceX = 4, sourceY = 8;
    const bool transpose = transform & TEGRA_VIC_ROT90;
    const uint32_t dw = (transpose ? height : width) * scale;
    const uint32_t dh = (transpose ? width : height) * scale;
    auto operation = tegra_vic_probe_op(width, height, dw, dh, transform);
    operation.srcX = sourceX << 16;
    operation.srcY = sourceY << 16;
    operation.dstX = kX;
    operation.dstY = kY;
    operation.nearest = !bilinear;
    TegraVICJob job{};
    const auto error = tegra_vic_build(&source, &target, &operation, &kTegraVICTraits, 0x80001000, 0x80002000, &job);
    ASSERT_EQ(error, nullptr) << error;
    vic_model_run(&surface.model, &job);
    EXPECT_EQ(surface.model.clipped + surface.model.faults, 0u);
    EXPECT_EQ(surface.model.writes, uint64_t(dw) * dh * (nv12 ? 5 : 4) / 4);

    std::vector<uint8_t> expected(surface.target.size(), TEGRA_VIC_POISON_BYTE);
    TegraVICMatrix matrix{};
    tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, 1, &kTegraVICTraits.out, &matrix);
    for (uint32_t y = 0; y != height * scale; ++y)
        for (uint32_t x = 0; x != width * scale; ++x) {
            uint32_t u, v;
            tegra_vic_transform_point(transform, width * scale, height * scale, x, y, &u, &v);
            u += kX;
            v += kY;
            const auto value = pixel(sourceX + x / scale, sourceY + y / scale);
            if (!nv12) {
                memcpy(expected.data() + v * kPitch + u * 4, &value, 4);
            } else {
                const uint32_t input[] = {((value >> 16) & 255u) * 4, ((value >> 8) & 255u) * 4, (value & 255u) * 4};
                uint32_t output[3];
                tegra_vic_matrix_apply(&matrix, input, output);
                expected[v * kPitch + u] = output[0] >> 2;
                if (!((u | v) & 1)) {
                    const auto at = kPitch * kHeight + (v / 2) * kPitch + u;
                    expected[at] = output[kTegraVICTraits.out.cbChannel] >> 2;
                    expected[at + 1] = output[kTegraVICTraits.out.crChannel] >> 2;
                }
            }
        }
    EXPECT_EQ(surface.target, expected);
}

TEST(SwitchVICSubrect, AllTransformsCroppedPaddedRGB)
{
    for (uint32_t transform = 0; transform != 8; ++transform)
        for (uint32_t scale : {1u, 2u})
            check(transform, scale, false, false);
}

TEST(SwitchVICSubrect, AllTransformsCroppedPaddedNV12)
{
    for (uint32_t transform = 0; transform != 8; ++transform)
        for (uint32_t scale : {1u, 2u})
            check(transform, scale, true, false);
}

TEST(SwitchVICSubrect, BilinearUsesSameQualifiedPlacement)
{
    // The register model checks placement and sampling bounds; filter arithmetic has separate hardware qualification.
    for (uint32_t transform = 0; transform != 8; ++transform)
        check(transform, 2, false, true);
}
}  // namespace
