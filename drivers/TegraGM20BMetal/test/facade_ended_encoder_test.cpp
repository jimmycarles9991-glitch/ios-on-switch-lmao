// SPDX-License-Identifier: MIT
// A command recorded on an encoder after its endEncoding (Metal's validation error) on the facade over the kernel
// model, against what an M4 Max with validation off does (macOS 27.2.1): where nothing began after the end it runs
// inside that encoder, after its work and before the next encoder; after an encoder of another kind or an event it
// crashes the process, and a second endEncoding aborts it, which the driver turns into its command buffer failing
// InvalidResource. Blits run on the copy engine's model (facade_blit_engines.h); draws are emitted, not run.
#include "facade_blit_engines.h"
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <string>

namespace {

using scale_frames::opened;

id<MTLBuffer> buffer(uint8_t fill)
{
    id<MTLBuffer> b = [opened().device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    memset(b.contents, fill, b.length);
    return b;
}

uint8_t byte(id<MTLBuffer> b, size_t at)
{
    return static_cast<const uint8_t *>(b.contents)[at];
}

MTLRenderPassDescriptor *onePass()
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = scale_frames::targets(opened().device, {1, 1, 1})[0];
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return d;
}

// One quad of the BGRA8 state, bound as the render server's passes bind it (scale_frames.h)
void quad(id<MTLRenderCommandEncoder> e)
{
    const float vertices[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float colour[4] = {1, 0, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e setVertexBytes:transform length:sizeof transform atIndex:2];
    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

id<MTLRenderCommandEncoder> drawnPass(id<MTLCommandBuffer> cb)
{
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:onePass()];
    [e setRenderPipelineState:opened().state];
    quad(e);
    return e;
}

// `cb` committed, completed with no error and no failed counter moved
void expectCompleted(id<MTLCommandBuffer> cb)
{
    NSDictionary *before = harness::counters(opened().device);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
    EXPECT_EQ(harness::gm20bPath(cb), "gm20b");
    EXPECT_TRUE(harness::failedMoved(before, harness::counters(opened().device)).empty());
}

// `cb` committed, failed InvalidResource naming no predicate, counted once
void expectInvalid(id<MTLCommandBuffer> cb)
{
    NSDictionary *before = harness::counters(opened().device);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource) << cb.error.description.UTF8String;
    EXPECT_EQ(harness::gm20bPath(cb), "error:none");
    EXPECT_EQ(harness::failedMoved(before, harness::counters(opened().device)), harness::failedOnce("none"));
}

// endprobe b_a_copy: the late copy lands over the encoder's own ([0, 32) 0xaa over 0x00..0x1f), the late fill runs, and
// a later encoder's copy of the destination holds both
TEST(GM20BFacadeEndedEncoder, ACopyOnTheBlitEncoderThatEndedLastRunsInItBeforeTheNextEncoder)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLBuffer> from = buffer(0), late = buffer(0xaa), into = buffer(0), snapshot = buffer(0);
    for (size_t i = 0; i < 256; i++)
        static_cast<uint8_t *>(from.contents)[i] = uint8_t(i);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:from sourceOffset:0 toBuffer:into destinationOffset:0 size:64];
    [b endEncoding];
    [b copyFromBuffer:late sourceOffset:0 toBuffer:into destinationOffset:0 size:32];
    [b fillBuffer:into range:NSMakeRange(128, 32) value:0x55];
    id<MTLBlitCommandEncoder> next = [cb blitCommandEncoder];
    [next copyFromBuffer:into sourceOffset:0 toBuffer:snapshot destinationOffset:0 size:256];
    [next endEncoding];
    NSDictionary *before = harness::counters(opened().device);
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_TRUE(harness::failedMoved(before, harness::counters(opened().device)).empty());
    id<MTLBuffer> written[] = {into, snapshot};
    for (id<MTLBuffer> w : written) {
        EXPECT_EQ(byte(w, 0), 0xaa);
        EXPECT_EQ(byte(w, 31), 0xaa);
        EXPECT_EQ(byte(w, 32), 0x20);
        EXPECT_EQ(byte(w, 63), 0x3f);
        EXPECT_EQ(byte(w, 64), 0x00);
        EXPECT_EQ(byte(w, 128), 0x55);
        EXPECT_EQ(byte(w, 159), 0x55);
        EXPECT_EQ(byte(w, 160), 0x00);
    }
}

// endprobe b_b_ended_order: through the reference of the first of two blit encoders (Metal's one object), the copy runs
// after the second's fill
TEST(GM20BFacadeEndedEncoder, ACopyThroughAnEarlierBlitEncoderRunsAfterTheLastOne)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLBuffer> from = buffer(0), late = buffer(0xaa), into = buffer(0);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> first = [cb blitCommandEncoder];
    [first copyFromBuffer:from sourceOffset:0 toBuffer:into destinationOffset:0 size:64];
    [first endEncoding];
    id<MTLBlitCommandEncoder> second = [cb blitCommandEncoder];
    [second fillBuffer:into range:NSMakeRange(0, 64) value:0x11];
    [second endEncoding];
    [first copyFromBuffer:late sourceOffset:0 toBuffer:into destinationOffset:0 size:32];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(byte(into, 0), 0xaa);
    EXPECT_EQ(byte(into, 31), 0xaa);
    EXPECT_EQ(byte(into, 32), 0x11);
    EXPECT_EQ(byte(into, 63), 0x11);
}

