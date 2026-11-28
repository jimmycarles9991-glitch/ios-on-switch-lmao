// SPDX-License-Identifier: MIT
// What recording costs the thread that records, through the facade over the kernel model: a compositor scale frame
// (boot 21: 15 passes, 30 render target switches, 90 draws, 55 textures of other processes) and its parts, each draw
// setting its vertices, transform and colour by setBytes, a texture and sampler by Core Animation's call and a scissor
// of its own; the same frame at twice the draws; encoders with no draws; draws with no state set between them. Per
// scenario: the recording thread's time, instructions and cycles, and its Objective-C messages, locks, allocations and
// dispatches (record_count.c). The counts are the test; times are printed.
#include "facade_harness.h"
#include "job_words.h"

#include <gtest/gtest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <time.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>

extern "C" {
struct record_count {
    uint64_t sends, locks, mallocs, frees, weaks, retains, dispatches;
};

void record_count_start(void);
void record_count_stop(struct record_count *out);
const uintptr_t *record_count_retain_sites(size_t *count);
int thread_selfcounts(int type, void *buf, size_t nbytes);
}

@protocol RecordCostChunkStats <NSObject>
+ (NSDictionary<NSString *, NSNumber *> *)switchMetalBytesChunkStats;  // TegraGM20BMetalRecordings.mm (debug builds)
@end

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
    std::map<uintptr_t, long> retainSites;  // each retain's and release's caller (a release's top bit set): how many
};

struct Bench {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> state;
    std::vector<id<MTLTexture>> targets, textures;
    id<MTLSamplerState> sampler;
};

std::vector<gm20b::PackEntry> compositorEntries()
{
    gm20b::PackEntry texture = {};
    texture.offset = 0;
    texture.kind = gm20b::EntryTexture;
    texture.stage = 2;
    texture.slot = 0;
    texture.samplerKind = gm20b::SamplerSlot;
    texture.sampler = 0;
    gm20b::PackEntry transform = {};
    transform.offset = 16;
    transform.kind = gm20b::EntryUbo;
    transform.stage = 1;
    transform.slot = 2;
    gm20b::PackEntry colour = {};
    colour.offset = 32;
    colour.kind = gm20b::EntryUbo;
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
        const std::string pack =
            writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, compositorEntries());
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
            size_t sites = 0;
            const uintptr_t *callers = record_count_retain_sites(&sites);
            for (size_t i = 0; i < sites; i++)
                s.retainSites[callers[i]]++;
            EXPECT_EQ(s.counts.dispatches, 0u) << "round " << round;
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

// The callers whose retains and releases differ between `a` and `b`, by name
void printRetainSites(const char *name, const Sample &a, const Sample &b)
{
    std::map<uintptr_t, long> diff = a.retainSites;
    for (const auto &[caller, n] : b.retainSites)
        diff[caller] -= n;
    for (const auto &[caller, n] : diff) {
        if (!n)
            continue;
        const uintptr_t pc = caller & ~(uintptr_t(1) << 63);
        Dl_info info = {};
        dladdr(reinterpret_cast<void *>(pc), &info);
        printf("record cost %s: %+ld %s %s+0x%lx\n", name, n, caller >> 63 ? "releases" : "retains",
               info.dli_sname ? info.dli_sname : "?", (unsigned long)(pc - uintptr_t(info.dli_saddr)));
    }
}

Sample difference(const Sample &a, const Sample &b)
{
    Sample d;
    d.us = a.us - b.us;
    d.instructions = a.instructions - b.instructions;
    d.cycles = a.cycles - b.cycles;
    d.counts = {a.counts.sends - b.counts.sends,          a.counts.locks - b.counts.locks,
                a.counts.mallocs - b.counts.mallocs,      a.counts.frees - b.counts.frees,
                a.counts.weaks - b.counts.weaks,          a.counts.retains - b.counts.retains,
                a.counts.dispatches - b.counts.dispatches};
    return d;
}

