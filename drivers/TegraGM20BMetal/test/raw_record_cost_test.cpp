// SPDX-License-Identifier: MIT
// What recording costs the thread that records, through the raw facade over the kernel model: a compositor scale frame
// (boot 21: 15 passes, 30 render target switches, 90 draws, 55 textures of other processes) and its parts, each draw
// setting its vertices, transform and colour by setBytes, a texture and sampler by Core Animation's call and a scissor
// of its own; the same frame at twice the draws; encoders with no draws; draws with no state set between them. Per
// scenario: the recording thread's time, instructions and cycles, and its Objective-C messages, locks and allocations
// (record_count.c). The counts are the test; times are printed.
#include "raw_facade_harness.h"
#include "raw_job_words.h"

#include <gtest/gtest.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>

extern "C" {
struct record_count {
    uint64_t sends, locks, mallocs, frees, weaks, retains;
};

void record_count_start(void);
void record_count_stop(struct record_count *out);
int thread_selfcounts(int type, void *buf, size_t nbytes);
}

@protocol RecordCostCoreAnimationSPI
- (void)setFragmentTexture:(id<MTLTexture>)texture
            atTextureIndex:(NSUInteger)textureIndex
              samplerState:(id<MTLSamplerState>)sampler
            atSamplerIndex:(NSUInteger)samplerIndex;
@end

namespace {

using namespace harness;

constexpr int kEncoders = 30, kTargets = 15, kTextures = 55, kDraws = 90;
constexpr int kWarm = 8, kRounds = 40;

struct Sample {
    double us = 0, instructions = 0, cycles = 0;
    record_count counts{};
};

struct Bench {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> state;
    std::vector<id<MTLTexture>> targets, textures;
    id<MTLSamplerState> sampler;
};

std::vector<raw::PackEntry> compositorEntries()
{
    raw::PackEntry texture = {};
    texture.offset = 0;
    texture.kind = raw::EntryTexture;
    texture.stage = 2;
    texture.slot = 0;
    texture.samplerKind = raw::SamplerSlot;
    texture.sampler = 0;
    raw::PackEntry transform = {};
    transform.offset = 16;
    transform.kind = raw::EntryUbo;
    transform.stage = 1;
    transform.slot = 2;
    raw::PackEntry colour = {};
    colour.offset = 32;
    colour.kind = raw::EntryUbo;
    colour.stage = 2;
    colour.slot = 0;
    return {texture, transform, colour};
}

id<MTLTexture> texture(id<MTLDevice> device, uint32_t width, uint32_t height, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

Bench &bench()
{
    static Bench b = [] {
        Bench made;
        const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, compositorEntries());
        const std::string metallib = writeMetallib();
        made.device = open(pack);
        if (!made.device)
            return made;
        NSError *error = nil;
        id<MTLLibrary> library =
            [made.device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
        made.state = [made.device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm)
                                                                 error:&error];
        made.queue = [made.device newCommandQueue];
        for (int i = 0; i < kTargets; i++)
            made.targets.push_back(
                texture(made.device, 320, 180, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead));
        for (int i = 0; i < kTextures; i++)
            made.textures.push_back(texture(made.device, 64, 64, MTLTextureUsageShaderRead));
        made.sampler = [made.device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        return made;
    }();
    return b;
}

// The frame's pass descriptors, made before the recording is timed: Core Animation makes its own
std::vector<MTLRenderPassDescriptor *> passes(const Bench &b, int count)
{
    std::vector<MTLRenderPassDescriptor *> made;
    for (int p = 0; p < count; p++) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = b.targets[size_t(p) % b.targets.size()];
        d.colorAttachments[0].loadAction = p >= kTargets ? MTLLoadActionLoad : MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        made.push_back(d);
    }
    return made;
}

void compositorDraw(id<MTLRenderCommandEncoder> e, const Bench &b, int draw, uint32_t frame)
{
    const float x = float((draw * 37 + frame) % 97) / 97.0f - 0.5f, y = float(draw * 13 % 89) / 89.0f;
    const float vertices[16] = {x, y, 0, 1, x + 0.25f, y, 0, 1, x, y + 0.25f, 0, 1, x + 0.25f, y + 0.25f, 0, 1};
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(frame % 7) * 0.01f, 0, 0, 1};
    const float colour[4] = {float(draw % 5) / 5, 0.5f, 0.5f, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e setVertexBytes:transform length:sizeof transform atIndex:2];
    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
    [(id<RecordCostCoreAnimationSPI>)e setFragmentTexture:b.textures[size_t(draw) % b.textures.size()]
                                           atTextureIndex:0
                                             samplerState:b.sampler
                                           atSamplerIndex:0];
    [e setScissorRect:MTLScissorRect{NSUInteger(draw * 17 % 120), NSUInteger(draw * 11 % 60), 200, 120}];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void compositorFrame(id<MTLCommandBuffer> cb, const Bench &b, const std::vector<MTLRenderPassDescriptor *> &into,
                     int draws, uint32_t frame)
{
    int drawn = 0;
    for (int p = 0; p < kEncoders; p++) {
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[size_t(p)]];
        [e setRenderPipelineState:b.state];
        const int mine = draws / kEncoders + (p < draws % kEncoders ? 1 : 0);
        for (int i = 0; i < mine; i++)
            compositorDraw(e, b, drawn++, frame);
        [e endEncoding];
    }
}

