// SPDX-License-Identifier: MIT
// What Metal's validation layer would stop completes on the facade as Apple's driver completes it, nothing on m2v
// (client_errors.h, measured on an M-series Mac with API validation off: every one of these command buffers
// completed with no error): a copy or fill past its buffer, indirect arguments past theirs, a state of the other
// four-channel 8-bit order or another sample count than its pass, sample positions past the pass's samples, and a state
// whose colour Apple writes as reinterpreted bits (its draw skipped). The kernel refusing a submission or the channel
// fails the command buffer Internal naming the kernel's answer (TegraGM20BMetalPredicates.h predFailure), as a visibility result
// mode or buffer or an event Metal's validation stops fails it, and a visibility
// mode the device has no report page for fails it OutOfMemory. A state of another depth format than its pass's is
// drawn against the pass's attachment, as Apple's GPU draws it. A command buffer whose later job the kernel refused after
// earlier ones ran counts as not run; a refused reset of indirect commands wrote none of their words. Each failed
// command buffer moves `failed` and its predicate's failed.<predicate> by one; a completed one moves neither. The
// kernel's GPU address fault fails as Metal reports one. A render execute's range read past its buffer runs none, and
// one at an offset off 4 bytes is read from the offset rounded down, as an M4 Max's GPU reads it.
#include "facade_harness.h"
#include "../TegraGM20BMetalIndirect.h"
#include "../batch.h"
#include "../mme.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <condition_variable>
#include <mutex>
#include <string>

// A plain event this device did not make: none of a shared event's selectors
@interface ForeignEvent : NSObject <MTLEvent>
@end

@implementation ForeignEvent
@synthesize label;

- (id<MTLDevice>)device
{
    return nil;
}

@end

// A visibility result buffer this device did not make
@interface ForeignBuffer : NSObject
@end

@implementation ForeignBuffer

- (id<MTLDevice>)device
{
    return nil;
}

- (NSUInteger)length
{
    return 64;
}

@end

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;
constexpr float kTriangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

// The facade over the pack of the BGRA8 state, and that state
Opened openPacked()
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    return {device, state};
}

id<MTLTexture> target(id<MTLDevice> device, MTLPixelFormat format, NSUInteger samples = 1)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    return [device newTextureWithDescriptor:d];
}

MTLRenderPassDescriptor *pass(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = t.sampleCount > 1 ? MTLStoreActionDontCare : MTLStoreActionStore;
    return p;
}

void drawInto(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *p, id<MTLRenderPipelineState> state)
{
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

NSString *path(id<MTLCommandBuffer> cb)
{
    return ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
}

// Each helper below judges what `cb` moved from `before`, the counters read before its recording began
// (facade_harness.h countersMoved)

// `cb` committed failed with `code` for predicate `pred`, counted once under it, nothing skipped
void expectError(id<MTLDevice> device, NSDictionary *before, id<MTLCommandBuffer> cb, const char *pred,
                 MTLCommandBufferError code)
{
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(std::string(cb.error.domain.UTF8String ?: ""), std::string(MTLCommandBufferErrorDomain.UTF8String));
    EXPECT_EQ(cb.error.code, code) << text(cb.error);
    EXPECT_EQ(std::string(path(cb).UTF8String ?: ""), std::string("error:") + pred);
    NSDictionary *counts = counters(device);
    EXPECT_EQ(failedMoved(before, counts), failedOnce(pred)) << text(counts);
    EXPECT_TRUE(skippedMoved(before, counts).empty()) << text(counts);
}

// As expectError, the command buffer one its predicate failed (TegraGM20BMetalPredicates.h predFailure)
void expectFailed(id<MTLDevice> device, NSDictionary *before, id<MTLCommandBuffer> cb, const char *pred,
                  MTLCommandBufferError code)
{
    expectError(device, before, cb, pred, code);
    NSDictionary *counts = counters(device);
    NSString *failed = [NSString stringWithFormat:@"failed.%s", pred];
    EXPECT_EQ([counts[failed] intValue], 1) << text(counts);
}

// `cb` committed completed on the driver, no error, no failure counted, the skips `skipped` counted
void expectRan(id<MTLDevice> device, NSDictionary *before, id<MTLCommandBuffer> cb,
               const std::map<std::string, long long> &skipped)
{
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(std::string(path(cb).UTF8String ?: ""), "gm20b");
    NSDictionary *counts = counters(device);
    EXPECT_TRUE(failedMoved(before, counts).empty()) << text(counts);
    EXPECT_NE(counts[@"failed"], nil) << text(counts);
    EXPECT_EQ(skippedMoved(before, counts), skipped) << text(counts);
    EXPECT_NE(counts[@"skipped"], nil) << text(counts);
}

void expectCompleted(id<MTLDevice> device, NSDictionary *before, id<MTLCommandBuffer> cb)
{
    expectRan(device, before, cb, {});
}

// ... a call of it skipped as `pred` (Metal's validation stops it, the driver runs it as Apple's driver does)
void expectSkipped(id<MTLDevice> device, NSDictionary *before, id<MTLCommandBuffer> cb, const char *pred)
{
    expectRan(device, before, cb, skippedOnce(pred));
}

void bufferCopyPastItsBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> a = [o.device newBufferWithLength:4096 options:0], b = [o.device newBufferWithLength:4096 options:0];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:1 << 20];
    [blit endEncoding];
    expectSkipped(o.device, before, cb, "call.bufcopy");
}

void bufferFillPastItsBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> a = [o.device newBufferWithLength:4096 options:0];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:a range:NSMakeRange(0, 1 << 22) value:7];
    [blit endEncoding];
    expectSkipped(o.device, before, cb, "call.bufcopy");
}

void indirectArgumentsPastTheirBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLBuffer> arguments = [o.device newBufferWithLength:16 options:0];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(target(o.device, kPacked))];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:arguments indirectBufferOffset:4];
    [e endEncoding];
    expectSkipped(o.device, before, cb, "enc.idxrange");
}

// Apple's driver draws the right colour (the target orders the channels)
void stateOfTheOtherChannelOrder()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, MTLPixelFormatRGBA8Unorm)), o.state);
    expectCompleted(o.device, before, cb);
}

// Apple's driver writes the other format's bits: the draw is skipped, the command buffer completed
void stateOfAnotherColourFormat()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, MTLPixelFormatRGBA16Float)), o.state);
    expectSkipped(o.device, before, cb, "enc.colorfmt");
}

void stateSamplesNotThePasss()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked, 4)), o.state);
    expectCompleted(o.device, before, cb);
}

void samplePositionsPastThePasssSamples()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    MTLRenderPassDescriptor *p = pass(target(o.device, kPacked, 2));
    const MTLSamplePosition positions[4] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};
    [p setSamplePositions:positions count:4];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    expectSkipped(o.device, before, cb, "pass.positions");
}

// Metal's validation stops each (MTLRenderCommandEncoder.h setVisibilityResultMode:offset:): a mode set in a pass of no
// visibility result buffer, an offset past the buffer, a mode MTLVisibilityResultMode lacks
void visibilityModesMetalStops()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> results = [o.device newBufferWithLength:64 options:0];

    const struct {
        bool buffer;
        NSUInteger mode, offset;
    } stopped[] = {
        {false, MTLVisibilityResultModeBoolean, 0}, {true, MTLVisibilityResultModeCounting, 64}, {true, 7, 0}};

    for (const auto &s : stopped) {
        MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
        p.visibilityResultBuffer = s.buffer ? results : nil;
        NSDictionary *before = counters(o.device);
        id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e setVisibilityResultMode:MTLVisibilityResultMode(s.mode) offset:s.offset];
        [e endEncoding];
        expectError(o.device, before, cb, "call.vis", MTLCommandBufferErrorInternal);
    }
}

// A visibility result buffer of another device, which Metal's validation stops: the command buffer fails at its recording
void visibilityBufferOfAnotherDevice()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
    p.visibilityResultBuffer = (id<MTLBuffer>)[ForeignBuffer new];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];
    [e endEncoding];
    expectError(o.device, before, cb, "call.vis", MTLCommandBufferErrorInternal);
}