TEST(GM20BRecordCost, CompositorScaleFrame)
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
    // both past the encode-ahead hand-off (TegraGM20BMetalPipeline.mm kHandOff, 24 commands), which is once a command buffer
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
    printRetainSites("changed-state draw retains", doubled, frame);
    printRetainSites("unchanged-state draw retains", repeated, one);
    // the API calls' own messages alone: 6 a compositor draw, 1 a draw with nothing set before it, 3 an encoder of a
    // kept shape's begin, state and end and 1 its release (its block comes from the pool with no message), and no
    // allocation once the recording is warm
    EXPECT_EQ(difference(doubled, frame).counts.sends, uint64_t(6 * kDraws));
    EXPECT_EQ(difference(repeated, one).counts.sends, 300u);
    EXPECT_LE(encoders.counts.sends, uint64_t(4 * kEncoders));
    EXPECT_EQ(frame.counts.mallocs, 0u);
    // an encoder of a kept pass shape takes no lock: the pass cache is its thread's alone (gm20bPassTrim stops the thread)
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

// An encoder whose pass shape the pass cache has not seen (TegraGM20BMetalPassCache.h): its clear colour new each round, so
// every pass is a miss that copies its descriptor and reads every attachment, as a quarter to a half of the render
// server's passes did on 25 Nov (frame line hit= against kept=). Printed beside a kept shape's encoder.
TEST(GM20BRecordCost, AMissedPassShapeAgainstAKeptOne)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    using Passes = const std::vector<MTLRenderPassDescriptor *>;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const auto made = [] {
        const auto &passes = gm20b::frameCpuCounts().passes;
        return passes[size_t(gm20b::PassOpen::Kept)].load() + passes[size_t(gm20b::PassOpen::Unkept)].load();
    };
    const uint64_t madeBefore = made();
#endif
    const Sample hit = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t) {
        emptyEncoders(cb, b, into);
    });
    const Sample missed = measure([&](id<MTLCommandBuffer> cb, Passes &into, uint32_t round) {
        for (int p = 0; p < kEncoders; p++) {
            into[size_t(p)].colorAttachments[0].loadAction = MTLLoadActionClear;
            into[size_t(p)].colorAttachments[0].clearColor =
                MTLClearColorMake(double(round) / 64, double(p) / 64, 0, 1);
        }
        emptyEncoders(cb, b, into);
    });
    print("kept-shape encoder", hit, kEncoders, "encoder");
    print("missed-shape encoder", missed, kEncoders, "encoder");
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    EXPECT_GE(made() - madeBefore, uint64_t(kEncoders) * kRounds);
#endif
}

// Command buffers recorded back to back before any is committed, so the setBytes chunk pool runs dry: the recording
// thread takes chunks or makes its own, and the pool's refill is left to the completions
TEST(GM20BRecordCost, ABurstDispatchesNothingFromTheRecordingThread)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    constexpr int kBurst = 72;  // past the pool's 64 kept chunks
    std::vector<id<MTLCommandBuffer>> recorded;
    uint64_t dispatches = 0;
    for (int f = 0; f < kBurst; f++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders);
            record_count c;
            record_count_start();
            compositorFrame(cb, b, into, 2 * kDraws, uint32_t(f));
            record_count_stop(&c);
            dispatches += c.dispatches;
            recorded.push_back(cb);
        }
    for (id<MTLCommandBuffer> cb : recorded)
        [cb commit];
    [recorded.back() waitUntilCompleted];
    for (id<MTLCommandBuffer> cb : recorded)
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(dispatches, 0u);
}

