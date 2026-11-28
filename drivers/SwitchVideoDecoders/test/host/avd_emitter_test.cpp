// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "avd_emitter_harness.h"
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TegraNVDECSubmit makeSubmit(uint32_t statusHandle)
{
    TegraNVDECSubmit s = {};
    s.entry_count = 1;
    s.entries[0] = {NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, statusHandle, 0};
    return s;
}

class AVDEmitterTest : public testing::Test {
protected:
    alignas(64) uint8_t statusMem[256] = {};
    AVDEmitter *e = nullptr;
    VTVideoDecoderSession fakeSession = reinterpret_cast<VTVideoDecoderSession>(0x1234);

    void SetUp() override
    {
        avd_emitter_harness_reset();
        avd_emitter_harness_set_status_buffer(1, statusMem);
        e = avd_emitter_create(nullptr);
        ASSERT_NE(e, nullptr);
        avd_emitter_set_session(e, fakeSession);
    }

    void TearDown() override
    {
        avd_emitter_destroy(e);
        e = nullptr;
    }
};

TEST_F(AVDEmitterTest, SteadyStatePoolsOutputsTakesNoBroadcastsAndSkipsLocksOnFastPaths)
{
    const TegraNVDECSubmit submit = makeSubmit(1);
    for (uint32_t i = 1; i <= 200; ++i) {
        avd_emitter_submitted(e, i, &submit, false);

        const uint32_t locksBeforeOutput = avd_emitter_harness_stats().lockAcquisitions;
        Output *out = avd_emitter_output(e, reinterpret_cast<VTVideoDecoderFrame>(uintptr_t(i)), i, 0);
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(avd_emitter_harness_stats().lockAcquisitions, locksBeforeOutput)
            << "avd_emitter_output must not acquire e->lock";

        avd_emitter_push(e, out);

        const uint32_t locksBeforeDrain = avd_emitter_harness_stats().lockAcquisitions;
        avd_emitter_drain(e);
        EXPECT_EQ(avd_emitter_harness_stats().lockAcquisitions, locksBeforeDrain)
            << "idle avd_emitter_drain must not acquire e->lock";

        const uint32_t locksBeforeComplete = avd_emitter_harness_stats().lockAcquisitions;
        avd_emitter_completed(e, i, 0, 0, 10);
        const AVDEmitterHarnessStats stats = avd_emitter_harness_stats();
        EXPECT_EQ(stats.lockAcquisitions - locksBeforeComplete, 2u);
        EXPECT_EQ(stats.broadcasts, 0u) << "steady-state drain with no waiters must not broadcast";
        EXPECT_EQ(stats.emittedCount, i);
    }
    const AVDEmitterHarnessStats finalStats = avd_emitter_harness_stats();
    EXPECT_EQ(finalStats.outputHeapAllocs, 0u) << "all Outputs must come from the embedded pool";
    EXPECT_EQ(finalStats.broadcasts, 0u);
}

TEST_F(AVDEmitterTest, PoolOverflowFallsBackToHeapAndSettleOutputReturnsToPool)
{
    const TegraNVDECSubmit submit = makeSubmit(1);
    std::vector<Output *> outputs;
    avd_emitter_submitted(e, 1, &submit, false);
    for (uint32_t i = 1; i <= 70; ++i) {
        Output *out = avd_emitter_output(e, reinterpret_cast<VTVideoDecoderFrame>(uintptr_t(i)), 1, 0);
        ASSERT_NE(out, nullptr);
        outputs.push_back(out);
    }
    EXPECT_EQ(avd_emitter_harness_stats().outputHeapAllocs, 6u);
    for (Output *out : outputs)
        avd_emitter_push(e, out);

    avd_emitter_completed(e, 1, 0, 0, 1);
    EXPECT_EQ(avd_emitter_harness_stats().emittedCount, 70u);

    avd_emitter_submitted(e, 2, &submit, false);
    const uint32_t allocsBeforeTile = avd_emitter_harness_stats().outputHeapAllocs;
    Output *tile = avd_emitter_output(e, reinterpret_cast<VTVideoDecoderFrame>(uintptr_t(99)), 2, 0);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(avd_emitter_harness_stats().outputHeapAllocs, allocsBeforeTile);
    EXPECT_EQ(avd_emitter_settle_output(e, tile), noErr);
}