// The draw macro calls the kernel model's jobs from `firstJob` on hold
size_t drawsSince(size_t firstJob)
{
    size_t draws = 0;
    const uint32_t first = NVB197_CALL_MME_MACRO(0);
    for (size_t job = firstJob; job < kernel_model::model.jobs.size(); job++) {
        const std::vector<uint32_t> &words = kernel_model::model.jobs[job].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        for (long i = 0; i < count; i++)
            draws += !methods[i].subchannel && methods[i].method >= first && !((methods[i].method - first) % 8) &&
                     gm20b::mme::countedAsDraw((methods[i].method - first) / 8);
    }
    return draws;
}

// A state of no depth drawn in a pass with a depth attachment: Apple's GPU draws it against the pass's attachment, depth
// tested and written (an M4 Max with validation off, every pairing of states of no depth, Depth16Unorm, Depth32Float,
// Depth32Float_Stencil8 and Stencil8 with passes of none, Depth32Float and Depth32Float_Stencil8), and so does the driver
void stateOfAnotherDepthFormat()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    for (MTLPixelFormat format : {MTLPixelFormatDepth32Float, MTLPixelFormatDepth32Float_Stencil8}) {
        MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
        p.depthAttachment.texture = target(o.device, format);
        p.depthAttachment.loadAction = MTLLoadActionClear;
        p.depthAttachment.storeAction = MTLStoreActionDontCare;
        const size_t firstJob = kernel_model::model.jobs.size();
        NSDictionary *before = counters(o.device);
        id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
        drawInto(cb, p, o.state);
        expectCompleted(o.device, before, cb);
        EXPECT_EQ(drawsSince(firstJob), 1u) << format;
    }
}

// The same offset set twice in one encoder, which Metal's validation stops ("Using the same offset (8) in the
// visibility buffer twice in the same command encoder is not allowed."): the recording takes both, the encode fails it
void visibilityOffsetSetTwice()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
    p.visibilityResultBuffer = [o.device newBufferWithLength:64 options:0];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    for (MTLVisibilityResultMode mode : {MTLVisibilityResultModeCounting, MTLVisibilityResultModeBoolean}) {
        [e setVisibilityResultMode:mode offset:8];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [e endEncoding];
    expectError(o.device, before, cb, "call.vis", MTLCommandBufferErrorInternal);
}

// Metal's validation stops a plain event of another device ("event is associated with a different device."): the
// command buffer fails at its recording, its draw submitted nowhere
void eventOfAnotherDevice()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    for (const bool signal : {true, false}) {
        NSDictionary *before = counters(o.device);
        id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
        drawInto(cb, pass(target(o.device, kPacked)), o.state);
        if (signal)
            [cb encodeSignalEvent:[ForeignEvent new] value:1];
        else
            [cb encodeWaitForEvent:[ForeignEvent new] value:1];
        expectError(o.device, before, cb, "event", MTLCommandBufferErrorInternal);
    }
}

// One command buffer completes first, so the channel is made before the kernel refuses the next submission
void drawnOnce(id<MTLCommandQueue> queue, const Opened &o)
{
    id<MTLCommandBuffer> first = [queue commandBuffer];
    drawInto(first, pass(target(o.device, kPacked)), o.state);
    [first commit];
    [first waitUntilCompleted];
    ASSERT_EQ(first.status, MTLCommandBufferStatusCompleted) << text(first.error);
}

// The kernel has no control memory for the device's visibility report page, made at the first mode set
void visibilityWithNoReportPage()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
    p.visibilityResultBuffer = [o.device newBufferWithLength:64 options:0];
    kernel_model::model.refuseControl = true;
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];
    [e endEncoding];
    expectError(o.device, before, cb, "call.vis", MTLCommandBufferErrorOutOfMemory);
}

void kernelRefusesTheSubmission()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    kernel_model::model.refuse = 1;
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked)), o.state);
    expectFailed(o.device, before, cb, "enc.kernel", MTLCommandBufferErrorInternal);
    EXPECT_NE(text(cb.error).find("the kernel refused its job (0x"), std::string::npos) << text(cb.error);
}

