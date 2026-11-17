// SPDX-License-Identifier: MIT
// compiler/compiler_pool.h: one compile per record however many processes ask, at the class of its most urgent waiter
// (raised while it runs when a more urgent one joins), background for work nobody waits for, and at most two compiles
// above the background class at once. Its waiters are answered at that class, and a parked worker wakes at the
// interactive class: boot 22's compiler answered from a worker already dropped to background, a state compiled in 216 ms
// reaching its client 8.1 s later while the system was busy.
#include "../compiler/compiler_pool.h"

#include <gtest/gtest.h>

#include <pthread/qos.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace {

using switch_compiler::Band;
using switch_compiler::Name;
using switch_compiler::Pool;
using switch_compiler::Waiter;
using switch_compiler::Work;

Name nameOf(uint8_t n)
{
    Name name{};
    name[0] = n;
    return name;
}

struct Item : Work {
    uint8_t n = 0;
};

// A pool whose compiles each wait to be let go, and which records what the pool told the hooks
class Harness {
public:
    std::mutex lock;
    std::condition_variable changed;
    std::map<uint8_t, bool> released;
    std::map<uint8_t, unsigned> runs;
    std::map<uint64_t, int32_t> completed;
    std::map<uint8_t, Band> startBand, pickedAt;
    std::map<uint64_t, Band> answeredAt;  // the answering thread's class, or a raise on it not yet lowered
    std::map<uintptr_t, Band> liveRaises;
    std::vector<Band> idleBands;
    std::vector<uint8_t> started;
    std::vector<Band> raises;
    std::vector<size_t> idles;  // how many waiters were answered at each idle
    unsigned lowered = 0, running = 0, mostRunning = 0, mostForeground = 0, foreground = 0;
    std::unique_ptr<Pool> pool;
    thread_local static Band band, before;

    explicit Harness(unsigned workers = Pool::kWorkers)
    {
        Pool::Hooks hooks;
        hooks.setBand = [](Band b) {
            before = band;
            band = b;
        };
        hooks.raise = [this](pthread_t, Band b) -> void * {
            std::lock_guard<std::mutex> l(lock);
            raises.push_back(b);
            liveRaises[raises.size()] = b;
            return reinterpret_cast<void *>(uintptr_t(raises.size()));
        };
        hooks.lower = [this](void *token) {
            std::lock_guard<std::mutex> l(lock);
            lowered++;
            liveRaises.erase(uintptr_t(token));
        };
        hooks.complete = [this](const Waiter &w, int32_t result) {
            std::lock_guard<std::mutex> l(lock);
            completed[w.id] = result;
            Band in = band;
            for (const auto &[token, raised] : liveRaises)
                in = std::max(in, raised);
            answeredAt[w.id] = in;
            changed.notify_all();
        };
        hooks.idle = [this] {
            std::lock_guard<std::mutex> l(lock);
            idles.push_back(completed.size());
            idleBands.push_back(band);
            changed.notify_all();
        };
        hooks.run = [this](Work &work, const std::function<void(int32_t)> &primary) {
            const uint8_t n = static_cast<Item &>(work).n;
            const bool above = band > Band::Background;
            {
                std::unique_lock<std::mutex> l(lock);
                runs[n]++;
                startBand[n] = band;
                pickedAt[n] = before;
                started.push_back(n);
                running++;
                foreground += above;
                mostRunning = std::max(mostRunning, running);
                mostForeground = std::max(mostForeground, foreground);
                changed.notify_all();
                changed.wait(l, [&] {
                    return released[n];
                });
                running--;
                foreground -= above;
            }
            primary(int32_t(n));
            return int32_t(n) + 100;
        };
        pool = std::make_unique<Pool>(std::move(hooks), workers);
    }

    bool submit(uint8_t n, uint64_t waiter, Band b, bool split = false, uint64_t client = 0)
    {
        auto item = std::make_unique<Item>();
        item->n = n;
        return pool->submit(nameOf(n), std::move(item), Waiter{waiter, client ? client : waiter, b, split});
    }

    void release(uint8_t n)
    {
        std::lock_guard<std::mutex> l(lock);
        released[n] = true;
        changed.notify_all();
    }

    void waitStarted(size_t count)
    {
        std::unique_lock<std::mutex> l(lock);
        changed.wait(l, [&] {
            return started.size() >= count;
        });
    }

    void waitRunning(unsigned count)
    {
        std::unique_lock<std::mutex> l(lock);
        changed.wait(l, [&] {
            return running >= count;
        });
    }

    void waitIdles(size_t count)
    {
        std::unique_lock<std::mutex> l(lock);
        changed.wait(l, [&] {
            return idles.size() >= count;
        });
    }
};

thread_local Band Harness::band = Band::Background;
thread_local Band Harness::before = Band::Background;