// A command buffer recorded after the last one completed takes its setBytes chunks from the pool: once the pool has
// grown to what one frame in flight needs, no recording makes its own (debug builds count those misses)
TEST(GM20BRecordCost, SteadyFramesTakeTheirChunksFromThePool)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    Class<RecordCostChunkStats> cls = (Class<RecordCostChunkStats>)NSClassFromString(@"SwitchMetalCommandBuffer");
    if (![cls respondsToSelector:@selector(switchMetalBytesChunkStats)])
        GTEST_SKIP() << "release builds count no misses";
    const auto misses = [&] {
        return [[cls switchMetalBytesChunkStats][@"misses"] unsignedLongLongValue];
    };
    const auto frame = [&](int f) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            compositorFrame(cb, b, passes(b, kEncoders), 2 * kDraws, uint32_t(f));
            [cb commit];
            [cb waitUntilCompleted];
        }
    };
    for (int f = 0; f < kWarm; f++)
        frame(f);
    const unsigned long long before = misses();
    for (int f = kWarm; f < kWarm + kRounds; f++)
        frame(f);
    EXPECT_EQ(misses() - before, 0u);
}

// Each call of a compositor draw, its instructions apart (less what two counter reads cost) over a frame's 90 draws,
// the fewest of any measured round: the thread's counter (thread_selfcounts THSC_CPI) sums its user and kernel
// instructions, so preemption, interrupts and refills on a busy host only ever add to a round
TEST(GM20BRecordCost, ChangedStateDrawByCall)
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
    double calibration = 1e30;
    for (int i = 0; i < 64; i++) {
        const Counts a = counts(), c = counts();
        calibration = std::min(calibration, double(c.instructions - a.instructions));
    }
    constexpr int draws = kEncoders * (kDraws / kEncoders);
    // by call, then all of them: a round's instructions, the fewest of the measured rounds
    double fewest[Calls + 1];
    for (double &f : fewest)
        f = 1e30;
    for (int round = 0; round < kWarm + kRounds; round++)
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders);
            double spent[Calls + 1] = {};
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
                    for (int k = 0; k < Calls; k++) {
                        const double instructions = double(at[k + 1].instructions - at[k].instructions) - calibration;
                        spent[k] += instructions;
                        spent[Calls] += instructions;
                    }
                }
                [e endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            if (round >= kWarm)
                for (int k = 0; k <= Calls; k++)
                    fewest[k] = std::min(fewest[k], spent[k]);
        }
    for (int k = 0; k < Calls; k++)
        printf("record cost by call %-26s %8.0f instr a draw\n", names[k], fewest[k] / draws);
    printf("record cost by call %-26s %8.0f instr a draw\n", "all", fewest[Calls] / draws);
    // Core Animation's texture and sampler with the sampler bound already, and a draw whose bindings leave its check as
    // it was (measured 745 and 1,924 a draw in a release build, 926 and 2,023 in a debug one; all calls 3,959 and 4,666)
    EXPECT_LT(fewest[TextureSampler] / draws, 1100);
    EXPECT_LT(fewest[Draw] / draws, 2300);
    EXPECT_LT(fewest[Calls] / draws, 5200);
}