void kernelRefusesTheChannel()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (!kernel_model::model.slots[s].uid)
            kernel_model::model.slots[s].foreign = true;
    kernel_model::model.foreignTakesFreedSlot = true;
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (kernel_model::model.slots[s].uid && !kernel_model::model.slots[s].foreign)
            kernel_model::model.kill(s + 1, 0x1f);
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked)), o.state);
    expectFailed(o.device, before, cb, "enc.nochannel", MTLCommandBufferErrorInternal);
}

// The submission after the model holds this many jobs is refused, once; each job completes as the model takes it
size_t gRefuseAfterJobs = 0;

void completeAndRefuseAfter()
{
    kernel_model::Model &m = kernel_model::model;
    m.fence(m.jobs.back().channel - 1).completed = m.jobs.back().seqno;
    if (m.jobs.size() == gRefuseAfterJobs)
        m.refuse = 1;
}

// A pass of a batch's draws (batch.h) into `first`, then a pass of one draw into `second`: a job after the first's
id<MTLCommandBuffer> twoJobs(id<MTLCommandQueue> queue, const Opened &o, id<MTLTexture> first, id<MTLTexture> second)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(first)];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    for (uint32_t i = 0; i < gm20b::BatchWork::kOperations; i++)
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    drawInto(cb, pass(second), o.state);
    return cb;
}

MTLRenderPassDescriptor *loading(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = pass(t);
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    return p;
}

// The kernel refuses the second job once the first ran: the command buffer fails once and its range counts as not run,
// so neither new target is marked written and each, its clear given back, is written to its first contents ahead of its
// next use
void kernelRefusesTheSecondJob()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    id<MTLTexture> first = target(o.device, kPacked), second = target(o.device, kPacked);
    NSDictionary *counted = counters(o.device);
    id<MTLCommandBuffer> cb = twoJobs(queue, o, first, second);
    __block int completions = 0;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        completions++;
    }];
    kernel_model::Model &m = kernel_model::model;
    const size_t jobs = m.jobs.size();
    gRefuseAfterJobs = jobs + 1;
    m.jobAdded = completeAndRefuseAfter;
    expectFailed(o.device, counted, cb, "enc.kernel", MTLCommandBufferErrorInternal);
    m.jobAdded = nullptr;
    EXPECT_EQ(m.jobs.size(), jobs + 1) << "not a command buffer whose first job ran";
    EXPECT_EQ(m.refuse, 0u) << "no second job was submitted to refuse";
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(((SwitchMetalTexture *)first)->_gpuDirty.load());
    EXPECT_FALSE(((SwitchMetalTexture *)second)->_gpuDirty.load());
    NSDictionary *before = ask(o.device, "switchMetalFresh");
    ASSERT_NE(before[@"filled"], nil);
    counted = counters(o.device);
    id<MTLCommandBuffer> next = [queue commandBuffer];
    drawInto(next, loading(first), o.state);
    drawInto(next, loading(second), o.state);
    expectCompleted(o.device, counted, next);
    NSDictionary *after = ask(o.device, "switchMetalFresh");
    EXPECT_EQ([after[@"filled"] unsignedLongLongValue] - [before[@"filled"] unsignedLongLongValue], 2u);
}

id nothing()
{
    return nil;
}

