// SPDX-License-Identifier: MIT
// Compute work the driver refuses ends on it, never replayed elsewhere: a dispatch whose kernel the compiler refused
// (HW Test gpu.upload.blit_buffer and gpu.upload.replace_region: "a kernel the pack lacks is refused ... a texture view
// of VkFormat 1000470001 NVK has no header for"), directly or in an indirect command buffer, fails Internal named by
// compute.nokernel; a call Metal's validation stops fails InvalidResource; a kernel record the encode refuses, or a
// texture the driver keeps no form of, fails Internal. What Apple's GPU does with no failure runs so on raw (an M4 Max,
// c38-audit/m2v-compute/probe): indirect arguments at their offset rounded down to 4 bytes, a grid past their buffer
// launching nothing; a predication Start naming no buffer withholding its compute unit's dispatches. A compute state of
// another device's function is not made, and a state's selector it has no method of opens no m2v.
#include "facade_compute.h"
#include "../gm20b_cache.h"

#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <utility>

// The encoder's predication SPI (facade_loops_test.cpp)
@protocol RefusalPredicatedEncoder
- (void)encodeStartIf:(id<MTLBuffer>)buffer
               offset:(NSUInteger)offset
           comparison:(NSUInteger)comparison
       referenceValue:(uint32_t)reference;
- (BOOL)encodeEndIf;
- (void)encodeStartDoWhile;
- (BOOL)encodeEndDoWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference;
@end

namespace {

using namespace harness;

constexpr uint32_t kRefusedThreads = kKernels + 3, kUnboundThreads = kKernels + 5, kTextureThreads = kKernels + 7;
constexpr uint32_t kCompute = 1, kSendPcasA = 0x02b4;  // NVB1C0_SEND_PCAS_A
const gm20b::BoundTexture kRgba = {0, MTLPixelFormatRGBA8Unorm, MTLTextureType2D, 1, 1, {2, 3, 4, 5}};

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState;

// Kernel 0's record at kUnboundThreads threads wide, holding a set entry of a kind no dispatch binds: admission reads
// no such entry, the encode refuses it
gm20b::PackKernel unboundRecord(uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(kKernelSeed);
    const uint32_t threads[3] = {kUnboundThreads, 1, 1};
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(gm20b::computeKey(air, "", maxThreads, false, threads, "", "", ""));
    k.regionWord = -1;
    k.entries = 1;
    k.entry[0] = {0, 0x7f, 0, 0};
    return k;
}

// Kernel 0's record at kTextureThreads threads wide over an RGBA8 texture at slot 0
gm20b::PackKernel textureRecord(uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(kKernelSeed);
    const uint32_t threads[3] = {kTextureThreads, 1, 1};
    gm20b::PackKernel k = {};
    const std::string textures = gm20b::computeTextures(&kRgba, 1);
    k.keyHash = gm20b::computeKeyHash(gm20b::computeKey(air, "", maxThreads, false, threads, "", textures, ""));
    k.regionWord = -1;
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(gm20b::KernelTexture), 0, 0};
    k.readOnlyEntries = 1;
    return k;
}

// The compute launches of every job submitted from `first` on
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
    NSInteger code;
    std::string path;
    uint32_t launched;
    std::map<std::string, long long> failed;
};

// What `record` records into a command buffer, committed and waited for: how it ended, how many compute launches it
// made and the failed counters it moved
Ran ranCommands(const std::function<void(id<MTLCommandBuffer>)> &record)
{
    const size_t jobs = kernel_model::model.jobs.size();
    NSDictionary *before = counters(gDevice);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    record(cb);
    [cb commit];
    [cb waitUntilCompleted];
    NSString *path = [(id)cb valueForKey:@"switchMetalPath"];
    return {cb.status, cb.error.code, path.UTF8String ?: "", launches(jobs), failedMoved(before, counters(gDevice))};
}

// One compute encoder `encode` records
template <typename Encode> Ran ran(Encode encode)
{
    return ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        encode(e);
        [e endEncoding];
    });
}