// The submit worker's render encoder made again for each pass (gm20b::RenderEncoder::remake): its binding tables keep
// their storage and only the slots the last pass set are cleared (measured 2,276 instructions on 24B119's kind, where
// destroying and value-initialising its 19,408 bytes measured 3,667)
TEST(GM20BRecordCost, APassEncoderIsMadeAgainWithoutItsWholeBindingTables)
{
    auto *e = new gm20b::RenderEncoder();
    const auto *sampler = reinterpret_cast<const gm20b::Sampler *>(uintptr_t(0x1000));
    double fewest = 1e30;
    for (int round = 0; round < 8; round++) {
        constexpr int kRemakes = 4000;
        const Counts before = counts();
        for (int i = 0; i < kRemakes; i++) {
            for (uint32_t slot = 0; slot < 4; slot++)
                e->setSampler(2, slot, sampler + slot);
            e->remake();
        }
        fewest = std::min(fewest, double(counts().instructions - before.instructions) / kRemakes);
    }
    delete e;
    printf("record cost pass encoder remake %.0f instr (%zu bytes)\n", fewest, sizeof(gm20b::RenderEncoder));
    EXPECT_LT(fewest, 3000.0);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The frame rates line over frames of kept pass shapes: every pass a hit, each command buffer counted once, and each
// blocked waitUntilScheduled's time split whole into its parts
TEST(GM20BRecordCost, TheFrameLineCountsKeptPassesAndWhereWaitsWent)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    const auto frame = [&](uint32_t f) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            compositorFrame(cb, b, passes(b, kEncoders), kDraws, f);
            [cb commit];
            [cb waitUntilScheduled];
            [cb waitUntilCompleted];
        }
    };
    for (uint32_t f = 0; f < uint32_t(kWarm); f++)
        frame(f);
    char line[1024];
    (void)metal_frame_rates(line, sizeof line);
    constexpr int kFrames = 4;
    for (uint32_t f = 0; f < kFrames; f++)
        frame(f);
    ASSERT_GT(metal_frame_rates(line, sizeof line), 0u);
    unsigned long long opened = 0, hits = 0, kept = 0, unkept = 0, unkeyed = 0, counters = 0, owned = 0;
    ASSERT_EQ(sscanf(line, "frame pass=%llu hit=%llu kept=%llu unkept=%llu unkeyed=%llu counters=%llu owned=%llu",
                     &opened, &hits, &kept, &unkept, &unkeyed, &counters, &owned),
              7)
        << line;
    EXPECT_EQ(opened, unsigned(kFrames * kEncoders)) << line;
    EXPECT_EQ(hits, opened) << line;
    const char *ahead = strstr(line, " ahead=");
    unsigned long long encodedAhead = 0, buffers = 0;
    ASSERT_TRUE(ahead && sscanf(ahead, " ahead=%llu/%llu", &encodedAhead, &buffers) == 2) << line;
    EXPECT_EQ(buffers, unsigned(kFrames)) << line;
    const char *ws = strstr(line, " ws=");
    unsigned long long waits = 0, waited = 0, parts[5] = {};
    ASSERT_TRUE(ws && sscanf(ws, " ws=%llu/%lluus(queued %llu parked %llu encoding %llu handlers %llu other %llu)",
                             &waits, &waited, &parts[0], &parts[1], &parts[2], &parts[3], &parts[4]) == 7)
        << line;
    EXPECT_EQ(parts[0] + parts[1] + parts[2] + parts[3] + parts[4], waited) << line;
}