// Calls Metal's validation stops that an M4 Max's driver crashes on or fails (c38-audit/failed-counts/probe), each of
// this device's own objects or of none: the command buffer fails InvalidResource as it is recorded
void blitsAnM4MaxCrashesOnFail()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> a = [o.device newBufferWithLength:4096 options:0];
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModeMemoryless;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> memoryless = [o.device newTextureWithDescriptor:d];
    d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:48 height:48 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> large = target(o.device, kPacked), small = [o.device newTextureWithDescriptor:d];
    ASSERT_TRUE(memoryless != nil && small != nil);

    const struct {
        const char *pred;
        void (^encode)(id<MTLBlitCommandEncoder>);
    } calls[] = {{"call.buf", ^(id<MTLBlitCommandEncoder> b){
                      [b fillBuffer:nothing() range:NSMakeRange(0, 64) value:7];
}
}

, {"call.buf", ^(id<MTLBlitCommandEncoder> b){
       [b copyFromBuffer:nothing() sourceOffset:0 toBuffer:a destinationOffset:0 size:64];
}
}
, {"tex.foreign", ^(id<MTLBlitCommandEncoder> b){
       [b copyFromTexture:nothing()
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(16, 16, 1)
                   toTexture:large
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
}
}
, {"call.blit", ^(id<MTLBlitCommandEncoder> b){
       [b copyFromTexture:memoryless
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(16, 16, 1)
                   toTexture:large
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
}
}
, {"call.blit", ^(id<MTLBlitCommandEncoder> b){
       [b copyFromTexture:large toTexture:small];
}
}
, {"call.counter", ^(id<MTLBlitCommandEncoder> b){
       [b sampleCountersInBuffer:nothing() atSampleIndex:0 withBarrier:YES];
}
}
,
}
;
for (const auto &c : calls) {
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    c.encode(b);
    [b endEncoding];
    expectError(o.device, before, cb, c.pred, MTLCommandBufferErrorInvalidResource);
}
}

// A copy out of a level past its texture, which an M4 Max copies to no rule: nothing recorded, skipped as call.blit;
// one into a region past its texture's level writes the part inside, as an M4 Max does, skipped as call.blit too
void blitsAnM4MaxRunsToNoRuleOrInPartAreSkipped()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLTexture> from = target(o.device, kPacked), into = target(o.device, kPacked);
    id<MTLBuffer> rows = [o.device newBufferWithLength:65536 options:0];
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> past = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [past blitCommandEncoder];
    [b copyFromTexture:from
              sourceSlice:0
              sourceLevel:3
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(8, 8, 1)
                toTexture:into
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    expectSkipped(o.device, before, past, "call.blit");
    const size_t firstJob = kernel_model::model.jobs.size();
    before = counters(o.device);
    id<MTLCommandBuffer> cut = [[o.device newCommandQueue] commandBuffer];
    b = [cut blitCommandEncoder];
    [b copyFromBuffer:rows
               sourceOffset:0
          sourceBytesPerRow:256
        sourceBytesPerImage:65536
                 sourceSize:MTLSizeMake(16, 16, 1)
                  toTexture:into
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(56, 0, 0)];
    [b endEncoding];
    expectSkipped(o.device, before, cut, "call.blit");
    EXPECT_GT(kernel_model::model.jobs.size(), firstJob) << "the part inside the level was copied";
}

// An indirect command's indices at an offset off their type, which an M4 Max reads from the offset rounded down to it:
// the command takes that offset, counted skipped as call.icb at each call
void indicesOffTheirTypeRoundDown()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeDrawIndexed;
    d.inheritPipelineState = YES;
    d.inheritBuffers = YES;
    id<MTLIndirectCommandBuffer> icb = [o.device newIndirectCommandBufferWithDescriptor:d maxCommandCount:2 options:0];
    id<MTLBuffer> indices = [o.device newBufferWithLength:64 options:0];
    ASSERT_TRUE(icb != nil && indices != nil);
    NSDictionary *before = counters(o.device);

    const struct {
        MTLIndexType type;
        NSUInteger offset, read;
    } commands[] = {{MTLIndexTypeUInt32, 6, 4}, {MTLIndexTypeUInt16, 3, 2}};

    for (NSUInteger i = 0; i < 2; i++)
        [[icb indirectRenderCommandAtIndex:i] drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                                         indexCount:3
                                                          indexType:commands[i].type
                                                        indexBuffer:indices
                                                  indexBufferOffset:commands[i].offset
                                                      instanceCount:1
                                                         baseVertex:0
                                                       baseInstance:0];
    const std::map<std::string, long long> each = {{"skipped", 2}, {"skipped.call.icb", 2}};
    EXPECT_EQ(skippedMoved(before, counters(o.device)), each) << text(counters(o.device));
    const std::vector<SwitchMetalICBCommand> &taken = ((SwitchMetalIndirectCommandBuffer *)icb)->_commands;
    for (NSUInteger i = 0; i < 2; i++) {
        EXPECT_EQ(taken[i].kind, SwitchMetalICBCommand::Indexed) << i;
        EXPECT_EQ(taken[i].indexOffset, commands[i].read) << i;
    }
}

