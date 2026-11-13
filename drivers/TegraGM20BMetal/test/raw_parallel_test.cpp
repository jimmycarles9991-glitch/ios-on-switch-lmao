// SPDX-License-Identifier: MIT
// raw/runtime/raw_parallel.h: a parallel encoder's sub-encoders as the passes m2v replays (the first loads as the parallel
// pass, the last stores as it, each seam keeps the attachment), and their commands recorded again into the parallel
// encoder's command buffer with its passes, setBytes chunks and extra words
#include "../raw/runtime/raw_parallel.h"

#include <gtest/gtest.h>

namespace {

constexpr uint8_t kClear = 2, kDontCare = 0, kResolve = 2;

TEST(RawParallel, SeamsKeepWhatTheSubEncoderBeforeDrew)
{
    EXPECT_EQ(raw::seamLoad(kClear, 0), kClear);
    for (size_t at = 1; at < 4; at++)
        EXPECT_EQ(raw::seamLoad(kClear, at), raw::kLoadActionLoad);
    for (size_t at = 0; at + 1 < 4; at++)
        EXPECT_EQ(raw::seamStore(kDontCare, at, 4), raw::kStoreActionStore);
    EXPECT_EQ(raw::seamStore(kDontCare, 3, 4), kDontCare);
    EXPECT_EQ(raw::seamStore(kResolve, 3, 4), kResolve);
    EXPECT_EQ(raw::seamLoad(kClear, 0), kClear);
    EXPECT_EQ(raw::seamStore(kResolve, 0, 1), kResolve) << "one sub-encoder is the parallel pass itself";
}

TEST(RawParallel, CommandsTakeTheParallelCommandBuffersPassesChunksAndWords)
{
    Cmd render = cmd(OpRender);
    render.x = 0;
    EXPECT_EQ(raw::spliced(render, 5, 2, 40).x, 5u);
    Cmd bytes = cmd(OpBytes, 1, 0);
    bytes.a = 256;
    bytes.c = 1;
    const Cmd b = raw::spliced(bytes, 5, 2, 40);
    EXPECT_EQ(b.c, 3u);
    EXPECT_EQ(b.a, 256u) << "the offset in its chunk";
    for (Op op : {OpViewport, OpBlendColor, OpDepthBias, OpDrawIndexed}) {
        Cmd c = cmd(op);
        c.x = 7;
        EXPECT_EQ(raw::spliced(c, 5, 2, 40).x, 40u) << int(op);
    }
    Cmd indirect = cmd(OpDrawIndirect);
    indirect.x = 64;
    EXPECT_EQ(raw::spliced(indirect, 5, 2, 40).x, 64u) << "an argument offset";
    Cmd buffer = cmd(OpBuffer, 1, 3);
    buffer.c = 128;
    EXPECT_EQ(raw::spliced(buffer, 5, 2, 40).c, 128u) << "a buffer offset";
}

TEST(RawParallel, ExtraWordsAreTheRecordedOnes)
{
    EXPECT_EQ(raw::extraWords(cmd(OpViewport)), 6u);
    EXPECT_EQ(raw::extraWords(cmd(OpDispatch)), 12u);
    EXPECT_EQ(raw::extraWords(cmd(OpBlendColor)), 4u);
    EXPECT_EQ(raw::extraWords(cmd(OpDepthBias)), 3u);
    EXPECT_EQ(raw::extraWords(cmd(OpDrawIndexed)), 3u);
    for (Op op : {OpRender, OpDraw, OpDrawIndirect, OpDrawIndexedIndirect, OpBytes, OpEnd, OpScissor})
        EXPECT_EQ(raw::extraWords(cmd(op)), 0u) << int(op);
}

TEST(RawParallel, ViewportAndScissorArraysTakeTheirWords)
{
    EXPECT_EQ(raw::extraWords(cmd(OpViewport, 0, 1)), 6u);
    EXPECT_EQ(raw::extraWords(cmd(OpViewport, 0, 16)), 96u);
    EXPECT_EQ(raw::extraWords(cmd(OpScissor, 0, 1)), 0u) << "one rectangle is the command's own words";
    EXPECT_EQ(raw::extraWords(cmd(OpScissor, 0, 16)), 64u);
    Cmd scissors = cmd(OpScissor, 0, 3);
    scissors.x = 7;
    EXPECT_EQ(raw::spliced(scissors, 5, 2, 40).x, 40u);
}

}  // namespace
