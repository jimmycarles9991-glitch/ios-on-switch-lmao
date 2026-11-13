// SPDX-License-Identifier: MIT
// MTLComputeCommandEncoder's predicated loops on the raw facade, end to end on the host (raw_facade_compute.h): committed
// through the facade's own submission, which splits a command buffer after each block that holds a loop, holds what
// follows until that range retires, and lays down a loop's continuation where its record says it ran past its copies.
// Each job runs on the model GPU (model_gpu.h). Every shape's launches and words must be Metal's (loop_tape.h).
#include "raw_facade_compute.h"
#include "loop_tape.h"
#include "model_gpu.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

// The encoder's predication SPI, as MPSGraph encodes control flow with it
@protocol HarnessPredicatedEncoder
- (void)encodeStartIf:(id<MTLBuffer>)buffer
               offset:(NSUInteger)offset
           comparison:(NSUInteger)comparison
       referenceValue:(uint32_t)reference;
- (void)encodeStartElse;
- (BOOL)encodeEndIf;
- (void)encodeStartWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference;
- (BOOL)encodeEndWhile;
- (void)encodeStartDoWhile;
- (BOOL)encodeEndDoWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference;
@end

namespace {

using namespace harness;
using namespace loop_tape;
using loop_tape::Op;  // the facade's headers declare raw's own Op (RawMetalArguments.h) at global scope
using Encoder = id<MTLComputeCommandEncoder, HarnessPredicatedEncoder>;

// QMDV01_07's HW_ONLY_* words (clb1c0qmd.h, bits 1536-1663): the scheduler writes them as it runs a launch
constexpr uint32_t kHwOnlyFirst = 48, kHwOnlyLast = 51;

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gStates[kKernels];

struct Launch {
    size_t buffer;
    uint32_t at;