// An indirect command buffer of four triangles drawn with the encoder's state, and a 64-byte buffer for its range
struct Executed {
    id<MTLIndirectCommandBuffer> icb;
    id<MTLBuffer> range;
};

Executed fourTriangles(id<MTLDevice> device)
{
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeDraw;
    d.inheritPipelineState = YES;
    d.inheritBuffers = YES;
    id<MTLIndirectCommandBuffer> icb = [device newIndirectCommandBufferWithDescriptor:d maxCommandCount:4 options:0];
    for (NSUInteger i = 0; i < 4; i++)
        [[icb indirectRenderCommandAtIndex:i] drawPrimitives:MTLPrimitiveTypeTriangle
                                                 vertexStart:0
                                                 vertexCount:3
                                               instanceCount:1
                                                baseInstance:0];
    return {icb, [device newBufferWithLength:64 options:0]};
}

// A render execute whose range lies past its buffer, which an M4 Max's GPU reads from whatever lies past it: nothing
// recorded, each call skipped as call.icb once (one off 4 bytes and past too), and the encoder's draw after them drawn
void executeRangePastItsBufferRunsNone()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    const Executed x = fourTriangles(o.device);
    ASSERT_TRUE(x.icb != nil && x.range != nil);
    const size_t firstJob = kernel_model::model.jobs.size();
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(target(o.device, kPacked))];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    for (NSUInteger offset : {60, 62, 64, 1 << 20}) {
        [e executeCommandsInBuffer:x.icb indirectBuffer:x.range indirectBufferOffset:offset];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded) << offset;
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    expectRan(o.device, before, cb, {{"skipped", 4}, {"skipped.call.icb", 4}});
    EXPECT_EQ(drawsSince(firstJob), 1u);
}

// A render execute whose range offset is off 4 bytes, which an M4 Max's GPU reads from the offset rounded down: the
// execute recorded at that offset (the plan's draw keeps it as `first`, semantic_plan.cpp), skipped as call.icb, and
// its command buffer completed
void executeRangeOffFourBytesReadsFromItRoundedDown()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    const Executed x = fourTriangles(o.device);
    ASSERT_TRUE(x.icb != nil && x.range != nil);
    NSDictionary *before = counters(o.device);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(target(o.device, kPacked))];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    const SwitchMetalCommandPlan &plan = ((SwitchMetalCommandBuffer *)cb)->_plan;
    const size_t recorded = plan.size();
    [e executeCommandsInBuffer:x.icb indirectBuffer:x.range indirectBufferOffset:58];
    size_t executes = 0;
    for (size_t i = recorded; i < plan.size(); i++)
        if (plan.operation(i).op == OpExecuteCommands) {
            executes++;
            EXPECT_EQ(plan.draw(i).first, 56u);
        }
    EXPECT_EQ(executes, 1u);
    [e endEncoding];
    expectSkipped(o.device, before, cb, "call.icb");
}

// The kernel killing the channel for an MMU fault (TegraGM20BAbiFence error 1) with a command buffer in flight: it fails
// with the code, description and underlying error an M4 Max's GPU address fault carries
void anMmuFaultInFlightIsAPageFault()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    // the completion thread's wait held until the kill, so it meets the kill and no wait it took as refused
    static std::mutex held;
    static std::condition_variable changed;
    static bool killed = false;
    kernel_model::model.blockUntil = [](uint32_t, uint32_t) {
        std::unique_lock<std::mutex> l(held);
        changed.wait(l, [] {
            return killed;
        });
        return true;
    };
    kernel_model::model.inFlight = true;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked)), o.state);
    [cb commit];
    [cb waitUntilScheduled];
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        for (uint32_t s = 0; s < kernel_model::kSlots; s++)
            if (kernel_model::model.slots[s].uid && !kernel_model::model.slots[s].foreign)
                kernel_model::model.kill(s + 1, 1);
    }
    {
        std::lock_guard<std::mutex> l(held);
        killed = true;
    }
    changed.notify_all();
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorPageFault) << text(cb.error);
    EXPECT_EQ(text(cb.error.localizedDescription),
              "Caused GPU Address Fault Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)");
    NSError *beneath = cb.error.userInfo[NSUnderlyingErrorKey];
    EXPECT_EQ(text(beneath.domain), "IOGPUCommandQueueErrorDomain");
    EXPECT_EQ(beneath.code, 11);
}

}  // namespace