TEST(ShaderCompilerPool, IdenticalRequestsFromSeveralProcessesAreOneCompile)
{
    Harness h;
    EXPECT_TRUE(h.submit(1, 11, Band::Initiated));
    h.waitRunning(1);
    EXPECT_FALSE(h.submit(1, 12, Band::Initiated));
    EXPECT_FALSE(h.submit(1, 13, Band::Utility));
    h.release(1);
    h.pool->drain();
    EXPECT_EQ(h.runs[1], 1u);
    EXPECT_EQ(h.completed[11], 1);
    EXPECT_EQ(h.completed[12], 1);
    EXPECT_EQ(h.completed[13], 1);
    EXPECT_EQ(h.pool->stats().joined, 2u);
}

TEST(ShaderCompilerPool, AWaitedRequestRunsAtItsWaitersClassAndAnUnwaitedOneInTheBackground)
{
    EXPECT_EQ(switch_compiler::bandOf(QOS_CLASS_USER_INTERACTIVE, true), Band::Interactive);
    EXPECT_EQ(switch_compiler::bandOf(QOS_CLASS_UTILITY, true), Band::Utility);
    EXPECT_EQ(switch_compiler::bandOf(QOS_CLASS_UNSPECIFIED, true), Band::Interactive);  // a fixed-priority waiter
    EXPECT_EQ(switch_compiler::bandOf(QOS_CLASS_USER_INTERACTIVE, false), Band::Background);
    Harness h;
    h.submit(1, 11, switch_compiler::bandOf(QOS_CLASS_USER_INITIATED, true));
    h.submit(2, 12, switch_compiler::bandOf(QOS_CLASS_USER_INTERACTIVE, false));
    h.release(1);
    h.release(2);
    h.pool->drain();
    EXPECT_EQ(h.startBand[1], Band::Initiated);
    EXPECT_EQ(h.startBand[2], Band::Background);
    EXPECT_EQ(switch_compiler::qosOf(Band::Initiated), uint32_t(QOS_CLASS_USER_INITIATED));
}

TEST(ShaderCompilerPool, AMoreUrgentWaiterRaisesARunningCompileUntilItsRecordIsIn)
{
    Harness h;
    h.submit(3, 31, Band::Background);
    h.waitRunning(1);
    h.submit(3, 32, Band::Interactive);
    h.submit(3, 33, Band::Utility);  // below the raise already lent: nothing more
    {
        std::lock_guard<std::mutex> l(h.lock);
        ASSERT_EQ(h.raises.size(), 1u);
        EXPECT_EQ(h.raises[0], Band::Interactive);
    }
    h.release(3);
    h.pool->drain();
    EXPECT_EQ(h.lowered, 1u);
    EXPECT_EQ(h.completed[32], 3);
}

TEST(ShaderCompilerPool, ASplitWaiterIsAnsweredAfterTheSplitAndARecordWaiterAfterTheRecord)
{
    Harness h;
    h.submit(4, 41, Band::Initiated);
    h.submit(4, 42, Band::Initiated, true);
    h.release(4);
    h.pool->drain();
    EXPECT_EQ(h.completed[41], 4);
    EXPECT_EQ(h.completed[42], 104);
    EXPECT_EQ(h.runs[4], 1u);
}

TEST(ShaderCompilerPool, AtMostTwoCompilesAboveBackgroundAndOneBackgroundStartAtOnce)
{
    Harness h;
    for (uint8_t n = 10; n < 14; ++n)
        h.submit(n, n, Band::Interactive);
    h.submit(20, 20, Band::Background);
    h.submit(21, 21, Band::Background);
    h.waitRunning(3);
    {
        std::lock_guard<std::mutex> l(h.lock);
        EXPECT_EQ(h.foreground, 2u);
        EXPECT_EQ(h.running, 3u);
    }
    for (uint8_t n : {10, 11, 12, 13, 20, 21})
        h.release(n);
    h.pool->drain();
    EXPECT_EQ(h.mostForeground, 2u);
    EXPECT_LE(h.mostRunning, Pool::kWorkers);
    EXPECT_EQ(h.completed.size(), 6u);
}

TEST(ShaderCompilerPool, AClientWithNothingRunningGoesBeforeOneThatHasACompileRunning)
{
    Harness h;
    h.submit(1, 1, Band::Interactive, false, 100);
    h.submit(2, 2, Band::Interactive, false, 101);
    h.waitRunning(2);
    h.submit(10, 10, Band::Interactive, false, 7);
    h.submit(11, 11, Band::Interactive, false, 7);
    h.submit(12, 12, Band::Interactive, false, 7);
    h.submit(20, 20, Band::Interactive, false, 8);
    h.release(1);
    h.waitStarted(3);
    h.release(2);
    h.waitStarted(4);
    {
        std::lock_guard<std::mutex> l(h.lock);
        ASSERT_GE(h.started.size(), 4u);
        EXPECT_EQ(h.started[2], 10);
        EXPECT_EQ(h.started[3], 20);
    }
    for (uint8_t n : {10, 11, 12, 20})
        h.release(n);
    h.pool->drain();
}

