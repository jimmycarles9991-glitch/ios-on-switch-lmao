// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_shader_tests.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

namespace {
using Image = raw::ShaderTestImage;
using Images = std::array<std::vector<uint8_t>, 4>;

std::vector<uint8_t> &image(Images &images, Image which)
{
    return images[static_cast<size_t>(which)];
}

void execute(Images &images, const raw::ShaderTestTransfers &plan, size_t texels)
{
    ASSERT_LE(plan.count, plan.transfers.size());
    for (uint8_t i = 0; i < plan.count; ++i) {
        const auto &copy = plan.transfers[i];
        const uint32_t size = ((copy.remap >> 16) & 3) + 1;
        const uint32_t sourceComponents = ((copy.remap >> 20) & 3) + 1;
        const uint32_t destinationComponents = ((copy.remap >> 24) & 3) + 1;
        const auto &source = image(images, copy.source);
        auto &destination = image(images, copy.destination);
        ASSERT_GE(source.size(), texels * sourceComponents * size);
        ASSERT_GE(destination.size(), texels * destinationComponents * size);
        for (size_t pixel = 0; pixel < texels; ++pixel) {
            for (uint32_t component = 0; component < destinationComponents; ++component) {
                const uint32_t select = (copy.remap >> (component * 4)) & 7;
                if (select == 6)
                    continue;
                ASSERT_LT(select, sourceComponents);
                std::memcpy(&destination[(pixel * destinationComponents + component) * size],
                            &source[(pixel * sourceComponents + select) * size], size);
            }
        }
    }
}

TEST(RawShaderTests, PhysicalSampleGridPreservesEverySample)
{
    struct Case {
        uint32_t samples, width, height;
    };

    for (const auto &c : {Case{1, 37, 19}, Case{2, 74, 19}, Case{4, 74, 38}, Case{8, 148, 38}}) {
        raw::ShaderTestShape shape;
        ASSERT_TRUE(raw::shaderTestShape(260, c.samples, 37, 19, 3, shape));
        EXPECT_EQ(shape.width, c.width);
        EXPECT_EQ(shape.height, c.height);
        EXPECT_EQ(shape.width * shape.height, 37u * 19u * c.samples);
        EXPECT_TRUE(shape.packedStencil);
    }
}

TEST(RawShaderTests, RejectsInvalidShapesAndUnavailableAspects)
{
    raw::ShaderTestShape shape;
    EXPECT_FALSE(raw::shaderTestShape(250, 1, 16, 16, 1, shape));
    EXPECT_FALSE(raw::shaderTestShape(252, 1, 16, 16, 2, shape));
    EXPECT_FALSE(raw::shaderTestShape(253, 1, 16, 16, 1, shape));
    for (uint32_t samples : {0u, 3u, 16u})
        EXPECT_FALSE(raw::shaderTestShape(260, samples, 16, 16, 3, shape));
    for (uint32_t width : {0u, 16385u, 0xffffffffu})
        EXPECT_FALSE(raw::shaderTestShape(260, 8, width, 16, 3, shape));
    for (uint8_t flags : {0, 4, 255})
        EXPECT_FALSE(raw::shaderTestShape(260, 1, 16, 16, flags, shape));
    EXPECT_TRUE(raw::shaderTestShape(260, 8, 16384, 16384, 3, shape));
}

void roundTrip(uint32_t format, uint8_t flags)
{
    const size_t bpp = format == 260 ? 8 : format == 252 ? 4 : 1;
    for (const uint32_t samples : {1u, 2u, 4u, 8u}) {
        SCOPED_TRACE(::testing::Message()
                     << "format=" << format << " flags=" << unsigned(flags) << " samples=" << samples);
        raw::ShaderTestShape shape;
        ASSERT_TRUE(raw::shaderTestShape(format, samples, 13, 9, flags, shape));
        const size_t texels = size_t(shape.width) * shape.height;
        Images images;
        auto &attachment = image(images, Image::attachment);
        attachment.resize(texels * bpp);
        for (size_t i = 0; i < attachment.size(); ++i)
            attachment[i] = uint8_t((i * 37 + i / bpp * 3) ^ (i >> 5));
        const auto original = attachment;
        auto &depth = image(images, Image::depth);
        auto &stencil = image(images, Image::stencil);
        depth.resize(texels * 4, 0xf1);
        stencil.resize(texels, 0x72);
        image(images, Image::temporary).resize(texels * 2, 0xa3);
        execute(images, raw::shaderTestTransfers(flags, shape.packedStencil, false), texels);
        for (size_t pixel = 0; pixel < texels; ++pixel) {
            if (flags & raw::kShaderTestDepth) {
                for (size_t byte = 0; byte < 4; ++byte) {
                    ASSERT_EQ(depth[pixel * 4 + byte], original[pixel * bpp + byte]);
                    depth[pixel * 4 + byte] ^= 0x5a;
                }
            }
            if (flags & raw::kShaderTestStencil) {
                ASSERT_EQ(stencil[pixel], original[pixel * bpp + (format == 260 ? 4 : 0)]);
                stencil[pixel] ^= 0xc7;
            }
        }
        execute(images, raw::shaderTestTransfers(flags, shape.packedStencil, true), texels);
        for (size_t pixel = 0; pixel < texels; ++pixel) {
            if (format != 253)
                for (size_t byte = 0; byte < 4; ++byte)
                    ASSERT_EQ(attachment[pixel * bpp + byte],
                              original[pixel * bpp + byte] ^ ((flags & raw::kShaderTestDepth) ? 0x5a : 0));
            if (format != 252) {
                const size_t offset = pixel * bpp + (format == 260 ? 4 : 0);
                ASSERT_EQ(attachment[offset], original[offset] ^ ((flags & raw::kShaderTestStencil) ? 0xc7 : 0));
            }
            if (format == 260) {
                EXPECT_EQ(attachment[pixel * bpp + 6], original[pixel * bpp + 6]);
                EXPECT_EQ(attachment[pixel * bpp + 7], original[pixel * bpp + 7]);
            }
        }
    }
}

TEST(RawShaderTests, DepthRoundTrip)
{
    roundTrip(252, raw::kShaderTestDepth);
}

TEST(RawShaderTests, StencilRoundTrip)
{
    roundTrip(253, raw::kShaderTestStencil);
}

TEST(RawShaderTests, PackedAspectsStayIndependent)
{
    for (uint8_t flags : {1, 2, 3})
        roundTrip(260, flags);
}

TEST(RawShaderTests, DependentTemporaryCopiesAreOrdered)
{
    for (bool store : {false, true}) {
        for (uint8_t flags : {2, 3}) {
            const auto plan = raw::shaderTestTransfers(flags, true, store);
            ASSERT_GE(plan.count, 2);
            const auto &producer = plan.transfers[plan.count - 2];
            const auto &consumer = plan.transfers[plan.count - 1];
            EXPECT_EQ(producer.destination, Image::temporary);
            EXPECT_EQ(consumer.source, Image::temporary);
            EXPECT_TRUE(consumer.ordered);
        }
    }
}
}  // namespace