class FacadeKernelRefusal : public ::testing::Test {
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
        kernels.push_back(unboundRecord(maxThreads));
        kernels.push_back(textureRecord(maxThreads));
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), kernels, facts));
        ASSERT_TRUE(gDevice != nil);
        gm20b::AirHash air;
        air.fill(kKernelSeed);
        const uint32_t threads[3] = {kRefusedThreads, 1, 1};
        gm20b::CacheKey refused;
        refused.key = "kernel | " + gm20b::computeKey(air, "", maxThreads, false, threads, "", "", "");
        gm20b::RecordCache cache;
        ASSERT_TRUE(cache.attach(dir(), 0, 1 << 20));
        ASSERT_TRUE(cache.storeKernel(refused, nullptr, "a texture view of VkFormat 1000470001 NVK has no header for"));
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
    }
};

// The compiler's refusal reaches the first dispatch at its submission, or as it is recorded where the compiler answered
// first; the second's always as it is recorded, the state keeping the refused kernel. Each names compute.nokernel.
TEST_F(FacadeKernelRefusal, ADispatchOfARefusedKernelNamesComputeNoKernel)
{
    for (int dispatch = 0; dispatch < 2; dispatch++) {
        SCOPED_TRACE(dispatch);
        NSDictionary *before = counters(gDevice);
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kRefusedThreads, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        NSString *path = [(id)cb valueForKey:@"switchMetalPath"];
        ASSERT_TRUE(path != nil);
        EXPECT_TRUE([path isEqualToString:@"error:compute.nokernel"]) << path.UTF8String;
        EXPECT_EQ(failedMoved(before, counters(gDevice)), failedOnce("compute.nokernel"));
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
        ASSERT_TRUE(cb.error != nil);
        EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
        EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    }
}

TEST_F(FacadeKernelRefusal, AKernelRecordTheEncodeRefusesFailsItsCommandBufferOnRaw)
{
    const Ran r = ran([](id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:gState];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kUnboundThreads, 1, 1)];
    });
    EXPECT_EQ(r.status, MTLCommandBufferStatusError);
    EXPECT_EQ(r.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(r.path, "error:compute");
    EXPECT_EQ(r.failed, failedOnce("compute"));
}

