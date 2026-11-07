// SPDX-License-Identifier: MIT
// nvkmd_darwin's WAIT_ANY (third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_wait_any.h) on the host, over a
// model of the kext's WaitFence (asleep until the fence interrupt announces the seqno) and of the syncs (host-signalled,
// or pending on a channel's fence): it returns once any one resolves, sleeps until then, keeps its deadline, and calls a
// passed fence whose wake never came a lost interrupt.
#include <gtest/gtest.h>

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

struct FakeConn {
    int id;
};

#define DARWIN_ANY_CONN FakeConn
#include "../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_wait_any.h"

namespace {

using std::chrono::milliseconds;

// The kext: each channel's fence word, the seqno its last interrupt announced, and WaitFence asleep until the
// announcement reaches its seqno or its timeout.
struct Kernel {
    std::mutex m;
    std::condition_variable irq;
    std::map<uint32_t, uint32_t> fence, announced;
    int error = 0;
    int calls = 0, asleep = 0;

    void complete(uint32_t channel, uint32_t seqno, bool interrupt = true)
    {
        std::lock_guard<std::mutex> l(m);
        fence[channel] = seqno;
        if (interrupt)
            announced[channel] = seqno;
        irq.notify_all();
    }
};

Kernel gKernel;

pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t gCond = PTHREAD_COND_INITIALIZER;

// A sync: host-signalled, or (channel != 0) a submission's fence
struct Sync {
    bool signaled = false;
    uint32_t channel = 0, seqno = 0;
};

std::vector<Sync> gSyncs;
std::vector<std::pair<uint32_t, uint32_t>> gLost;

bool passed(uint32_t completed, uint32_t seqno)
{
    return int32_t(completed - seqno) >= 0;
}

uint64_t now()
{
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

enum darwin_any_status status(void *, uint32_t i, struct darwin_any_fence *fence)
{
    const Sync &s = gSyncs[i];
    if (!s.channel)
        return s.signaled ? DARWIN_ANY_DONE : DARWIN_ANY_WAITING;
    {
        std::lock_guard<std::mutex> l(gKernel.m);
        if (passed(gKernel.fence[s.channel], s.seqno))
            return DARWIN_ANY_DONE;
    }
    fence->conn = {7};
    fence->channel = s.channel;
    fence->seqno = s.seqno;
    return DARWIN_ANY_FENCE;
}

bool fenceDone(const struct darwin_any_fence *f)
{
    std::lock_guard<std::mutex> l(gKernel.m);
    return passed(gKernel.fence[f->channel], f->seqno);
}

int waitFence(const struct darwin_any_fence *f, uint64_t timeoutNs)
{
    std::unique_lock<std::mutex> l(gKernel.m);
    gKernel.calls++;
    if (gKernel.error)
        return gKernel.error;
    gKernel.asleep++;
    auto announced = [&] {
        return passed(gKernel.announced[f->channel], f->seqno);
    };
    bool woken = true;
    if (timeoutNs != UINT64_MAX)
        woken = gKernel.irq.wait_for(l, std::chrono::nanoseconds(timeoutNs), announced);
    // A deadline that finds the fence passed unannounced is the kext's own lost-interrupt panic: here it never returns
    // until the announcement, as a wake that never came.
    if (timeoutNs == UINT64_MAX || (!woken && passed(gKernel.fence[f->channel], f->seqno))) {
        gKernel.irq.wait(l, announced);
        woken = true;
    }
    gKernel.asleep--;
    gKernel.irq.notify_all();
    return woken ? 0 : ETIMEDOUT;
}

int condWait(uint64_t absNs)
{
    if (absNs == UINT64_MAX)
        return pthread_cond_wait(&gCond, &gLock);
    const uint64_t at = now();
    if (absNs <= at)
        return ETIMEDOUT;
    const uint64_t rel = absNs - at;
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    const uint64_t nsec = uint64_t(abstime.tv_nsec) + rel % 1000000000ull;
    abstime.tv_sec += time_t(rel / 1000000000ull + nsec / 1000000000ull);
    abstime.tv_nsec = long(nsec % 1000000000ull);
    return pthread_cond_timedwait(&gCond, &gLock, &abstime);
}

void lost(const struct darwin_any_fence *f)
{
    gLost.emplace_back(f->channel, f->seqno);
}

const struct darwin_any_ops kOps = {&gLock, &gCond, status, fenceDone, waitFence, condWait, now, lost};

void signal(size_t i)
{
    pthread_mutex_lock(&gLock);
    gSyncs[i].signaled = true;
    pthread_cond_broadcast(&gCond);
    pthread_mutex_unlock(&gLock);
}

class WaitAny : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::lock_guard<std::mutex> l(gKernel.m);
        gKernel.fence.clear();
        gKernel.announced.clear();
        gKernel.error = 0;
        gKernel.calls = 0;
        gSyncs.clear();
        gLost.clear();
    }

