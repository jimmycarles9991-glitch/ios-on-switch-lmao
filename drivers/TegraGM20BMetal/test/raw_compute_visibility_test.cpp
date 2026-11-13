// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_compute_visibility.h"
#include "../../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb197.h"
#include "../../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb1c0.h"

#include <gtest/gtest.h>

#include <array>

namespace {

void expectMethod(uint32_t word, uint32_t subchannel, uint32_t method, uint32_t value)
{
    EXPECT_EQ(word >> 29, 4u);
    EXPECT_EQ((word >> 13) & 7, subchannel);
    EXPECT_EQ((word & 0xfff) * 4, method);
    EXPECT_EQ((word >> 16) & 0x1fff, value);
}

constexpr uint32_t kProducer =
    NVB1C0_INVALIDATE_SHADER_CACHES_FLUSH_DATA_TRUE << (0 ? NVB1C0_INVALIDATE_SHADER_CACHES_FLUSH_DATA) |
    NVB1C0_INVALIDATE_SHADER_CACHES_DATA_TRUE << (0 ? NVB1C0_INVALIDATE_SHADER_CACHES_DATA);
constexpr uint32_t kConsumer =
    NVB197_INVALIDATE_SHADER_CACHES_NO_WFI_GLOBAL_DATA_TRUE
        << (0 ? NVB197_INVALIDATE_SHADER_CACHES_NO_WFI_GLOBAL_DATA) |
    NVB197_INVALIDATE_SHADER_CACHES_NO_WFI_CONSTANT_TRUE << (0 ? NVB197_INVALIDATE_SHADER_CACHES_NO_WFI_CONSTANT);

TEST(RawComputeVisibility, FrontierStoresReleasedBeforeGraphicsConstantRead)
{
    std::array<uint32_t, 3> words{0, 0, 0xdeadbeef};
    raw::Push push{words.data(), words.data() + 2};
    raw::computeToGraphics(push, false);
    ASSERT_EQ(push.at, push.end);
    expectMethod(words[0], raw::SUBC_COMPUTE, NVB1C0_INVALIDATE_SHADER_CACHES, kProducer);
    expectMethod(words[1], raw::SUBC_3D, NVB197_INVALIDATE_SHADER_CACHES_NO_WFI, kConsumer);
    EXPECT_EQ(words[2], 0xdeadbeefu);
}

TEST(RawComputeVisibility, ComputeImagesReleasedBeforeTextureAndConstantReads)
{
    std::array<uint32_t, 4> words{0, 0, 0, 0xdeadbeef};
    raw::Push push{words.data(), words.data() + 3};
    raw::computeToGraphics(push, true);
    ASSERT_EQ(push.at, push.end);
    expectMethod(words[0], raw::SUBC_COMPUTE, NVB1C0_INVALIDATE_SHADER_CACHES, kProducer);
    expectMethod(words[1], raw::SUBC_3D, NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI,
                 NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI_LINES_ALL);
    expectMethod(words[2], raw::SUBC_3D, NVB197_INVALIDATE_SHADER_CACHES_NO_WFI, kConsumer);
    EXPECT_EQ(words[3], 0xdeadbeefu);
}

}  // namespace