TEST(GM20BFacadeErrors, ARenderExecutesRangePastItsBufferRunsNoneAndTheEncoderGoesOn)
{
    switch_ios::test::ExpectPassesInFreshProcess(executeRangePastItsBufferRunsNone);
}

TEST(GM20BFacadeErrors, ARenderExecutesRangeOffFourBytesIsReadFromTheOffsetRoundedDown)
{
    switch_ios::test::ExpectPassesInFreshProcess(executeRangeOffFourBytesReadsFromItRoundedDown);
}

TEST(GM20BFacadeErrors, AnMmuFaultInFlightFailsItsCommandBufferAsMetalReportsAPageFault)
{
    switch_ios::test::ExpectPassesInFreshProcess(anMmuFaultInFlightIsAPageFault);
}

TEST(GM20BFacadeErrors, IndicesOfAnIndirectCommandOffTheirTypeAreReadFromTheOffsetRoundedDown)
{
    switch_ios::test::ExpectPassesInFreshProcess(indicesOffTheirTypeRoundDown);
}

TEST(GM20BFacadeErrors, CallsAnM4MaxCrashesOnFailInvalidResource)
{
    switch_ios::test::ExpectPassesInFreshProcess(blitsAnM4MaxCrashesOnFail);
}

TEST(GM20BFacadeErrors, CallsAnM4MaxRunsToNoRuleOrInPartAreSkipped)
{
    switch_ios::test::ExpectPassesInFreshProcess(blitsAnM4MaxRunsToNoRuleOrInPartAreSkipped);
}

TEST(GM20BFacadeErrors, ABufferCopyPastItsBufferCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferCopyPastItsBuffer);
}

TEST(GM20BFacadeErrors, ABufferFillPastItsBufferCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferFillPastItsBuffer);
}

TEST(GM20BFacadeErrors, IndirectArgumentsPastTheirBufferComplete)
{
    switch_ios::test::ExpectPassesInFreshProcess(indirectArgumentsPastTheirBuffer);
}

TEST(GM20BFacadeErrors, AStateOfTheOtherChannelOrderCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateOfTheOtherChannelOrder);
}

TEST(GM20BFacadeErrors, AStateOfAnotherColourFormatCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateOfAnotherColourFormat);
}

TEST(GM20BFacadeErrors, AStateOfOtherSamplesCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateSamplesNotThePasss);
}

TEST(GM20BFacadeErrors, SamplePositionsPastThePasssSamplesComplete)
{
    switch_ios::test::ExpectPassesInFreshProcess(samplePositionsPastThePasssSamples);
}

TEST(GM20BFacadeErrors, VisibilityModesMetalStopsFail)
{
    switch_ios::test::ExpectPassesInFreshProcess(visibilityModesMetalStops);
}

TEST(GM20BFacadeErrors, AVisibilityBufferOfAnotherDeviceFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(visibilityBufferOfAnotherDevice);
}

TEST(GM20BFacadeErrors, AStateOfAnotherDepthFormatDrawsAgainstThePasssAttachment)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateOfAnotherDepthFormat);
}

TEST(GM20BFacadeErrors, AVisibilityOffsetSetTwiceInAnEncoderFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(visibilityOffsetSetTwice);
}

TEST(GM20BFacadeErrors, AVisibilityModeWithNoReportPageFailsOutOfMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(visibilityWithNoReportPage);
}

TEST(GM20BFacadeErrors, AnEventOfAnotherDeviceFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(eventOfAnotherDevice);
}

TEST(GM20BFacadeErrors, TheKernelRefusingTheSubmissionFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheSubmission);
}

TEST(GM20BFacadeErrors, TheKernelRefusingTheChannelFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheChannel);
}

TEST(GM20BFacadeErrors, TheKernelRefusingTheSecondJobLeavesTheRangeNotRun)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheSecondJob);
}