#if SWITCH_METAL_DIAGNOSTICS
TEST(GM20BRecordCost, TheFrameLineReportsEnqueueTimingAndLateBoundWrites)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    const std::vector<MTLRenderPassDescriptor *> into = passes(b, 1);
    char line[1024];
    (void)metal_frame_rates(line, sizeof line);

    const float vertices[16] = {0, 0, 0, 1, 0.25f, 0, 0, 1, 0, 0.25f, 0, 1, 0.25f, 0.25f, 0, 1};
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float colour[4] = {0.25f, 0.5f, 0.75f, 1};
    id<MTLBuffer> sharedBuf =
        [b.device newBufferWithBytes:transform length:sizeof transform options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:16
                                                                                 height:16
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sharedTex = [b.device newTextureWithDescriptor:td];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @16,
        (id)kIOSurfaceHeight : @16,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @64,
        (id)kIOSurfaceAllocSize : @(16 * 64),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(sharedBuf != nil && sharedTex != nil && surface != nullptr);
    id<MTLTexture> surfaceTex = [b.device newTextureWithDescriptor:td iosurface:surface plane:0];
    ASSERT_NE(surfaceTex, nil);

    const auto recordPass = [&](id<MTLCommandBuffer> cb) {
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[0]];
        [e setRenderPipelineState:b.state];
        [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
        [e setVertexBuffer:sharedBuf offset:0 atIndex:2];
        [e setFragmentBytes:colour length:sizeof colour atIndex:0];
        [(id<RecordCostCoreAnimationSPI>)e setFragmentTexture:sharedTex
                                               atTextureIndex:0
                                                 samplerState:b.sampler
                                               atSamplerIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [(id<RecordCostCoreAnimationSPI>)e setFragmentTexture:surfaceTex
                                               atTextureIndex:0
                                                 samplerState:b.sampler
                                               atSamplerIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    };

    for (int mode = 0; mode < 3; mode++) {
        id<MTLCommandBuffer> cb = [b.queue commandBuffer];
        if (mode == 0)
            [cb enqueue];
        recordPass(cb);
        if (mode == 1)
            [cb enqueue];
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }
    ASSERT_GT(metal_frame_rates(line, sizeof line), 0u);
    const char *enq = strstr(line, " enq=");
    unsigned long long beforeFirst = 0, beforeCommit = 0, never = 0;
    ASSERT_TRUE(enq && sscanf(enq, " enq=%llu/%llu/%llu", &beforeFirst, &beforeCommit, &never) == 3) << line;
    EXPECT_EQ(beforeFirst, 1u) << line;
    EXPECT_EQ(beforeCommit, 1u) << line;
    EXPECT_EQ(never, 1u) << line;
    CFRelease(surface);
}
#endif

// The recording thread prepares an encoder's range only where the submit worker may encode it ahead of the commit, which
// reads it as the tape is written: a command buffer too short to be handed to the worker leaves every range to the
// worker's turn, and one handed has each encoder prepared once
TEST(GM20BRecordCost, OnlyAnEncodeAheadHasTheRecordingThreadPrepareRanges)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    std::atomic<uint64_t> &prepared = gm20b::frameCpuCounts().recordPrepares;
    const auto run = [&](const std::function<void(id<MTLCommandBuffer>)> &record) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [b.queue commandBuffer];
            const uint64_t before = prepared.load();
            record(cb);
            const uint64_t made = prepared.load() - before;
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            EXPECT_EQ(prepared.load() - before, made) << "the commit prepared ranges on the recording thread";
            return made;
        }
    };
    const std::vector<MTLRenderPassDescriptor *> into = passes(b, kEncoders);
    const uint64_t shortOne = run([&](id<MTLCommandBuffer> cb) {
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:into[0]];
        [e setRenderPipelineState:b.state];
        compositorDraw(e, b, 0, 0);
        [e endEncoding];
    });
    EXPECT_EQ(shortOne, 0u);
    const uint64_t frame = run([&](id<MTLCommandBuffer> cb) {
        compositorFrame(cb, b, into, kDraws, 0);
    });
    EXPECT_LE(frame, uint64_t(kEncoders));
}

// A wait's time goes to its parts in order, none past the wait: before the worker took it, parked, encoding, the
// handler queue's hop from its scheduling, then the rest
TEST(GM20BRecordCost, AScheduledWaitSplitsIntoItsParts)
{
    gm20b::ScheduleNotes n;
    n.takenUs = 1300;
    n.parkedUs = 200;
    n.encodeUs = 400;
    n.scheduledUs = 2000;
    const gm20b::ScheduledWait w = gm20b::splitScheduledWait(n, 1000, 2500);
    EXPECT_EQ(w.queued, 300u);
    EXPECT_EQ(w.parked, 200u);
    EXPECT_EQ(w.encoding, 400u);
    EXPECT_EQ(w.handlers, 500u);
    EXPECT_EQ(w.other, 100u);
    gm20b::ScheduleNotes never;
    EXPECT_EQ(gm20b::splitScheduledWait(never, 1000, 1700).queued, 700u);
    gm20b::ScheduleNotes late;
    late.takenUs = 900;
    late.encodeUs = 5000;
    const gm20b::ScheduledWait clipped = gm20b::splitScheduledWait(late, 1000, 1600);
    EXPECT_EQ(clipped.queued, 0u);
    EXPECT_EQ(clipped.encoding, 600u);
    EXPECT_EQ(clipped.total(), 600u);
}

