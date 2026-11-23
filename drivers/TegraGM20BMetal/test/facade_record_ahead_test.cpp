// SPDX-License-Identifier: MIT
// Command buffers recorded before any of them is committed, then committed and waited for, through the facade over
// the kernel model: Metal lets an app hold any number uncommitted, so recording one never waits on what only another's
// commit or completion makes (the submit worker's encode ahead of the first, the setBytes chunks the others take, the
// GPU's completions). Rounds of 24 at 8 and 16 draws a pass in scale_frames.h's shape and in gpu.path.encode_cost's,
// and one of 300, with the model completing each job as it is submitted and with the timed GPU running them. A stall
// watchdog (stall_watchdog.h) prints the submit worker's and the command buffers' state and every thread's frames.
#include "encode_cost_shape.h"
#include "stall_watchdog.h"
#include "timed_gpu.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kRounds = 40, kMany = 300;
constexpr auto kStall = std::chrono::seconds(20);

// the round's command buffers, which the watchdog reads (racing the test: it reads them only once the test stalled)
std::vector<__unsafe_unretained id<MTLCommandBuffer>> gRound;

void describe(FILE *out)
{
    const GM20BState &r = *gRM;
    fprintf(out,
            "watchdog: submitAsleep %d committed %u rangesInFlight %u waitingCommitted %u schedule %zu inflight %zu "
            "completionWaiters %u submitPromised %d\n",
            int(r.submitAsleep.load()), r.committed.load(), r.rangesInFlight.load(), r.waitingCommitted.load(),
            r.schedule.size(), r.inflight.size(), r.completionWaiters.load(), int(r.submitPromised.load()));
    for (size_t i = 0; i < gRound.size(); i++) {
        SwitchMetalCommandBuffer *cb = (SwitchMetalCommandBuffer *)gRound[i];
        const uint64_t published = cb->_published.load();
        fprintf(out,
                "watchdog: cb %zu status %d committed %d published %llu..%llu handedOnce %d declined %d veto 0x%x "
                "plan %zu segs %zu/%zu\n",
                i, int(cb->_status.load()), int(cb->_committed.load()), (unsigned long long)(published & 0xffffffff),
                (unsigned long long)(published >> 32), int(cb->_handedOnce), int(cb->_pipelineDeclined.load()),
                cb->_pipelineVeto.load(), cb->_plan.size(), cb->_segNext, cb->_segs.size());
    }
}

// `count` recorded, then committed, then waited for; false where one did not complete
bool round(stall_watchdog::Watchdog &dog, int count, int draws, bool caShape, const std::vector<id<MTLTexture>> &into)
{
    NSMutableArray<id<MTLCommandBuffer>> *recorded = [NSMutableArray arrayWithCapacity:NSUInteger(count)];
    gRound.clear();
    const scale_frames::Shape shape{encode_cost::kPasses, encode_cost::kPasses * draws, 4};
    for (int i = 0; i < count; i++) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb;
            if (caShape) {
                cb = [encode_cost::bench().queue commandBuffer];
                encode_cost::encode(cb, draws);
            } else {
                cb = scale_frames::record(encode_cost::bench().queue, scale_frames::opened().state, into, shape,
                                          uint32_t(i));
            }
            [recorded addObject:cb];
            gRound.push_back(cb);
        }
        dog.progress("record");
    }
    for (id<MTLCommandBuffer> cb in recorded) {
        [cb commit];
        dog.progress("commit");
    }
    bool completed = true;
    for (id<MTLCommandBuffer> cb in recorded) {
        [cb waitUntilCompleted];
        completed &= cb.status == MTLCommandBufferStatusCompleted;
        dog.progress("wait");
    }
    gRound.clear();
    return completed;
}

TEST(GM20BFacadeRecordAhead, CommandBuffersRecordedBeforeAnyIsCommittedNeverWait)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    const auto into = scale_frames::targets(scale_frames::opened().device, {encode_cost::kPasses, 48, 4});
    stall_watchdog::Watchdog dog(kStall, describe);
    for (const bool timed : {false, true}) {
#if __has_feature(thread_sanitizer)
        // the timed GPU's fence writes race the device's plain reads of the fence page by design (the hardware writes it)
        if (timed)
            continue;
#endif
        std::unique_ptr<timed_gpu::Gpu> gpu = timed ? std::make_unique<timed_gpu::Gpu>() : nullptr;
        encode_cost::finish(encode_cost::batch(8));
        for (int r = 0; r < kRounds; r++)
            EXPECT_TRUE(round(dog, kRounds - 1 == r ? kMany : encode_cost::kBuffers, r % 2 ? 16 : 8, r % 4 >= 2, into))
                << "round " << r << (timed ? " with the timed GPU" : "");
    }
}

}  // namespace
