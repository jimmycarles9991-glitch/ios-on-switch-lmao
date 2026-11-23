// SPDX-License-Identifier: MIT
// TextureTransfers from several threads, some held inside their WaitFence (transfer_fake.h): the others' copies go on
// meanwhile, never in a slot a held thread still owns (a new slot when every one is held), and a readback whose channel
// another thread reset while it waited fails rather than judge its copy by a fence record the new channel holds.
#include "transfer_fake.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// The first `count` WaitFences held on their threads until release(); the threads it started are let go and joined at its end
class HeldWaits {
public:
    HeldWaits(FakeGpu &gpu, unsigned count) : gpu_(gpu), count_(count)
    {
        gpu.onWait = [this] {
            std::unique_lock<std::mutex> l(lock_);
            if (taken_ == count_)
                return;
            ++taken_;
            changed_.notify_all();
            changed_.wait(l, [&] {
                return released_;
            });
        };
    }

    ~HeldWaits()
    {
        release();
        for (std::thread &t : threads_)
            t.join();
        gpu_.onWait = {};
    }

    void start(std::function<void()> work)
    {
        threads_.emplace_back(std::move(work));
    }

    bool reached()
    {
        std::unique_lock<std::mutex> l(lock_);
        return changed_.wait_for(l, 5s, [&] {
            return taken_ == count_;
        });
    }

    void release()
    {
        std::lock_guard<std::mutex> l(lock_);
        released_ = true;
        changed_.notify_all();
    }

    void join()
    {
        release();
        for (std::thread &t : threads_)
            t.join();
        threads_.clear();
    }

private:
    FakeGpu &gpu_;
    const unsigned count_;
    std::vector<std::thread> threads_;
    std::mutex lock_;
    std::condition_variable changed_;
    unsigned taken_ = 0;
    bool released_ = false;
};

template <typename T> uint64_t at(T &v)
{
    return reinterpret_cast<uint64_t>(v.data());
}