    bool operator==(const Launch &o) const
    {
        return buffer == o.buffer && at == o.at;
    }
};

struct Ran {
    std::vector<Launch> launches;
    std::vector<Words> words;
    size_t jobsWhileHeld = 0, jobs = 0, noPrefetch = 0;
    uint32_t qmdsBroken = 0;
    bool completed = true;
    std::string errors;
};

class FacadeLoops : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        std::vector<raw::PackKernel> kernels;
        for (uint32_t i = 0; i < kKernels; i++)
            kernels.push_back(kernelRecord(i, maxThreads));
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), kernels), [HarnessM2VMethods class],
                       [HarnessComputeM2V class]);
        ASSERT_TRUE(gDevice != nil);
        raw_metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        for (uint32_t i = 0; i < kKernels; i++) {
            MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
            d.computeFunction = [library newFunctionWithName:@(kernelName(i).c_str())];
            gStates[i] = [gDevice newComputePipelineStateWithDescriptor:d
                                                                options:MTLPipelineOptionNone
                                                             reflection:nil
                                                                  error:&error];
            ASSERT_TRUE(gStates[i] != nil) << i << ": " << (error.description.UTF8String ?: "");
        }
        // each state's kernel is looked up in the pack off the encoding thread: once found, every later dispatch has it
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        for (uint32_t i = 0; i < kKernels; i++) {
            [e setComputePipelineState:gStates[i]];
            [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(i + 1, 1, 1)];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }

    // Each tape a command buffer of one compute encoder on one queue, each dispatch a kernel of its own, its words a raw
    // buffer's from `start`, word `condition` alone in a 4-byte buffer as MPSGraph's condition is; committed in order and
    // waited for. `held`: the GPU runs nothing until the first command buffer's submission stops for a while, then
    // everything.
    static Ran run(const std::vector<Tape> &tapes, const std::vector<Words> &start, bool held = false,
                   uint32_t condition = kWordCount)
    {
        Ran out;
        std::vector<id<MTLBuffer>> buffers, conditions;
        const auto at = [&](size_t b, uint32_t w) {
            return w == condition ? static_cast<uint32_t *>(conditions[b].contents)
                                  : static_cast<uint32_t *>(buffers[b].contents) + w;
        };
        const auto load = [&](size_t b) {
            Words words;
            for (uint32_t w = 0; w < kWordCount; w++)
                words[w] = *at(b, w);
            return words;
        };
        const auto store = [&](size_t b, const Words &words) {
            for (uint32_t w = 0; w < kWordCount; w++)
                *at(b, w) = words[w];
        };
        for (size_t b = 0; b < tapes.size(); b++) {
            buffers.push_back([gDevice newBufferWithLength:4096 options:MTLResourceStorageModeShared]);
            conditions.push_back([gDevice newBufferWithLength:4 options:0]);
            store(b, start[b]);
        }
        std::vector<std::pair<size_t, uint32_t>> owner(kKernels, {SIZE_MAX, 0});
        model_gpu::Gpu gpu([&](uint32_t *qmd, predication_model::Gpu &) {
            for (uint32_t w = kHwOnlyFirst; w <= kHwOnlyLast; ++w) {
                out.qmdsBroken += qmd[w] != 0;
                qmd[w] = 0xa5a5a5a5u;
            }
            const uint32_t k = kernelOfQmd(qmd);
            if (k >= kKernels || owner[k].first == SIZE_MAX) {
                ADD_FAILURE() << "a launch of no dispatch: kernel " << k;
                return;
            }
            const auto [b, op] = owner[k];
            out.launches.push_back({b, op});
            Words now = load(b);
            effect(tapes[b][op], now);
            store(b, now);
        });
        gpu.hold(held);
        id<MTLCommandQueue> queue = [gDevice newCommandQueue];
        std::vector<id<MTLCommandBuffer>> cbs;
        uint32_t next = 0;
        for (size_t b = 0; b < tapes.size(); b++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            Encoder e = (Encoder)[cb computeCommandEncoder];
            for (uint32_t i = 0; i < tapes[b].size(); i++) {
                const Op &op = tapes[b][i];
                id<MTLBuffer> words = op.w == condition ? conditions[b] : buffers[b];
                const NSUInteger offset = op.w == condition ? 0 : 4 * op.w, compare = NSUInteger(op.compare);
                switch (op.kind) {
                case Op::Dispatch:
                    owner[next] = {b, i};
                    [e setComputePipelineState:gStates[next]];
                    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(next + 1, 1, 1)];
                    next++;
                    break;
                case Op::If:
                    [e encodeStartIf:words offset:offset comparison:compare referenceValue:op.reference];
                    break;
                case Op::Else:
                    [e encodeStartElse];
                    break;
                case Op::EndIf:
                    EXPECT_TRUE([e encodeEndIf]);
                    break;
                case Op::While:
                    [e encodeStartWhile:words offset:offset comparison:compare referenceValue:op.reference];
                    break;
                case Op::EndWhile:
                    EXPECT_TRUE([e encodeEndWhile]);
                    break;
                case Op::DoWhile:
                    [e encodeStartDoWhile];
                    break;
                case Op::EndDoWhile:
                    EXPECT_TRUE(
                        [e encodeEndDoWhile:words offset:offset comparison:compare referenceValue:op.reference]);
                    break;
                case Op::Encoder:
                    [e endEncoding];
                    e = (Encoder)[cb computeCommandEncoder];
                    break;
                }
            }
            [e endEncoding];
            [cb commit];
            cbs.push_back(cb);
        }
        if (held) {
            gpu.submittedBy(1, std::chrono::seconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            out.jobsWhileHeld = gpu.submitted();
            gpu.hold(false);
        }
        for (id<MTLCommandBuffer> cb : cbs) {
            [cb waitUntilCompleted];
            out.completed &= cb.status == MTLCommandBufferStatusCompleted && !cb.error;
            if (cb.error)
                out.errors += std::string(cb.error.description.UTF8String ?: "?") + "\n";
        }
        out.jobs = gpu.submitted();
        out.noPrefetch = gpu.noPrefetchEntries();
        for (size_t b = 0; b < tapes.size(); b++)
            out.words.push_back(load(b));
        return out;
    }

    // Metal's launches and words for the tapes, one command buffer after another
    static Ran metal(const std::vector<Tape> &tapes, std::vector<Words> words)
    {
        Ran out;
        for (size_t b = 0; b < tapes.size(); b++) {
            for (uint32_t at : loop_tape::metal(tapes[b], words[b]))
                out.launches.push_back({b, at});
            out.words.push_back(words[b]);
        }
        return out;
    }

    static void expectMetal(const std::vector<Tape> &tapes, const std::vector<Words> &start, const char *shape,
                            uint32_t condition = kWordCount)
    {
        const int opens = gOpens;
        const Ran want = metal(tapes, start), got = run(tapes, start, false, condition);
        EXPECT_TRUE(got.completed) << shape << ": " << got.errors;
        EXPECT_FALSE(got.launches.empty()) << shape;
        EXPECT_TRUE(got.launches == want.launches)
            << shape << ": " << got.launches.size() << " launches, Metal's " << want.launches.size();
        EXPECT_EQ(got.words, want.words) << shape;
        const bool loops = std::any_of(tapes.begin(), tapes.end(), [](const Tape &t) {
            return std::any_of(t.begin(), t.end(), [](const Op &op) {
                return op.kind == Op::While || op.kind == Op::DoWhile;
            });
        });
        EXPECT_EQ(got.noPrefetch > 0, loops) << shape << ": " << got.noPrefetch << " copies' GP entries";
        EXPECT_EQ(got.qmdsBroken, 0u) << shape;
        EXPECT_EQ(gOpens, opens) << shape << ": m2v ran something";
    }
};