void emptyEncoders(id<MTLCommandBuffer> cb, const Bench &b, const std::vector<MTLRenderPassDescriptor *> &into)
{
    for (int p = 0; p < kEncoders; p++) {
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[size_t(p)]];
        [e setRenderPipelineState:b.state];
        [e endEncoding];
    }
}

void repeatedDraws(id<MTLCommandBuffer> cb, const Bench &b, const std::vector<MTLRenderPassDescriptor *> &into,
                   int repeats)
{
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[0]];
    [e setRenderPipelineState:b.state];
    compositorDraw(e, b, 0, 0);
    for (int i = 0; i < repeats; i++)
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
}

// The calling thread's instructions and cycles retired
struct Counts {
    uint64_t instructions = 0, cycles = 0;
};

Counts counts()
{
    Counts c;
    uint64_t words[2] = {};
    if (!thread_selfcounts(1, words, sizeof words))
        c = {words[0], words[1]};
    return c;
}

double ticksToUs(uint64_t ticks)
{
    static const double perTick = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return double(tb.numer) / double(tb.denom) / 1000.0;
    }();
    return double(ticks) * perTick;
}

using Record = std::function<void(id<MTLCommandBuffer>, const std::vector<MTLRenderPassDescriptor *> &, uint32_t)>;

// The median round of `record` (each into a command buffer of its own, committed and completed outside the window, and
// its pass descriptors made before it)
Sample measure(const Record &record)
{
    Bench &b = bench();
    std::vector<Sample> samples;
    for (int round = 0; round < kWarm + kRounds; round++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders);
            Sample s;
            const Counts before = counts();
            record_count_start();
            const uint64_t t = mach_absolute_time();
            record(cb, into, uint32_t(round));
            const uint64_t ticks = mach_absolute_time() - t;
            record_count_stop(&s.counts);
            const Counts after = counts();
            s.us = ticksToUs(ticks);
            s.instructions = double(after.instructions - before.instructions);
            s.cycles = double(after.cycles - before.cycles);
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            if (round >= kWarm)
                samples.push_back(s);
        }
    std::sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &c) {
        return a.us < c.us;
    });
    return samples[samples.size() / 2];
}