// The compiler gives its freed heap back when the pool goes idle: once the last queued or running job's waiters are
// answered, never while another job is still in the pool
TEST(ShaderCompilerPool, ThePoolIsIdleOnceItsLastJobsWaitersAreAnswered)
{
    Harness h;
    h.submit(1, 11, Band::Interactive);
    h.submit(2, 12, Band::Interactive);
    h.waitRunning(2);
    h.release(1);
    {
        std::unique_lock<std::mutex> l(h.lock);
        h.changed.wait(l, [&] {
            return h.completed.count(11) != 0;
        });
        EXPECT_TRUE(h.idles.empty());
    }
    h.release(2);
    h.pool->drain();
    h.waitIdles(1);
    h.submit(3, 13, Band::Utility);
    h.release(3);
    h.pool->drain();
    h.waitIdles(2);
    std::lock_guard<std::mutex> l(h.lock);
    ASSERT_EQ(h.idles.size(), 2u);
    EXPECT_EQ(h.idles[0], 2u);
    EXPECT_EQ(h.idles[1], 3u);
}

TEST(ShaderCompilerPool, AWaitersAnswerLeavesItsWorkerAtTheWaitersClass)
{
    Harness h;
    h.submit(5, 51, Band::Interactive);
    h.release(5);
    h.pool->drain();
    std::lock_guard<std::mutex> l(h.lock);
    ASSERT_EQ(h.completed.count(51), 1u);
    EXPECT_EQ(h.answeredAt[51], Band::Interactive) << "answered from a worker below its waiter's class";
}

// A raise lent by a more urgent waiter lasts until that waiter is answered
TEST(ShaderCompilerPool, ARaiseIsLoweredOnlyOnceItsWaiterIsAnswered)
{
    Harness h;
    h.submit(6, 61, Band::Background);
    h.waitRunning(1);
    h.submit(6, 62, Band::Interactive);
    h.release(6);
    h.pool->drain();
    std::lock_guard<std::mutex> l(h.lock);
    ASSERT_EQ(h.raises.size(), 1u);
    EXPECT_EQ(h.lowered, 1u);
    EXPECT_EQ(h.answeredAt[62], Band::Interactive) << "the raise was lowered before its waiter was answered";
}

// A worker that has never run a job, and one parked after a job, pick the next job at the interactive class and only
// then take its band; the idle housekeeping runs at the background class
TEST(ShaderCompilerPool, AParkedWorkerPicksItsNextJobAtTheInteractiveClass)
{
    Harness h(1);
    h.submit(7, 71, Band::Utility);
    h.release(7);
    h.pool->drain();
    h.waitIdles(1);
    h.submit(8, 81, Band::Background);
    h.release(8);
    h.pool->drain();
    h.waitIdles(2);
    std::lock_guard<std::mutex> l(h.lock);
    EXPECT_EQ(h.pickedAt[7], Band::Interactive) << "a new worker picked its first job below the interactive class";
    EXPECT_EQ(h.pickedAt[8], Band::Interactive) << "a parked worker picked its next job below the interactive class";
    EXPECT_EQ(h.startBand[7], Band::Utility);
    EXPECT_EQ(h.startBand[8], Band::Background);
    ASSERT_EQ(h.idleBands.size(), 2u);
    EXPECT_EQ(h.idleBands[0], Band::Background);
    EXPECT_EQ(h.idleBands[1], Band::Background);
}

// Boot 22's queue waits: an urgent request arriving while every worker sits parked after its jobs is picked up at the
// interactive class, never by a worker still at the background class its last job left it in
TEST(ShaderCompilerPool, AForegroundRequestToAllParkedWorkersIsPickedAtTheInteractiveClass)
{
    Harness h;
    h.submit(1, 11, Band::Interactive);
    h.submit(2, 12, Band::Interactive);
    h.submit(3, 13, Band::Background);
    h.waitRunning(3);
    for (uint8_t n : {1, 2, 3})
        h.release(n);
    h.pool->drain();
    h.waitIdles(1);
    h.submit(4, 14, Band::Interactive);
    h.release(4);
    h.pool->drain();
    std::lock_guard<std::mutex> l(h.lock);
    EXPECT_EQ(h.pickedAt[4], Band::Interactive) << "a parked worker picked an urgent job below the interactive class";
    EXPECT_EQ(h.startBand[4], Band::Interactive);
    EXPECT_EQ(h.answeredAt[14], Band::Interactive);
}

}  // namespace