TEST_F(AVDEmitterTest, BroadcastFiresOnlyWhenSettleOrCloseWaitsForActiveDrain)
{
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool inCallback = false;
        bool release = false;
    } gate;

    avd_emitter_harness_set_emit_hook(
        [](void *, OSStatus, void *ctx) {
            auto *g = static_cast<Gate *>(ctx);
            std::unique_lock<std::mutex> lk(g->m);
            g->inCallback = true;
            g->cv.notify_all();
            g->cv.wait(lk, [&] {
                return g->release;
            });
        },
        &gate);

    const TegraNVDECSubmit submit = makeSubmit(1);
    avd_emitter_submitted(e, 1, &submit, false);
    Output *out = avd_emitter_output(e, reinterpret_cast<VTVideoDecoderFrame>(uintptr_t(1)), 1, 0);
    ASSERT_NE(out, nullptr);
    avd_emitter_push(e, out);

    std::thread completer([&] {
        avd_emitter_completed(e, 1, 0, 0, 1);
    });
    {
        std::unique_lock<std::mutex> lk(gate.m);
        gate.cv.wait(lk, [&] {
            return gate.inCallback;
        });
    }

    std::atomic<bool> settled{false};
    std::thread settler([&] {
        avd_emitter_settle(e, true);
        settled.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(settled.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lk(gate.m);
        gate.release = true;
    }
    gate.cv.notify_all();
    completer.join();
    settler.join();

    EXPECT_TRUE(settled.load(std::memory_order_acquire));
    EXPECT_GE(avd_emitter_harness_stats().broadcasts, 1u);
}

TEST_F(AVDEmitterTest, ConcurrentSubmitPushCompleteAndSettle)
{
    const TegraNVDECSubmit submit = makeSubmit(1);
    constexpr uint32_t kFrames = 256;
    avd_emitter_harness_set_wait_hook(
        [](uint32_t fence, void *) -> int {
            return (fence % 37 == 0) ? -5 : 0;
        },
        nullptr);
    std::atomic<uint32_t> submitted{0};
    std::thread completer([&] {
        uint32_t done = 0;
        while (done < kFrames) {
            const uint32_t avail = submitted.load(std::memory_order_acquire);
            while (done < avail) {
                ++done;
                avd_emitter_completed(e, done, (done % 37 == 0) ? -5 : 0, 0, 1);
            }
            std::this_thread::yield();
        }
    });

    for (uint32_t i = 1; i <= kFrames; ++i) {
        avd_emitter_submitted(e, i, &submit, false);
        submitted.store(i, std::memory_order_release);
        Output *out = avd_emitter_output(e, reinterpret_cast<VTVideoDecoderFrame>(uintptr_t(i)), i, 0);
        ASSERT_NE(out, nullptr);
        avd_emitter_push(e, out);
        avd_emitter_drain(e);
        if (i % 32 == 0)
            avd_emitter_settle(e, false);
    }
    completer.join();
    avd_emitter_settle(e, true);
    EXPECT_EQ(avd_emitter_harness_stats().emittedCount, kFrames);
    EXPECT_EQ(avd_emitter_errors(e), kFrames / 37);
}

TEST(AVDHotPathWiring, DecoderUsesSingleOwnerGateAndZeroCopySubmitPacket)
{
    const std::string decoder = SourceText("../../AVD/AVDDecoder.c");
    const std::string decodeFrame = Body(decoder, "static OSStatus decodeFrame(");
    EXPECT_THAT(decodeFrame, HasSubstr("avd_decoder_enter(d);"));
    EXPECT_THAT(decodeFrame, HasSubstr("avd_decoder_leave(d);"));
    EXPECT_THAT(decodeFrame, Not(HasSubstr("pthread_mutex_lock(&d->lock)")));

    const std::string placePicture = Body(decoder, "void avd_place_picture(");
    EXPECT_THAT(placePicture, HasSubstr("#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG"));

    const std::string client = SourceText("../../AVD/NVDECClient.c");
    const std::string submit = Body(client, "int nvdec_submit(");
    EXPECT_THAT(submit, HasSubstr("&c->submitPacket.submit"));
    EXPECT_THAT(submit, HasSubstr("IOConnectCallStructMethod("));
}

TEST(AVDDecoderGate, UncontendedFastPathTakesNoMutexAndContendedSerializesCleanly)
{
    AVDGateHarness *g = avd_gate_harness_create();
    ASSERT_NE(g, nullptr);
    for (int i = 0; i < 100; ++i) {
        avd_gate_harness_enter(g);
        avd_gate_harness_leave(g);
    }
    EXPECT_EQ(avd_gate_harness_slow_locks(g), 0u) << "uncontended frames must not touch d->lock";

    uint32_t sharedState = 0;
    std::atomic<int> inCritical{0};
    constexpr int kThreads = 4, kIters = 400;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                avd_gate_harness_enter(g);
                EXPECT_EQ(inCritical.fetch_add(1, std::memory_order_relaxed), 0);
                ++sharedState;
                if ((i & 15) == 0)
                    std::this_thread::yield();
                EXPECT_EQ(inCritical.fetch_sub(1, std::memory_order_relaxed), 1);
                avd_gate_harness_leave(g);
            }
        });
    }
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(sharedState, static_cast<uint32_t>(kThreads * kIters));
    avd_gate_harness_destroy(g);
}
}  // namespace