void print(const char *name, const Sample &s, double per, const char *unit)
{
    printf("record cost %-22s %8.1f us %10.0f instr %10.0f cycles | sends %6llu locks %4llu mallocs %4llu frees %4llu "
           "weaks %4llu retains %5llu | per %s: %.3f us %.0f instr %.2f sends %.2f locks %.2f mallocs\n",
           name, s.us, s.instructions, s.cycles, (unsigned long long)s.counts.sends, (unsigned long long)s.counts.locks,
           (unsigned long long)s.counts.mallocs, (unsigned long long)s.counts.frees, (unsigned long long)s.counts.weaks,
           (unsigned long long)s.counts.retains, unit, s.us / per, s.instructions / per, double(s.counts.sends) / per,
           double(s.counts.locks) / per, double(s.counts.mallocs) / per);
}

Sample difference(const Sample &a, const Sample &b)
{
    Sample d;
    d.us = a.us - b.us;
    d.instructions = a.instructions - b.instructions;
    d.cycles = a.cycles - b.cycles;
    d.counts = {a.counts.sends - b.counts.sends, a.counts.locks - b.counts.locks, a.counts.mallocs - b.counts.mallocs,
                a.counts.frees - b.counts.frees, a.counts.weaks - b.counts.weaks, a.counts.retains - b.counts.retains};
    return d;
}

TEST(RawRecordCost, CompositorScaleFrame)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    using Passes = const std::vector<MTLRenderPassDescriptor *>;
    const Sample frame = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t f) {
        compositorFrame(cb, b, into, kDraws, f);
    });
    const Sample doubled = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t f) {
        compositorFrame(cb, b, into, 2 * kDraws, f);
    });
    const Sample encoders = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t) {
        emptyEncoders(cb, b, into);
    });
    // both past the encode-ahead hand-off (RawMetalPipeline.mm kHandOff, 24 commands), which is once a command buffer
    const Sample one = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t) {
        repeatedDraws(cb, b, into, 32);
    });
    const Sample repeated = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t) {
        repeatedDraws(cb, b, into, 332);
    });
    print("frame 30 enc 90 draws", frame, 1, "frame");
    print("changed-state draw", difference(doubled, frame), kDraws, "draw");
    print("empty encoder", encoders, kEncoders, "encoder");
    print("unchanged-state draw", difference(repeated, one), 300, "draw");
    // the API calls' own messages alone: 6 a compositor draw, 1 a draw with nothing set before it, 3 an encoder's
    // begin, state and end (plus the pass copy's own at a new shape), and no allocation once the recording is warm
    EXPECT_EQ(difference(doubled, frame).counts.sends, uint64_t(6 * kDraws));
    EXPECT_EQ(difference(repeated, one).counts.sends, 300u);
    EXPECT_LE(encoders.counts.sends, uint64_t(7 * kEncoders));
    EXPECT_EQ(frame.counts.mallocs, 0u);
    // an encoder of a kept pass shape takes no lock: the pass cache is its thread's alone (rmPassTrim stops the thread)
    EXPECT_EQ(encoders.counts.locks, 0u);
    // no weak reference (each a side-table lock), and the binding path's retains gone: two a draw at most (the first
    // bind of a texture in a command buffer keeps it, and its release at completion)
    EXPECT_EQ(frame.counts.weaks, 0u);
    // the device's lock (imports, creates, backings) and every other lock stay off the recording of a frame whose
    // resources exist
    EXPECT_EQ(frame.counts.locks, 0u);
    EXPECT_LE(difference(doubled, frame).counts.retains, uint64_t(2 * kDraws));
    EXPECT_EQ(difference(repeated, one).counts.retains, 0u);
}

