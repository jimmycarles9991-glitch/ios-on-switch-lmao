// SPDX-License-Identifier: BSD-2-Clause-Patent
// A jobless bridge's failure released on another thread while the display publishes its stream, in every order the
// two threads find: the stream either is refused with the failure, or is published first and cancelled by the release
// before it can write. Run under ThreadSanitizer too (switch_host1x_publish_race_tsan_test).
#include "switch_host1x_submit_harness.h"

#include <cstdio>
#include <thread>

namespace {

TEST_F(Host1xSubmit, AFailureReleasedWhileTheStreamIsPublishedNeverLetsItWrite)
{
    static SwitchHost1xDisplayStream stream;
    static uint32_t bridgeSyncpt = 0;
    if (!bridgeSyncpt) {
        ASSERT_EQ(switch_host1x_display_start(&stream), 0);
        ASSERT_EQ(tegra_host1x_syncpt_alloc(&bridgeSyncpt), 0);
    }
    gCpuIncrements = true;
    unsigned refused = 0, cancelled = 0;
    for (unsigned round = 0; round < 400; ++round) {
        switch_gpu_bridge bridge = {bridgeSyncpt, syncpoint(bridgeSyncpt) + 2, 0, 0, false};
        const uint32_t word = 0xdc0, address = 0x20000000;
        DisplayCompletion done;
        int submitted = 1;
        bool go = false;
        // Odd rounds hold the display back, even ones the producer, by a varying number of yields.
        const unsigned displayWaits = round % 2 ? round % 64 : 0, producerWaits = round % 2 ? 0 : round % 64;
        const auto start = [&go](unsigned waits) {
            while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
                std::this_thread::yield();
            for (; waits; --waits)
                std::this_thread::yield();
        };
        std::thread display([&] {
            start(displayWaits);
            submitted = switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5,
                                                     DisplayCompletion::heard, &done, &bridge);
        });
        std::thread producer([&] {
            start(producerWaits);
            __atomic_store_n(&bridge.failed, SWITCH_GPU_BACKING_WRITER_GONE, __ATOMIC_RELEASE);
            EXPECT_EQ(tegra_host1x_syncpt_release_failed(bridge.syncpoint, bridge.threshold, 2), 0);
        });
        __atomic_store_n(&go, true, __ATOMIC_RELEASE);
        display.join();
        producer.join();
        if (submitted) {
            ++refused;
            EXPECT_EQ(submitted, SWITCH_GPU_BACKING_WRITER_GONE) << round;
            EXPECT_EQ(bridge.releases, 0u) << round;
            continue;
        }
        ++cancelled;
        EXPECT_EQ(stream.status, SwitchHost1xDisplayStream::kCancelled) << round << ": published and not cancelled";
        stream.fence.signaled(&stream.fence, 0);
        EXPECT_EQ(done.calls, 1u) << round;
        EXPECT_EQ(bridge.releases, 1u) << round;
    }
    EXPECT_EQ(refused + cancelled, 400u);
    EXPECT_GT(refused, 0u) << "the release before the publication";
    EXPECT_GT(cancelled, 0u) << "the publication before the release";
    std::printf("orders: refused %u, published then cancelled %u\n", refused, cancelled);
}

TEST_F(Host1xSubmit, AGatedBridgeCancelledWhileTheStreamIsPublishedNeverLetsItWrite)
{
    static SwitchHost1xDisplayStream stream;
    static uint32_t bridgeSyncpt = 0;
    if (!bridgeSyncpt) {
        ASSERT_EQ(switch_host1x_display_start(&stream), 0);
        ASSERT_EQ(tegra_host1x_syncpt_alloc(&bridgeSyncpt), 0);
    }
    gCpuIncrements = true;
    unsigned refused = 0, cancelled = 0;
    for (unsigned round = 0; round < 400; ++round) {
        const uint32_t base = syncpoint(bridgeSyncpt);
        switch_gpu_bridge bridge = {bridgeSyncpt, base + 2, 0, 0, false};
        const uint32_t word = 0xdc0, address = 0x20000000;
        DisplayCompletion done;
        int submitted = 1;
        bool go = false;
        const unsigned displayWaits = round % 2 ? round % 64 : 0, producerWaits = round % 2 ? 0 : round % 64;
        const auto start = [&go](unsigned waits) {
            while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
                std::this_thread::yield();
            for (; waits; --waits)
                std::this_thread::yield();
        };
        std::thread display([&] {
            start(displayWaits);
            submitted = switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5,
                                                     DisplayCompletion::heard, &done, &bridge);
        });
        std::thread gate([&] {
            start(producerWaits);
            __atomic_store_n(&bridge.failed, SWITCH_GPU_BACKING_WRITER_GONE, __ATOMIC_RELEASE);
            EXPECT_EQ(tegra_host1x_syncpt_cancel_waiters(bridge.syncpoint, bridge.threshold), 0);
        });
        __atomic_store_n(&go, true, __ATOMIC_RELEASE);
        display.join();
        gate.join();
        EXPECT_EQ(syncpoint(bridgeSyncpt), base) << round << ": cancel_waiters makes no CPU increment";
        if (submitted) {
            ++refused;
            EXPECT_EQ(submitted, SWITCH_GPU_BACKING_WRITER_GONE) << round;
            EXPECT_EQ(bridge.releases, 0u) << round;
            continue;
        }
        ++cancelled;
        EXPECT_EQ(stream.status, SwitchHost1xDisplayStream::kCancelled) << round << ": published and not cancelled";
        stream.fence.signaled(&stream.fence, 0);
        EXPECT_EQ(done.calls, 1u) << round;
        EXPECT_EQ(bridge.releases, 1u) << round;
    }
    EXPECT_EQ(refused + cancelled, 400u);
    EXPECT_GT(refused, 0u) << "gate cancel before publication";
    EXPECT_GT(cancelled, 0u) << "publication before gate cancel";
    std::printf("gate orders: refused %u, published then cancelled %u\n", refused, cancelled);
}

}  // namespace