    // Every watcher wakes and leaves before the next test reuses the model.
    void TearDown() override
    {
        std::unique_lock<std::mutex> l(gKernel.m);
        for (auto &[channel, seqno] : gKernel.fence)
            gKernel.announced[channel] = gKernel.fence[channel] = UINT32_MAX / 2;
        gKernel.irq.notify_all();
        gKernel.irq.wait(l, [] {
            return gKernel.asleep == 0;
        });
        l.unlock();
        pthread_mutex_lock(&gLock);
        pthread_mutex_unlock(&gLock);
    }

    // The wait over gSyncs, `deadline` from now (none: no deadline); the result and how long it took
    std::pair<int, milliseconds> wait(milliseconds deadline = milliseconds(-1))
    {
        const uint64_t start = now();
        const uint64_t abs = deadline.count() < 0 ? UINT64_MAX : start + uint64_t(deadline.count()) * 1000000;
        pthread_mutex_lock(&gLock);
        const int ret = darwin_wait_any_locked(&kOps, nullptr, uint32_t(gSyncs.size()), abs);
        pthread_mutex_unlock(&gLock);
        return {ret, milliseconds((now() - start) / 1000000)};
    }

    static Sync fence(uint32_t channel, uint32_t seqno)
    {
        Sync s;
        s.channel = channel;
        s.seqno = seqno;
        return s;
    }
};

TEST_F(WaitAny, ReturnsOnceAHostSignalResolvesOneOfSeveral)
{
    gSyncs = {Sync(), fence(1, 5), Sync()};
    std::thread later([] {
        std::this_thread::sleep_for(milliseconds(40));
        signal(2);
    });
    const auto [ret, took] = wait(milliseconds(5000));
    later.join();
    EXPECT_EQ(ret, 0);
    EXPECT_GE(took.count(), 35);
    EXPECT_LT(took.count(), 2000);
    EXPECT_TRUE(gLost.empty());
}

TEST_F(WaitAny, ReturnsOnceOneOfSeveralFencesPasses)
{
    gSyncs = {Sync(), fence(1, 5), fence(2, 3)};
    std::thread later([] {
        std::this_thread::sleep_for(milliseconds(40));
        gKernel.complete(2, 3);
    });
    const auto [ret, took] = wait();
    later.join();
    EXPECT_EQ(ret, 0);
    EXPECT_GE(took.count(), 35);
    EXPECT_LT(took.count(), 2000);
    EXPECT_EQ(gKernel.calls, 2);
    EXPECT_TRUE(gLost.empty());
}

TEST_F(WaitAny, SleepsThroughOtherWakesUntilItsDeadline)
{
    gSyncs = {Sync(), fence(1, 5)};
    gKernel.complete(1, 4);
    std::thread others([] {
        for (int i = 0; i < 5; i++) {
            std::this_thread::sleep_for(milliseconds(20));
            pthread_mutex_lock(&gLock);
            pthread_cond_broadcast(&gCond);
            pthread_mutex_unlock(&gLock);
        }
    });
    const auto [ret, took] = wait(milliseconds(200));
    others.join();
    EXPECT_EQ(ret, ETIMEDOUT);
    EXPECT_GE(took.count(), 195);
    EXPECT_EQ(gKernel.calls, 1);  // one watcher: a wake that resolves nothing starts no other
    EXPECT_TRUE(gLost.empty());
}

TEST_F(WaitAny, KeepsItsDeadline)
{
    gSyncs = {Sync(), fence(1, 5)};
    const auto [ret, took] = wait(milliseconds(80));
    EXPECT_EQ(ret, ETIMEDOUT);
    EXPECT_GE(took.count(), 75);
    EXPECT_LT(took.count(), 1000);
    const auto [again, none] = wait(milliseconds(0));
    EXPECT_EQ(again, ETIMEDOUT);
    EXPECT_LT(none.count(), 100);
}

TEST_F(WaitAny, OneResolvedAlreadyReturnsWithoutAWatcher)
{
    gSyncs = {fence(1, 5), Sync()};
    gKernel.complete(1, 6);
    EXPECT_EQ(wait(milliseconds(1000)).first, 0);
    EXPECT_EQ(gKernel.calls, 0);
    gSyncs = {Sync(), Sync()};
    gSyncs[1].signaled = true;
    EXPECT_EQ(wait().first, 0);
}

TEST_F(WaitAny, AFencePassedWithNoWakeIsALostInterrupt)
{
    gSyncs = {Sync(), fence(1, 5)};
    std::thread later([] {
        std::this_thread::sleep_for(milliseconds(30));
        gKernel.complete(1, 5, false);
    });
    const auto [ret, took] = wait(milliseconds(150));
    later.join();
    EXPECT_EQ(ret, 0);
    EXPECT_GE(took.count(), 150 + 45);
    EXPECT_EQ(gLost, (std::vector<std::pair<uint32_t, uint32_t>>{{1, 5}}));
}

TEST_F(WaitAny, AWaitFenceErrorEndsTheWait)
{
    gSyncs = {Sync(), fence(1, 5)};
    gKernel.error = EIO;
    EXPECT_EQ(wait(milliseconds(5000)).first, EIO);
}

}  // namespace
