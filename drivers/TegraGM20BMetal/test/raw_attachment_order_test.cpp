// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_attachment_order.h"

namespace {
using raw::AttachmentAccess;
using raw::AttachmentBarrier;

TEST(AttachmentOrder, RepeatedOrderedStorageDrawsNeedNoCpuBarrier)
{
    const AttachmentAccess access{0, 0, 0, 1};
    EXPECT_EQ(access.after(0, 0), AttachmentBarrier::None);
    EXPECT_EQ(access.after(0, 1), AttachmentBarrier::None);
    EXPECT_EQ(access.after(0, 0xff), AttachmentBarrier::None);
}

TEST(AttachmentOrder, StorageFollowsRenderTargetWritesWithShaderVisibility)
{
    const AttachmentAccess access{0, 0, 0, 2};
    EXPECT_EQ(access.after(2, 0), AttachmentBarrier::Shader);
    EXPECT_EQ(access.after(1, 0), AttachmentBarrier::None);
}

TEST(AttachmentOrder, ShaderWritesReachBlendingAndTextureFetch)
{
    EXPECT_EQ((AttachmentAccess{1, 0, 0, 0}.after(0, 1)), AttachmentBarrier::Shader);
    EXPECT_EQ((AttachmentAccess{0, 1, 0, 0}.after(0, 1)), AttachmentBarrier::Shader);
    EXPECT_EQ((AttachmentAccess{0, 0, 1, 0}.after(0, 1)), AttachmentBarrier::Shader);
    EXPECT_EQ((AttachmentAccess{0, 0, 2, 0}.after(0, 1)), AttachmentBarrier::None);
}

TEST(AttachmentOrder, MixedTargetsKeepIndependentStorageAndRopOrdering)
{
    const AttachmentAccess access{2, 2, 0, 1};
    EXPECT_EQ(access.after(2, 1), AttachmentBarrier::None);
    EXPECT_EQ(access.after(1, 0), AttachmentBarrier::Shader);
    EXPECT_EQ(access.after(0, 2), AttachmentBarrier::Shader);
}

TEST(AttachmentOrder, TextureFetchKeepsPixelBarrier)
{
    const AttachmentAccess access{0, 1, 1, 0};
    EXPECT_EQ(access.after(1, 0), AttachmentBarrier::Pixel);
    EXPECT_EQ(access.after(0, 0), AttachmentBarrier::None);
    EXPECT_EQ(access.after(1, 1), AttachmentBarrier::Shader);
}

TEST(AttachmentOrder, PixelBarrierDoesNotForgetOtherPendingShaderWrites)
{
    raw::AttachmentWrites writes{2, 1};
    const AttachmentAccess sampleSecond{0, 0, 2, 0};
    const auto barrier = sampleSecond.after(writes.rop, writes.storage);
    ASSERT_EQ(barrier, AttachmentBarrier::Pixel);
    writes.synchronized(barrier);
    EXPECT_EQ(writes.rop, 0);
    EXPECT_EQ(writes.storage, 1);
    const AttachmentAccess blendFirst{1, 1, 0, 0};
    EXPECT_EQ(blendFirst.after(writes.rop, writes.storage), AttachmentBarrier::Shader);
    writes.synchronized(AttachmentBarrier::Shader);
    EXPECT_EQ(writes.storage, 0);
}

}  // namespace