// endprobe r_a_draw, r_a_two_late, r_a_pipeline: late draws, and one after a late state, run in the ended pass
TEST(GM20BFacadeEndedEncoder, DrawsOnTheRenderEncoderThatEndedLastComplete)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = drawnPass(cb);
    [e endEncoding];
    quad(e);
    [e setRenderPipelineState:opened().state];
    quad(e);
    id<MTLBlitCommandEncoder> next = [cb blitCommandEncoder];
    [next fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    [next endEncoding];
    expectCompleted(cb);
}

// endprobe p_a_sub, p_b_sub: a late draw on a parallel encoder's ended sub-encoder runs in that sub-encoder's part,
// also after the next sub-encoder began
TEST(GM20BFacadeEndedEncoder, ADrawOnAnEndedParallelSubEncoderCompletes)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLParallelRenderCommandEncoder> parallel = [cb parallelRenderCommandEncoderWithDescriptor:onePass()];
    id<MTLRenderCommandEncoder> first = [parallel renderCommandEncoder];
    [first setRenderPipelineState:opened().state];
    quad(first);
    [first endEncoding];
    id<MTLRenderCommandEncoder> second = [parallel renderCommandEncoder];
    [second setRenderPipelineState:opened().state];
    quad(first);
    quad(second);
    [second endEncoding];
    [parallel endEncoding];
    expectCompleted(cb);
}

// endprobe p_c_parent_ended (an M4 Max crashes): a draw on a sub-encoder after its parallel encoder ended, which can
// reach no command buffer to fail, records nothing and is counted skipped once
TEST(GM20BFacadeEndedEncoder, ADrawOnASubEncoderAfterItsParallelEncoderEndedIsSkipped)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLParallelRenderCommandEncoder> parallel = [cb parallelRenderCommandEncoderWithDescriptor:onePass()];
    id<MTLRenderCommandEncoder> sub = [parallel renderCommandEncoder];
    [sub setRenderPipelineState:opened().state];
    quad(sub);
    [sub endEncoding];
    [parallel endEncoding];
    NSDictionary *before = harness::counters(opened().device);
    quad(sub);
    EXPECT_EQ(harness::skippedMoved(before, harness::counters(opened().device)), harness::skippedOnce("enc.pass"));
    expectCompleted(cb);
}

// endprobe r_c_blit_open: a draw on a render encoder that ended, recorded while a blit encoder is open
TEST(GM20BFacadeEndedEncoder, ADrawAfterABlitEncoderBeganFailsInvalidResource)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = drawnPass(cb);
    [e endEncoding];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    quad(e);
    [b endEncoding];
    expectInvalid(cb);
}

// endprobe b_c_render_ended: a copy on a blit encoder that ended, recorded after a render encoder began and ended
TEST(GM20BFacadeEndedEncoder, ACopyAfterARenderEncoderFailsInvalidResource)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    [b endEncoding];
    [drawnPass(cb) endEncoding];
    [b copyFromBuffer:buffer(1) sourceOffset:0 toBuffer:buffer(0) destinationOffset:0 size:64];
    expectInvalid(cb);
}

// endprobe r_a_event_between: an event between the end and the command
TEST(GM20BFacadeEndedEncoder, ACopyAfterAnEventFailsInvalidResource)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    [b endEncoding];
    [cb encodeSignalEvent:[opened().device newEvent] value:1];
    [b copyFromBuffer:buffer(1) sourceOffset:0 toBuffer:buffer(0) destinationOffset:0 size:64];
    expectInvalid(cb);
}

// endprobe i_blit_blit, i_render_render: Apple aborts on an encoder begun while another is encoding; the driver ends
// the first there, and the first's own endEncoding after is an end of an encoder that had ended
TEST(GM20BFacadeEndedEncoder, EndingAnEncoderTheNextBeginEndedFailsInvalidResource)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> first = [cb blitCommandEncoder];
    [first fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    id<MTLBlitCommandEncoder> second = [cb blitCommandEncoder];
    [second fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:2];
    [first endEncoding];
    [second endEncoding];
    expectInvalid(cb);
}

// endprobe r_e_end_twice, b_e_end_twice, c_e_end_twice: a second endEncoding
TEST(GM20BFacadeEndedEncoder, EndEncodingTwiceFailsInvalidResource)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> render = [[opened().device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = drawnPass(render);
    [e endEncoding];
    [e endEncoding];
    expectInvalid(render);
    id<MTLCommandBuffer> blit = [[opened().device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [blit blitCommandEncoder];
    [b fillBuffer:buffer(0) range:NSMakeRange(0, 64) value:1];
    [b endEncoding];
    [b endEncoding];
    expectInvalid(blit);
}

}  // namespace
