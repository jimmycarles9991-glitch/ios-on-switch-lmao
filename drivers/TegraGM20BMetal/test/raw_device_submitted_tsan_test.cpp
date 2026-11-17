// SPDX-License-Identifier: MIT
// The device's last submitted seqno, read by a thread that does not submit while command buffers commit, thread-sanitized,
// on a client device over the kernel model. The memory release's purge of idle heaps (RawMetal.mm's GpuMemoryRelease,
// Device::purgeArenas) reads it under the device lock, which the submitting thread does not take: NB ios27's
// raw_facade_handlers_tsan_test reported that read against CommandBuffer::commit's store.
#include "raw_kernel_model.h"
#include "empty_pack.h"
#include "../raw/runtime/raw_gpu.h"

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>

namespace {

constexpr int kCommits = 400;

class DeviceSubmitted : public testing::Test {
protected:
    raw::Device dev;
    raw::Buffer from, to;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createBuffer(4096, from));
        ASSERT_TRUE(dev.createBuffer(4096, to));
    }

    // One command buffer of one copy: its seqno, 0 when it was not submitted
    uint32_t copy()
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0;
        cb->use(from, false);
        cb->use(to, true, false);
        e->copyBytes(from.va, to.va, 256);
        e->end();
        uint32_t seqno = 0;
        return cb->commit(seqno) ? seqno : 0;
    }
};

TEST_F(DeviceSubmitted, TheMemoryReleasesPurgeReadsWhatTheSubmitterLastSubmitted)
{
    ASSERT_NE(copy(), 0u);
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> committed{0};
    std::atomic<int> purges{0};
    std::thread release([&] {
        while (!stop.load(std::memory_order_acquire)) {
            dev.purgeArenas(committed);
            purges.fetch_add(1, std::memory_order_relaxed);
        }
    });
    uint32_t last = 0;
    for (int i = 0; i < kCommits; i++) {
        const uint32_t seqno = copy();
        EXPECT_NE(seqno, 0u);
        EXPECT_GT(int32_t(seqno - last), 0);
        last = seqno;
    }
    stop.store(true, std::memory_order_release);
    release.join();
    EXPECT_GT(purges.load(), 0);
    EXPECT_EQ(dev.submitted(), last);
}

}  // namespace