constexpr uint32_t kPastOutermost = 2500, kPastNested = 300;

// d; while (w0) { w0-- }; d: the range after the loop's block waits for its job, which ran past its 1024 copies, and
// the loop's continuation starts mid encoder
TEST_F(FacadeLoops, ARangeAfterALoopWaitsForItAndTheLoopGoesOnPastItsCopies)
{
    const Tape tape = {dispatch(1, 1), test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0), dispatch(2, 1)};
    const std::vector<Words> start = {{kPastOutermost}};
    expectMetal({tape}, start, "while");
    const Ran held = run({tape}, start, true);
    EXPECT_TRUE(held.completed) << held.errors;
    EXPECT_GT(held.jobsWhileHeld, 0u);
    EXPECT_GT(held.jobs, held.jobsWhileHeld) << "the range after the loop was submitted before the loop's job ran";
    EXPECT_TRUE(held.launches == metal({tape}, start).launches);
}

TEST_F(FacadeLoops, ALoopWithinItsCopiesRunsInOneLaying)
{
    for (uint32_t n : {0u, 1u, 100u})
        expectMetal({{test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0), dispatch(1, 1)}}, {{n}}, "within");
}

// while (w0) { w0--; w1 = 300; while (w1) { w1-- }; w2++ }; w3++: the inner loop runs past its 128 copies each time
TEST_F(FacadeLoops, AnInnerLoopPastItsCopiesGoesOnAndItsOuterLoopAfterIt)
{
    expectMetal({{test(Op::While, 0), dispatch(0, -1, 1, kPastNested), test(Op::While, 1), dispatch(1, -1),
                  test(Op::EndWhile, 1), dispatch(2, 1), test(Op::EndWhile, 0), dispatch(3, 1)}},
                {{3}}, "while inside while");
}

// if (w2 == 1) { while (w0) { w0-- }; w3++ } else { w4++ }; w5++, as MPSGraph's scf.if around an scf.while
TEST_F(FacadeLoops, ALoopInAnIfPastItsCopiesFinishesTheIfAndSkipsItsElse)
{
    for (uint32_t taken : {0u, 1u})
        expectMetal(
            {{test(Op::If, 2, PredicateCompare::Equal, 1), test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0),
              dispatch(3, 1), test(Op::Else, 0), dispatch(4, 1), test(Op::EndIf, 0), dispatch(5, 1)}},
            {{1500, 0, taken}}, taken ? "taken" : "not taken");
}

// A command buffer that ends in a loop's block: the range after it holds nothing, and the command buffer completes
TEST_F(FacadeLoops, ACommandBufferEndingInALoopCompletes)
{
    expectMetal({{test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0)}}, {{kPastOutermost}}, "last");
}

// The next command buffer on the queue runs after every continuation of the one before
TEST_F(FacadeLoops, TheNextCommandBufferRunsAfterTheLoopsContinuations)
{
    expectMetal({{dispatch(1, 1), test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0)}, {dispatch(7, 1)}},
                {{kPastOutermost}, {}}, "queue order");
}

// MPSGraph's shapes as it encodes them with jump support: a loop is a While on a 0/1 word its body's last kernel writes
// (word == 1), and an If always has its Else, empty or not
constexpr PredicateCompare kIsOne = PredicateCompare::Equal;