// Calls Metal's validation stops: a buffer of another device, a stage-in region past its buffer, a dispatch with no
// state set, a predicate word past its buffer
TEST_F(FacadeKernelRefusal, ACallMetalsValidationStopsFailsItsCommandBufferOnRaw)
{
    id<MTLBuffer> eightBytes = [gDevice newBufferWithLength:8 options:MTLResourceStorageModeShared];
    const std::vector<std::pair<std::function<void(id<MTLComputeCommandEncoder>)>, std::string>> calls = {
        {[](id<MTLComputeCommandEncoder> e) {
             [e setComputePipelineState:gState];
             [e setBuffer:(id<MTLBuffer>)[NSObject new] offset:0 atIndex:0];
             [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
         },
         "error:compute"},
        {[eightBytes](id<MTLComputeCommandEncoder> e) {
             [e setComputePipelineState:gState];
             [e setStageInRegionWithIndirectBuffer:eightBytes indirectBufferOffset:0];
             [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
         },
         "error:compute"},
        {[](id<MTLComputeCommandEncoder> e) {
             [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
         },
         "error:compute"},
        {[eightBytes](id<MTLComputeCommandEncoder> e) {
             [e setComputePipelineState:gState];
             [(id<RefusalPredicatedEncoder>)e encodeStartIf:eightBytes offset:64 comparison:0 referenceValue:1];
             [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
             [(id<RefusalPredicatedEncoder>)e encodeEndIf];
         },
         "error:compute.predicate"},
    };
    for (size_t i = 0; i < calls.size(); i++) {
        SCOPED_TRACE(i);
        const Ran r = ran(calls[i].first);
        EXPECT_EQ(r.status, MTLCommandBufferStatusError);
        EXPECT_EQ(r.code, MTLCommandBufferErrorInvalidResource);
        EXPECT_EQ(r.path, calls[i].second);
        EXPECT_EQ(r.failed, failedOnce(calls[i].second.substr(std::strlen("error:"))));
    }
}

// As an M4 Max reads them: at an offset off 4 bytes the words at the offset rounded down (one threadgroup here); a grid
// whose last word lies past the buffer reads it zero and launches nothing; both complete
TEST_F(FacadeKernelRefusal, IndirectArgumentsAreReadAsApplesGpuReadsThem)
{
    const uint32_t ones[4] = {1, 1, 1, 0};
    id<MTLBuffer> arguments = [gDevice newBufferWithBytes:ones length:16 options:MTLResourceStorageModeShared];
    const std::vector<std::pair<NSUInteger, uint32_t>> offsets = {{2, 1}, {8, 0}, {64, 0}};
    for (const auto &read : offsets) {
        SCOPED_TRACE(read.first);
        const Ran r = ran([&](id<MTLComputeCommandEncoder> e) {
            [e setComputePipelineState:gState];
            [e dispatchThreadgroupsWithIndirectBuffer:arguments
                                 indirectBufferOffset:read.first
                                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        });
        EXPECT_EQ(r.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(r.path, "gm20b");
        EXPECT_EQ(r.launched, read.second);
    }
}

// As an M4 Max runs it: no dispatch of the compute unit holding a Start that names no buffer (its encoders before and
// after it too) and every End of that encoder NO, the command buffer completed; a blit between encoders ends the unit
TEST_F(FacadeKernelRefusal, AStartNamingNoBufferWithholdsItsComputeUnit)
{
    id<MTLBuffer> scratch = [gDevice newBufferWithLength:16 options:MTLResourceStorageModeShared];
    for (const bool split : {false, true}) {
        SCOPED_TRACE(split);
        BOOL answered = YES;
        const Ran r = ranCommands([&](id<MTLCommandBuffer> cb) {
            const auto encoder = [&](bool withholds) {
                id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                [e setComputePipelineState:gState];
                [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                if (withholds) {
                    [(id<RefusalPredicatedEncoder>)e encodeStartIf:nil offset:0 comparison:0 referenceValue:1];
                    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                    answered = [(id<RefusalPredicatedEncoder>)e encodeEndIf];
                }
                [e endEncoding];
                if (split) {
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    [b fillBuffer:scratch range:NSMakeRange(0, 16) value:0];
                    [b endEncoding];
                }
            };
            encoder(false);
            encoder(true);
            encoder(false);
        });
        EXPECT_EQ(r.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(r.path, "gm20b");
        EXPECT_EQ(r.launched, split ? 2u : 0u);
        EXPECT_FALSE(answered);
    }
}

// As an M4 Max runs them: an EndDoWhile naming no buffer withholds its unit as a Start does, the loop's body included,
// and answers NO; an event signal between encoders, or another dispatch type, ends the unit
TEST_F(FacadeKernelRefusal, AnEndDoWhileNamingNoBufferWithholdsItsUnitAndEventsAndDispatchTypesEndOne)
{
    const auto one = [](id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:gState];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    };
    BOOL answered = YES;
    const Ran loop = ranCommands([&](id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        one(e);
        [(id<RefusalPredicatedEncoder>)e encodeStartDoWhile];
        one(e);
        answered = [(id<RefusalPredicatedEncoder>)e encodeEndDoWhile:nil offset:0 comparison:0 referenceValue:1];
        one(e);
        [e endEncoding];
    });
    EXPECT_EQ(loop.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(loop.path, "gm20b");
    EXPECT_EQ(loop.launched, 0u);
    EXPECT_FALSE(answered);
    id<MTLEvent> event = [gDevice newEvent];
    for (const bool signal : {true, false}) {
        SCOPED_TRACE(signal);
        const Ran r = ranCommands([&](id<MTLCommandBuffer> cb) {
            const MTLDispatchType outer = signal ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial;
            id<MTLComputeCommandEncoder> a = [cb computeCommandEncoderWithDispatchType:outer];
            one(a);
            [a endEncoding];
            if (signal)
                [cb encodeSignalEvent:event value:1];
            id<MTLComputeCommandEncoder> b = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
            one(b);
            [(id<RefusalPredicatedEncoder>)b encodeStartIf:nil offset:0 comparison:0 referenceValue:1];
            one(b);
            [(id<RefusalPredicatedEncoder>)b encodeEndIf];
            [b endEncoding];
            if (signal)
                [cb encodeSignalEvent:event value:2];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoderWithDispatchType:outer];
            one(c);
            [c endEncoding];
        });
        EXPECT_EQ(r.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(r.path, "gm20b");
        EXPECT_EQ(r.launched, 2u);
    }
}

// An indirect command buffer of one dispatch of gState at `threads` threads wide over `grid` threadgroups
id<MTLIndirectCommandBuffer> oneDispatch(MTLSize grid, NSUInteger threads)
{
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    d.inheritPipelineState = NO;
    d.inheritBuffers = NO;
    id<MTLIndirectCommandBuffer> icb = [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:1 options:0];
    id<MTLIndirectComputeCommand> c = [icb indirectComputeCommandAtIndex:0];
    [c setComputePipelineState:gState];
    [c concurrentDispatchThreadgroups:grid threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
    return icb;
}

// An execute that reaches a command whose kernel the compiler refused fails as that dispatch would; a command of an
// empty grid launches nothing
TEST_F(FacadeKernelRefusal, AnIndirectCommandOfARefusedKernelFailsAndOneOfNoGridRuns)
{
    id<MTLIndirectCommandBuffer> refused = oneDispatch(MTLSizeMake(1, 1, 1), kRefusedThreads);
    ASSERT_TRUE(refused != nil);
    const Ran failed = ran([refused](id<MTLComputeCommandEncoder> e) {
        [e executeCommandsInBuffer:refused withRange:NSMakeRange(0, 1)];
    });
    EXPECT_EQ(failed.status, MTLCommandBufferStatusError);
    EXPECT_EQ(failed.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(failed.path, "error:compute.nokernel");
    EXPECT_EQ(failed.failed, failedOnce("compute.nokernel"));
    id<MTLIndirectCommandBuffer> empty = oneDispatch(MTLSizeMake(0, 1, 1), 1);
    ASSERT_TRUE(empty != nil);
    const Ran ranEmpty = ran([empty](id<MTLComputeCommandEncoder> e) {
        [e executeCommandsInBuffer:empty withRange:NSMakeRange(0, 1)];
    });
    EXPECT_EQ(ranEmpty.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ranEmpty.path, "gm20b");
}

// A command encoded while the device had no memory for its regions fails the executes that reach it out of memory only
// while there is none: the next execute writes it again, the client re-encoding nothing
TEST_F(FacadeKernelRefusal, AnIndirectCommandWrittenWithoutMemoryRunsOnceThereIsMemory)
{
    const Ran placed = ran([](id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:gState];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    });
    ASSERT_EQ(placed.launched, 1u) << "its kernel placed while there is memory";
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    d.inheritPipelineState = NO;
    d.inheritBuffers = NO;
    id<MTLIndirectCommandBuffer> icb = [gDevice newIndirectCommandBufferWithDescriptor:d maxCommandCount:1 options:0];
    ASSERT_TRUE(icb != nil);
    kernel_model::model.refuseAllocs = true;
    id<MTLIndirectComputeCommand> c = [icb indirectComputeCommandAtIndex:0];
    [c setComputePipelineState:gState];
    [c concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    kernel_model::model.refuseAllocs = false;
    const Ran r = ran([icb](id<MTLComputeCommandEncoder> e) {
        [e executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    });
    EXPECT_EQ(r.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(r.path, "gm20b");
    EXPECT_EQ(r.launched, 1u);
}

TEST_F(FacadeKernelRefusal, AStateOfAnotherDevicesFunctionIsNotMade)
{
    HarnessFunction *foreign = [HarnessFunction new];
    foreign->_name = @(kernelName(0).c_str());
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = (id<MTLFunction>)foreign;
    NSError *error = nil;
    id<MTLComputePipelineState> state =
        [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
    EXPECT_TRUE(state == nil);
    EXPECT_TRUE(error != nil);
}

// A selector the state has no method of (gpuResourceID: m2v's state has none either) is answered zero by the state
TEST_F(FacadeKernelRefusal, AStatesSelectorItHasNoMethodOfOpensNoM2v)
{
    const MTLResourceID id = [gState gpuResourceID];
    uint64_t word = 1;
    memcpy(&word, &id, sizeof word);
    EXPECT_EQ(word, 0u);
}

}  // namespace
