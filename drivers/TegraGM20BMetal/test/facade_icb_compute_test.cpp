// SPDX-License-Identifier: MIT
// Indirect command buffers of dispatches on the facade over the kernel model (facade_compute.h,
// TegraGM20BMetalIndirectCompute.h): a command that launches more than a buffer's slots hold grows them and the commands
// before it still run; a blit's copy out of a buffer whose slots grew into one whose slots did not, and a blit's reset,
// launch what Metal would, run on the copy engine of the model GPU (model_gpu.h); and a buffer that inherits the
// pipeline, whose variant holds a command the device had no memory for outside an earlier execute's range, makes its
// variant again when a later execute reaches that command, which then runs. A range read past its range buffer launches
// none, and one at an offset off 4 bytes is read from the offset rounded down, as an M4 Max's GPU reads them.
#include "facade_compute.h"
#include "model_gpu.h"
#include "../TegraGM20BMetalIndirectCompute.h"

#include <atomic>
#include <functional>
#include <gtest/gtest.h>
#include <memory>

namespace {

using namespace harness;

constexpr uint32_t kCompute = 1, kSendPcasA = 0x02b4;  // NVB1C0_SEND_PCAS_A
// QMDV01_07's CTA_RASTER_WIDTH, then CTA_RASTER_HEIGHT and CTA_RASTER_DEPTH (clb1c0qmd.h, bits 384-447)
constexpr uint32_t kRasterWidth = 12, kRasterHeightDepth = 13;
// Threadgroups down that the QMD's 16-bit height takes in nine launches (dispatch_bands.h qmdSlice): more than a
// slot of gm20b::icb::kComputeSlotWords holds
constexpr NSUInteger kGrownGroups = 8 * 0xffff + 1;
constexpr uint32_t kGrownLaunches = 9;
// Kernel 0's width whose record reads argument buffers through a set-1 heap array of its own
constexpr uint32_t kTablesThreads = kKernels + 3;

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState;

// Kernel 0's record at kTablesThreads threads wide with one sampler heap array, which no other record shares: its
// placement allocates the set's memory (Device::heapSet), refused while the model refuses control memory
gm20b::PackKernel tablesRecord(uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(kKernelSeed);
    const uint32_t threads[3] = {kTablesThreads, 1, 1};
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(gm20b::computeKey(air, "", maxThreads, false, threads, "", "", ""));
    k.regionWord = -1;
    k.heapArrays = 1;
    k.heap[0] = {2, 0};  // HeapTables::samplerClass
    k.heapBytes = 4 * gm20b::kPackHeapElements;
    return k;
}

// The compute launches in the words of every job submitted from `first` on
uint32_t launches(size_t first)
{
    uint32_t n = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, sub = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4)
                continue;
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i) {
                const uint32_t m = op == 1 ? method + 4 * k : op == 5 ? method + (k ? 4 : 0) : method;
                n += sub == kCompute && m == kSendPcasA;
            }
        }
    }
    return n;
}

struct Ran {
    MTLCommandBufferStatus status;
    std::string path;
    uint32_t launched;  // SEND_PCAS_A in its jobs' words
};

Ran ranCommands(const std::function<void(id<MTLCommandBuffer>)> &record)
{
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    record(cb);
    [cb commit];
    [cb waitUntilCompleted];
    NSString *path = [(id)cb valueForKey:@"switchMetalPath"];
    return {cb.status, path.UTF8String ?: "", launches(jobs)};
}

// An execute of `range` of `icb` under gState
Ran execute(id<MTLIndirectCommandBuffer> icb, NSRange range)
{
    return ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        [e executeCommandsInBuffer:icb withRange:range];
        [e endEncoding];
    });
}

Ran blit(const std::function<void(id<MTLBlitCommandEncoder>)> &encode)
{
    return ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        encode(b);
        [b endEncoding];
    });
}

id<MTLIndirectCommandBuffer> dispatches(NSUInteger count, BOOL inheritPipeline)
{
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    d.inheritPipelineState = inheritPipeline;
    d.inheritBuffers = NO;
    return [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:count options:0];
}