// scf.if { scf.for past 1024 } with an empty else
TEST_F(FacadeLoops, MPSGraphsForInsideAnIfRunsPastItsCopies)
{
    for (uint32_t taken : {0u, 1u})
        expectMetal({{test(Op::If, 5, kIsOne, 1), test(Op::While, 1, kIsOne, 1), dispatch(2, 1), countdown(0, 1),
                      test(Op::EndWhile, 1), test(Op::Else, 0), test(Op::EndIf, 0), dispatch(6, 1)}},
                    {{1500, 1, 0, 0, 0, taken}}, taken ? "for in if" : "for in if, not taken");
}

// scf.for (3) { scf.while past 128 }
TEST_F(FacadeLoops, MPSGraphsWhileInsideAForRunsPastItsCopies)
{
    expectMetal({{test(Op::While, 1, kIsOne, 1), dispatch(6, 0, 3, kPastNested), dispatch(6, 0, 4, 1),
                  test(Op::While, 4, kIsOne, 1), countdown(3, 4), test(Op::EndWhile, 4), countdown(0, 1),
                  test(Op::EndWhile, 1)}},
                {{3, 1}}, "while in for");
}

// scf.for past 1024 { scf.if/else }
TEST_F(FacadeLoops, MPSGraphsIfInsideAForPastItsCopiesTakesEachSide)
{
    expectMetal({{test(Op::While, 1, kIsOne, 1), test(Op::If, 2, kIsOne, 1), dispatch(3, 1, 2, 0), test(Op::Else, 0),
                  dispatch(4, 1, 2, 1), test(Op::EndIf, 0), countdown(0, 1), test(Op::EndWhile, 1)}},
                {{1100, 1, 1}}, "if in for");
}

// A loop whose body runs over more than one compute encoder of its unit, past its copies: the encoders' own words are
// part of each copy, and the continuation starts in the first of them
TEST_F(FacadeLoops, ALoopOverItsUnitsComputeEncodersGoesOnPastItsCopies)
{
    const Op encoder{Op::Encoder};
    expectMetal({{dispatch(1, 1), test(Op::While, 0), dispatch(0, -1), encoder, dispatch(2, 1), encoder,
                  test(Op::EndWhile, 0), dispatch(3, 1)}},
                {{kPastOutermost}}, "while over encoders");
}

// MPSGraph's condition: a 4-byte shared buffer (newBufferWithLength:4 options:0) that only kernels write, a full UInt32 0
// or 1, before the block and at the end of each iteration; the CPU never reads it and never clears what it held before
constexpr uint32_t kCondition = 7, kStale = 0xdeadbeef;

TEST_F(FacadeLoops, MPSGraphsForTestsTheConditionItsKernelsWrite)
{
    expectMetal({{condition(0, kCondition), test(Op::While, kCondition, kIsOne, 1), dispatch(2, 1),
                  countdown(0, kCondition), test(Op::EndWhile, kCondition), dispatch(3, 1)}},
                {{1500, 0, 0, 0, 0, 0, 0, kStale}}, "for on its condition buffer", kCondition);
}

TEST_F(FacadeLoops, MPSGraphsIfTestsTheConditionItsKernelWrites)
{
    for (uint32_t taken : {0u, 1u}) {
        expectMetal({{condition(5, kCondition), test(Op::If, kCondition, kIsOne, 1), dispatch(2, 1), test(Op::Else, 0),
                      test(Op::EndIf, 0), dispatch(3, 1)}},
                    {{0, 0, 0, 0, 0, taken, 0, kStale}}, taken ? "if, taken" : "if, not taken", kCondition);
    }
}

// An End answers NO only once a Start of its encoder named no buffer, as AGX's do: MPSGraph aborts on any other NO
TEST_F(FacadeLoops, AnEndAnswersYesUnlessAStartNamedNoBuffer)
{
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    Encoder e = (Encoder)[cb computeCommandEncoder];
    EXPECT_TRUE([e encodeEndIf]) << "an End no block is open for";
    EXPECT_TRUE([e encodeEndWhile]);
    [e endEncoding];
    Encoder without = (Encoder)[cb computeCommandEncoder];
    [without encodeStartIf:nil offset:0 comparison:0 referenceValue:1];
    EXPECT_FALSE([without encodeEndIf]);
    [without endEncoding];
}

}  // namespace