TEST_F(GM20BTransfer, AnUploadDoesNotWaitForAnotherThreadsReadback)
{
    std::vector<uint8_t> source(128, 0x5c), read(128, 0), target(128, 0), bytes(128, 0x21);
    const gm20b::TextureTransfer from{{at(source), 32}, 8, 4, 1, 4}, into{{at(target), 32}, 8, 4, 1, 4};
    HeldWaits held(gpu, 1);
    bool readBack = false;
    held.start([&] {
        readBack = copy(from, read.data(), 32, false);
    });
    ASSERT_TRUE(held.reached());
    auto upload = std::async(std::launch::async, [&] {
        return copy(into, bytes.data(), 32, true);
    });
    const bool uploaded = upload.wait_for(2s) == std::future_status::ready;
    held.join();
    EXPECT_TRUE(uploaded) << "the upload waited for the other thread's readback";
    EXPECT_TRUE(upload.get());
    EXPECT_TRUE(readBack);
    EXPECT_EQ(read, source);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

// Four uploads fill the slots, none complete: the fifth waits for slot 0's copy, held; the sixth, on another thread, waits
// only for slot 1's (the GPU runs them in order)
TEST_F(GM20BTransfer, AnUploadIntoAReusedSlotDoesNotWaitForAnotherThreadsWait)
{
    std::vector<uint8_t> first(16, 0), second(16, 0), filler(16, 0), bytes(16, 0x44), others(16, 0x67);
    for (uint32_t i = 0; i < 4; ++i)
        ASSERT_TRUE(copy({{at(filler), 16}, 4, 1, 1, 4}, bytes.data(), 16, true));
    HeldWaits held(gpu, 1);
    bool firstDone = false;
    held.start([&] {
        firstDone = copy({{at(first), 16}, 4, 1, 1, 4}, bytes.data(), 16, true);
    });
    ASSERT_TRUE(held.reached());
    auto upload = std::async(std::launch::async, [&] {
        return copy({{at(second), 16}, 4, 1, 1, 4}, others.data(), 16, true);
    });
    const bool uploaded = upload.wait_for(2s) == std::future_status::ready;
    held.join();
    EXPECT_TRUE(uploaded) << "the upload waited for the other thread's WaitFence";
    EXPECT_TRUE(upload.get());
    EXPECT_TRUE(firstDone);
    EXPECT_EQ(gpu.allocations, 4u);
    EXPECT_EQ(gpu.waits, 2u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(first, bytes);
    EXPECT_EQ(second, others);
}

// A readback waits for its copy, held, while another thread's uploads go round the slots: none takes the readback's slot,
// whose staging it has still to read
TEST_F(GM20BTransfer, ACopyNeverTakesTheSlotOfAnotherThreadsReadback)
{
    std::vector<uint8_t> source(16, 0x3a), read(16, 0), target(16, 0), bytes(16, 0x52);
    HeldWaits held(gpu, 1);
    bool readBack = false;
    held.start([&] {
        readBack = copy({{at(source), 16}, 4, 1, 1, 4}, read.data(), 16, false);
    });
    ASSERT_TRUE(held.reached());
    auto uploads = std::async(std::launch::async, [&] {
        bool all = true;
        for (uint32_t i = 0; i < 4; ++i)
            all &= copy({{at(target), 16}, 4, 1, 1, 4}, bytes.data(), 16, true);
        return all;
    });
    const bool uploaded = uploads.wait_for(2s) == std::future_status::ready;
    held.join();
    EXPECT_TRUE(uploaded) << "the uploads waited for the other thread's readback";
    EXPECT_TRUE(uploads.get());
    EXPECT_TRUE(readBack);
    EXPECT_EQ(read, source) << "an upload reused the slot the readback was waiting on";
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

// Four readbacks hold every slot while they wait: a fifth copy takes a slot of its own and does not wait
TEST_F(GM20BTransfer, WhenEverySlotIsHeldACopyTakesANewOne)
{
    std::array<std::vector<uint8_t>, 4> sources, reads;
    std::array<bool, 4> readBack{};
    HeldWaits held(gpu, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        sources[i].assign(16, uint8_t(0x10 + i));
        reads[i].assign(16, 0);
        held.start([&, i] {
            readBack[i] = copy({{at(sources[i]), 16}, 4, 1, 1, 4}, reads[i].data(), 16, false);
        });
    }
    ASSERT_TRUE(held.reached());
    std::vector<uint8_t> target(16, 0), bytes(16, 0x77);
    auto upload = std::async(std::launch::async, [&] {
        return copy({{at(target), 16}, 4, 1, 1, 4}, bytes.data(), 16, true);
    });
    const bool uploaded = upload.wait_for(2s) == std::future_status::ready;
    held.join();
    EXPECT_TRUE(uploaded) << "the copy waited for a held slot";
    EXPECT_TRUE(upload.get());
    EXPECT_EQ(gpu.allocations, 5u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(readBack[i]);
        EXPECT_EQ(reads[i], sources[i]);
    }
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

// A readback waits, held; its channel is killed and another thread's upload resets it and makes a new one, whose wait
// answers for the old seqno: the readback fails and leaves the caller's bytes alone
TEST_F(GM20BTransfer, AReadbackWhoseChannelAnotherThreadResetFails)
{
    std::vector<uint8_t> source(16, 0x3a), read(16, 0xee), target(16, 0), bytes(16, 0x52);
    HeldWaits held(gpu, 1);
    auto owner = std::make_shared<int>(4);
    std::weak_ptr<int> kept = owner;
    bool readBack = true;
    held.start([&, owner = std::move(owner)]() mutable {
        readBack = copy({{at(source), 16}, 4, 1, 1, 4}, read.data(), 16, false, std::move(owner));
    });
    ASSERT_TRUE(held.reached());
    {
        std::lock_guard<std::mutex> l(gpu.lock);
        gpu.fence.error = 1;
    }
    auto upload = std::async(std::launch::async, [&] {
        return copy({{at(target), 16}, 4, 1, 1, 4}, bytes.data(), 16, true);
    });
    const bool uploaded = upload.wait_for(2s) == std::future_status::ready;
    held.join();
    EXPECT_TRUE(uploaded) << "the upload waited for the other thread's readback";
    EXPECT_TRUE(upload.get());
    EXPECT_FALSE(readBack) << "the readback trusted a channel another thread had reset";
    EXPECT_EQ(read, std::vector<uint8_t>(16, 0xee));
    EXPECT_TRUE(kept.expired());
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

}  // namespace