// Command `index` of `icb`: `groups` threadgroups `threads` wide, of gState where its buffer does not inherit it
void encode(id<MTLIndirectCommandBuffer> icb, NSUInteger index, MTLSize groups, NSUInteger threads)
{
    id<MTLIndirectComputeCommand> c = [icb indirectComputeCommandAtIndex:index];
    if (!((SwitchMetalIndirectComputeBuffer *)icb)->_inheritPipeline)
        [c setComputePipelineState:gState];
    [c concurrentDispatchThreadgroups:groups threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
}

class FacadeIcbCompute : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        std::vector<gm20b::PackKernel> kernels;
        std::vector<gm20b::PackComputeFacts> facts;
        for (uint32_t i = 0; i < kKernels; i++) {
            kernels.push_back(kernelRecord(i, maxThreads));
            facts.push_back(kernelFacts(i, maxThreads));
        }
        kernels.push_back(tablesRecord(maxThreads));
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), kernels, facts));
        ASSERT_TRUE(gDevice != nil);
        metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library newFunctionWithName:@(kernelName(0).c_str())];
        gState =
            [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        ASSERT_TRUE(gState != nil) << (error.description.UTF8String ?: "");
        // the kernels' program placed while there is memory, so a placement inside a refusal adds only its tables
        const Ran placed = ranCommands([](id<MTLCommandBuffer> cb) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:gState];
            [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [e endEncoding];
        });
        ASSERT_EQ(placed.status, MTLCommandBufferStatusCompleted);
    }
};