// Each call of a compositor draw, its instructions apart (less what two counter reads cost), over a frame's 90 draws
TEST(RawRecordCost, ChangedStateDrawByCall)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);

    enum Call {
        VertexBytes,
        TransformBytes,
        ColourBytes,
        TextureSampler,
        Scissor,
        Draw,
        Calls
    };

    const char *names[Calls] = {"setVertexBytes vertices", "setVertexBytes transform", "setFragmentBytes colour",
                                "texture and sampler",     "setScissorRect",           "drawPrimitives"};
    double sums[Calls] = {};
    double calibration = 1e30;
    for (int i = 0; i < 64; i++) {
        const Counts a = counts(), c = counts();
        calibration = std::min(calibration, double(c.instructions - a.instructions));
    }
    uint32_t draws = 0;
    for (int round = 0; round < kWarm + kRounds; round++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders);
            int drawn = 0;
            for (int p = 0; p < kEncoders; p++) {
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[size_t(p)]];
                [e setRenderPipelineState:b.state];
                for (int d = 0; d < kDraws / kEncoders; d++, drawn++) {
                    const float x = float(drawn % 97) / 97.0f, vertices[16] = {x, 0, 0, 1, x, 1, 0, 1};
                    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(round % 7), 0, 0, 1};
                    const float colour[4] = {float(drawn % 5) / 5, 0.5f, 0.5f, 1};
                    Counts at[Calls + 1];
                    at[0] = counts();
                    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
                    at[1] = counts();
                    [e setVertexBytes:transform length:sizeof transform atIndex:2];
                    at[2] = counts();
                    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
                    at[3] = counts();
                    [(id<RecordCostCoreAnimationSPI>)e setFragmentTexture:b.textures[size_t(drawn) % b.textures.size()]
                                                           atTextureIndex:0
                                                             samplerState:b.sampler
                                                           atSamplerIndex:0];
                    at[4] = counts();
                    [e setScissorRect:MTLScissorRect{NSUInteger(drawn * 17 % 120), 0, 200, 120}];
                    at[5] = counts();
                    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    at[6] = counts();
                    if (round >= kWarm) {
                        for (int k = 0; k < Calls; k++)
                            sums[k] += double(at[k + 1].instructions - at[k].instructions) - calibration;
                        draws += 1;
                    }
                }
                [e endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
        }
    double total = 0;
    for (int k = 0; k < Calls; k++) {
        printf("record cost by call %-26s %8.0f instr a draw\n", names[k], sums[k] / draws);
        total += sums[k] / draws;
    }
    printf("record cost by call %-26s %8.0f instr a draw\n", "all", total);
    EXPECT_GT(draws, 0u);
    // Core Animation's texture and sampler with the sampler bound already, and a draw whose bindings leave its check as
    // it was (measured 745 and 1,924 a draw in a release build, 926 and 2,023 in a debug one; all calls 3,959 and 4,666)
    EXPECT_LT(sums[TextureSampler] / draws, 1100);
    EXPECT_LT(sums[Draw] / draws, 2300);
    EXPECT_LT(total, 5200);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The jobs of compositor frames, each with an encoder of indexed draws after them (the first of a new state, the rest of
// the same one), as raw_job_words.h's text: what a change of the recording path must leave as it is
TEST(RawRecordCost, CompositorFrameWords)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
    id<MTLBuffer> indices = [b.device newBufferWithBytes:quad length:sizeof quad options:MTLResourceStorageModeShared];
    std::vector<job_words::Run> runs;
    const uint64_t mark = job_words::handleMark();
    for (uint32_t frame = 0; frame < 3; frame++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders + 1);
            compositorFrame(cb, b, into, kDraws, frame);
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[kEncoders]];
            [e setRenderPipelineState:b.state];
            compositorDraw(e, b, 0, frame);
            for (int i = 0; i < 4; i++)
                [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                              indexCount:6
                               indexType:MTLIndexTypeUInt16
                             indexBuffer:indices
                       indexBufferOffset:0];
            [e endEncoding];
            runs.push_back(job_words::run(cb));
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            EXPECT_EQ(std::string([[(id)cb performSelector:@selector(switchRawPath)] UTF8String]), "raw");
        }
    const std::string words = job_words::words(runs, mark, {{"indices", indices.gpuAddress, sizeof quad}});
    const char *outputs = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    std::ofstream(std::string(outputs ? outputs : harness::dir().c_str()) + "/compositor_words.txt") << words;
    printf("compositor words: %zu bytes, hash %016llx\n", words.size(), (unsigned long long)job_words::fnv(words));
    EXPECT_FALSE(words.empty());
}
#endif

}  // namespace