// The jobs of compositor frames, each with an encoder of indexed draws after them (the first of a new state, the rest of
// the same one), as job_words.h's text: what a change of the recording path must leave as it is
TEST(GM20BRecordCost, CompositorFrameWords)
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
            EXPECT_EQ(std::string([[(id)cb performSelector:@selector(switchMetalPath)] UTF8String]), "gm20b");
        }
    const std::string words = job_words::words(runs, mark, {{"indices", indices.gpuAddress, sizeof quad}});
    const char *outputs = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    std::ofstream(std::string(outputs ? outputs : harness::dir().c_str()) + "/compositor_words.txt") << words;
    printf("compositor words: %zu bytes, hash %016llx\n", words.size(), (unsigned long long)job_words::fnv(words));
    EXPECT_FALSE(words.empty());
}
#endif

double threadCpuMicroseconds(const char *name)
{
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS || !list)
        return 0;
    double found = 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t info{};
        mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(list[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &size) == KERN_SUCCESS &&
            strcmp(info.pth_name, name) == 0) {
            found = double(info.pth_user_time + info.pth_system_time) / 1000.0;
            break;
        }
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), vm_size_t(count) * sizeof(thread_t));
    return found;
}

// Mirrors apps/hwtest/tests/FrameRecordCostTest.swift: 20 passes over 4 repeating 256x256 targets plus a memoryless
// RGBA16Float scratch, 125 draws with 8 setBytes + scissor + triangleStrip, measuring both recording and submit-worker CPU.
TEST(GM20BRecordCost, HomeScreenFrameCost)
{
    Bench &b = bench();
    ASSERT_TRUE(b.device != nil && b.state != nil);
    constexpr int kPasses = 20, kHomeDraws = 125, kHomeTargets = 4, kSide = 256, kHomeWarm = 16, kHomeFrames = 48;
    std::vector<id<MTLTexture>> targets;
    for (int i = 0; i < kHomeTargets; ++i)
        targets.push_back(texture(b.device, kSide, kSide, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead));
    MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                  width:kSide
                                                                                 height:kSide
                                                                              mipmapped:NO];
    sd.storageMode = MTLStorageModeMemoryless;
    sd.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> scratch = [b.device newTextureWithDescriptor:sd];
    ASSERT_NE(scratch, nil);

    struct Shape {
        int passes, draws, binds;
    };
    struct Cost {
        double recordUs = 0, workerUs = 0, recordInstr = 0;
        record_count counts{};
    };
    const uint8_t zeros[256] = {};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto encode = [&](id<MTLCommandBuffer> cb, Shape shape) {
        for (int pass = 0; pass < shape.passes; ++pass) {
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = targets[size_t(pass % kHomeTargets)];
            d.colorAttachments[0].loadAction = pass < kHomeTargets ? MTLLoadActionClear : MTLLoadActionLoad;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            d.colorAttachments[1].texture = scratch;
            d.colorAttachments[1].loadAction = MTLLoadActionClear;
            d.colorAttachments[1].storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
            [e setRenderPipelineState:b.state];
            const int mine = shape.draws / shape.passes + (pass < shape.draws % shape.passes ? 1 : 0);
            for (int draw = 0; draw < mine; ++draw) {
                const float x = float(draw % 8) / 4.0f - 1.0f, y = float(pass % 6) / 3.0f - 1.0f;
                const float vertices[16] = {x, y, 0, 1, x + 0.25f, y, 0, 1, x, y + 0.3f, 0, 1, x + 0.25f, y + 0.3f, 0,
                                            1};
                for (int k = 0; k < shape.binds; ++k) {
                    [e setVertexBytes:vertices length:sizeof vertices atIndex:1];
                    [e setVertexBytes:identity length:sizeof identity atIndex:2];
                    [e setVertexBytes:zeros length:sizeof zeros atIndex:0];
                    [e setVertexBytes:zeros length:sizeof zeros atIndex:3];
                    for (NSUInteger slot = 0; slot < 4; ++slot)
                        [e setFragmentBytes:zeros length:sizeof zeros atIndex:slot];
                }
                [e setScissorRect:MTLScissorRect{NSUInteger((draw % 8) * 32), NSUInteger((pass % 8) * 32), 32, 32}];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [e endEncoding];
        }
    };
    const auto run = [&](Shape shape) -> Cost {
        for (int i = 0; i < kHomeWarm; ++i)
            @autoreleasepool {
                id<MTLCommandBuffer> cb = [b.queue commandBuffer];
                encode(cb, shape);
                [cb commit];
                [cb waitUntilCompleted];
                EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            }
        const double workerBefore = threadCpuMicroseconds("com.brunostjohn.switch.gm20b.submit");
        double recordUs = 0, recordInstr = 0;
        record_count total{};
        std::vector<id<MTLCommandBuffer>> held;
        for (int i = 0; i < kHomeFrames; ++i)
            @autoreleasepool {
                id<MTLCommandBuffer> cb = [b.queue commandBuffer];
                const Counts c0 = counts();
                record_count_start();
                const uint64_t t0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
                encode(cb, shape);
                [cb commit];
                [cb waitUntilScheduled];
                const uint64_t dt = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - t0;
                record_count c{};
                record_count_stop(&c);
                const Counts c1 = counts();
                recordUs += double(dt) / 1000.0;
                recordInstr += double(c1.instructions - c0.instructions);
                total.locks += c.locks;
                total.mallocs += c.mallocs;
                total.weaks += c.weaks;
                total.retains += c.retains;
                held.push_back(cb);
                if (held.size() == 3) {
                    for (id<MTLCommandBuffer> h : held)
                        [h waitUntilCompleted];
                    held.clear();
                }
            }
        for (id<MTLCommandBuffer> h : held)
            [h waitUntilCompleted];
        const double workerAfter = threadCpuMicroseconds("com.brunostjohn.switch.gm20b.submit");
        return {recordUs / kHomeFrames, (workerAfter - workerBefore) / kHomeFrames, recordInstr / kHomeFrames, total};
    };
    const Cost base = run({kPasses, kHomeDraws, 1});
    const Cost morePasses = run({2 * kPasses, kHomeDraws, 1});
    const Cost moreDraws = run({kPasses, 2 * kHomeDraws, 1});
    const Cost moreBinds = run({kPasses, kHomeDraws, 2});
    printf("home frame: record %.1f us (%.0f instr) worker %.1f us total %.1f us | "
           "per pass: rec %.2f us (%.0f instr) wrk %.2f us | "
           "per draw: rec %.2f us (%.0f instr) wrk %.2f us | "
           "per bind: rec %.3f us (%.0f instr) wrk %.3f us\n",
           base.recordUs, base.recordInstr, base.workerUs, base.recordUs + base.workerUs,
           (morePasses.recordUs - base.recordUs) / kPasses, (morePasses.recordInstr - base.recordInstr) / kPasses,
           (morePasses.workerUs - base.workerUs) / kPasses, (moreDraws.recordUs - base.recordUs) / kHomeDraws,
           (moreDraws.recordInstr - base.recordInstr) / kHomeDraws, (moreDraws.workerUs - base.workerUs) / kHomeDraws,
           (moreBinds.recordUs - base.recordUs) / (kHomeDraws * 8.0),
           (moreBinds.recordInstr - base.recordInstr) / (kHomeDraws * 8.0),
           (moreBinds.workerUs - base.workerUs) / (kHomeDraws * 8.0));
}

}  // namespace