TEST_F(FacadeIcbCompute, ACommandPastItsBuffersSlotsGrowsThemAndTheCommandBeforeItStillRuns)
{
    id<MTLIndirectCommandBuffer> icb = dispatches(2, NO);
    ASSERT_TRUE(icb != nil);
    SwitchMetalIndirectComputeBuffer *own = (SwitchMetalIndirectComputeBuffer *)icb;
    encode(icb, 0, MTLSizeMake(1, 1, 1), 1);
    ASSERT_EQ(own->_slotWords, gm20b::icb::kComputeSlotWords);
    encode(icb, 1, MTLSizeMake(1, kGrownGroups, 1), 1);
    ASSERT_GT(own->_slotWords, gm20b::icb::kComputeSlotWords) << "nine launches in slots that hold eight";
    const Ran both = execute(icb, NSMakeRange(0, 2));
    EXPECT_EQ(both.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(both.path, "gm20b");
    EXPECT_EQ(both.launched, 1 + kGrownLaunches);
    const Ran first = execute(icb, NSMakeRange(0, 1));
    EXPECT_EQ(first.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(first.launched, 1u) << "the first command, written again into the grown slots";
}

// The copy engine copies the commands and rewrites their index words, and the reset fills a command's slot with no-ops,
// all in GPU order: the launches are what the model GPU runs, and their threadgroups what Metal dispatches
TEST_F(FacadeIcbCompute, ABlitsCopyAndResetOfCommandsLaunchWhatMetalWould)
{
    id<MTLIndirectCommandBuffer> from = dispatches(2, NO), to = dispatches(2, NO);
    ASSERT_TRUE(from != nil && to != nil);
    encode(from, 0, MTLSizeMake(1, 1, 1), 1);
    encode(from, 1, MTLSizeMake(1, kGrownGroups, 1), 1);
    SwitchMetalIndirectComputeBuffer *source = (SwitchMetalIndirectComputeBuffer *)from,
                                     *destination = (SwitchMetalIndirectComputeBuffer *)to;
    ASSERT_GT(source->_slotWords, destination->_slotWords);
    std::atomic<uint64_t> launched{0}, groups{0};
    model_gpu::Gpu gpu([&](uint32_t *qmd, predication_model::Gpu &) {
        launched++;
        groups += uint64_t(qmd[kRasterWidth]) * (qmd[kRasterHeightDepth] & 0xffff) * (qmd[kRasterHeightDepth] >> 16);
    });
    const auto runs = [&](NSRange range, uint64_t wantLaunches, uint64_t wantGroups) {
        launched = groups = 0;
        const Ran r = execute(to, range);
        EXPECT_EQ(r.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(r.path, "gm20b");
        EXPECT_EQ(launched.load(), wantLaunches);
        EXPECT_EQ(groups.load(), wantGroups);
    };
    const Ran copied = blit([&](id<MTLBlitCommandEncoder> b) {
        [b copyIndirectCommandBuffer:from sourceRange:NSMakeRange(0, 2) destination:to destinationIndex:0];
    });
    ASSERT_EQ(copied.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(destination->_slotWords, source->_slotWords) << "the destination's slots grew to the copied commands'";
    {
        SCOPED_TRACE("after the copy");
        runs(NSMakeRange(0, 2), 1 + kGrownLaunches, 1 + kGrownGroups);
    }
    const Ran reset = blit([&](id<MTLBlitCommandEncoder> b) {
        [b resetCommandsInBuffer:to withRange:NSMakeRange(1, 1)];
    });
    ASSERT_EQ(reset.status, MTLCommandBufferStatusCompleted);
    {
        SCOPED_TRACE("after the reset of the second command");
        runs(NSMakeRange(0, 2), 1, 1);
    }
}

// An execute of a range past its buffer launches the commands inside it, as an M4 Max does (skipped as call.icb); a
// buffer of dispatches of another device executed, a reset past a buffer and an execute that reaches a command of
// another device's buffer fail their command buffer InvalidResource (c38-audit/failed-counts/probe)
TEST_F(FacadeIcbCompute, ARangePastItsBufferRunsWhatIsInsideAndAnotherDevicesObjectsFail)
{
    id<MTLIndirectCommandBuffer> icb = dispatches(2, NO), foreign = dispatches(1, NO);
    ASSERT_TRUE(icb != nil && foreign != nil);
    encode(icb, 0, MTLSizeMake(1, 1, 1), 1);
    encode(icb, 1, MTLSizeMake(1, 1, 1), 1);
    encode(foreign, 0, MTLSizeMake(1, 1, 1), 1);
    [[foreign indirectComputeCommandAtIndex:0] setKernelBuffer:(id<MTLBuffer>)[NSObject new] offset:0 atIndex:0];
    std::atomic<uint64_t> launched{0};
    model_gpu::Gpu gpu([&](uint32_t *, predication_model::Gpu &) {
        launched++;
    });
    NSDictionary *before = counters(gDevice);
    const Ran cut = execute(icb, NSMakeRange(1, 3));
    EXPECT_EQ(cut.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(cut.path, "gm20b");
    EXPECT_EQ(launched.load(), 1u);
    NSDictionary *after = counters(gDevice);
    EXPECT_EQ(skippedMoved(before, after), skippedOnce("call.icb"));
    EXPECT_TRUE(failedMoved(before, after).empty());
    const auto fails = [](const Ran &r, const char *path) {
        EXPECT_EQ(r.status, MTLCommandBufferStatusError) << path;
        EXPECT_EQ(r.path, path);
    };
    fails(execute((id<MTLIndirectCommandBuffer>)[NSObject new], NSMakeRange(0, 1)), "error:call.icb");
    fails(blit([&](id<MTLBlitCommandEncoder> b) {
              [b resetCommandsInBuffer:icb withRange:NSMakeRange(1, 4)];
          }),
          "error:call.icb");
    fails(execute(foreign, NSMakeRange(0, 1)), "error:compute");
}

// An execute whose range the GPU reads from a buffer: past it (an M4 Max's GPU reads whatever lies there) nothing is
// recorded, each call skipped as call.icb once, and the encoder's dispatch after them launches; at an offset off 4 bytes
// the execute is recorded reading the range from the offset rounded down, skipped as call.icb
TEST_F(FacadeIcbCompute, ARangeReadPastItsBufferRunsNoneAndOneOffFourBytesIsReadFromTheOffsetRoundedDown)
{
    id<MTLIndirectCommandBuffer> icb = dispatches(2, NO);
    id<MTLBuffer> range = [gDevice newBufferWithLength:64 options:0];
    ASSERT_TRUE(icb != nil && range != nil);
    encode(icb, 0, MTLSizeMake(1, 1, 1), 1);
    encode(icb, 1, MTLSizeMake(1, 1, 1), 1);
    NSDictionary *before = counters(gDevice);
    const Ran past = ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
        for (NSUInteger offset : {60, 62, 64, 1 << 20}) {
            [e executeCommandsInBuffer:icb indirectBuffer:range indirectBufferOffset:offset];
            EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded) << offset;
        }
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [e endEncoding];
    });
    EXPECT_EQ(past.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(past.path, "gm20b");
    EXPECT_EQ(past.launched, 1u) << "the dispatch after them";
    NSDictionary *after = counters(gDevice);
    const std::map<std::string, long long> four = {{"skipped", 4}, {"skipped.call.icb", 4}};
    EXPECT_EQ(skippedMoved(before, after), four);
    EXPECT_TRUE(failedMoved(before, after).empty());
    before = counters(gDevice);
    size_t executes = 0;
    const Ran rounded = ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        const SwitchMetalCommandPlan &plan = ((SwitchMetalCommandBuffer *)cb)->_plan;
        const size_t recorded = plan.size();
        [e executeCommandsInBuffer:icb indirectBuffer:range indirectBufferOffset:58];
        for (size_t i = recorded; i < plan.size(); i++)
            if (plan.operation(i).op == OpExecuteKernels) {
                executes++;
                EXPECT_EQ(((__bridge SwitchMetalKernelExecute *)plan.control(i).object)->_rangeOffset, 56u);
            }
        [e endEncoding];
    });
    EXPECT_EQ(executes, 1u);
    EXPECT_EQ(rounded.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(rounded.path, "gm20b");
    after = counters(gDevice);
    EXPECT_EQ(skippedMoved(before, after), skippedOnce("call.icb"));
    EXPECT_TRUE(failedMoved(before, after).empty());
}

TEST_F(FacadeIcbCompute, AVariantsCommandWithoutMemoryOutsideAnExecuteIsMadeAgainWhenALaterExecuteReachesIt)
{
    id<MTLIndirectCommandBuffer> icb = dispatches(2, YES);
    ASSERT_TRUE(icb != nil);
    SwitchMetalIndirectComputeBuffer *own = (SwitchMetalIndirectComputeBuffer *)icb;
    encode(icb, 0, MTLSizeMake(1, 1, 1), 1);
    encode(icb, 1, MTLSizeMake(1, 1, 1), kTablesThreads);
    // the commands' kernels at the encoder's pipeline are made as the execute is recorded
    const Ran outside = ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        kernel_model::model.refuseControl = true;
        [e executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        kernel_model::model.refuseControl = false;
        [e endEncoding];
    });
    EXPECT_EQ(outside.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(outside.path, "gm20b");
    EXPECT_EQ(outside.launched, 1u);
    ASSERT_EQ(own->_variants.size(), 1u);
    const std::shared_ptr<SwitchMetalICBVariant> first = own->_variants.begin()->second;
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(first->missing[1].why != nullptr) << "the second command's tables made inside the refusal";
    ASSERT_EQ(first->missing[1].failure, gm20b::ComputeFailure::OutOfMemory);
    const Ran reached = execute(icb, NSMakeRange(0, 2));
    EXPECT_EQ(reached.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(reached.path, "gm20b");
    EXPECT_EQ(reached.launched, 2u);
    ASSERT_EQ(own->_variants.size(), 1u);
    EXPECT_NE(own->_variants.begin()->second.get(), first.get()) << "the variant made again";
    EXPECT_TRUE(own->_variants.begin()->second->missing[1].why == nullptr);
}

}  // namespace
