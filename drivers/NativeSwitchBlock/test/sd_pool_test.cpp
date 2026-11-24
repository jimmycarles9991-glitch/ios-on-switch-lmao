// Host test of drivers/NativeSwitchBlock/SwitchSdPool.h: slab freelist, single-lock submit, unlocked heap overflow,
// shutdown-race cleanup, chunked completion return before callbacks, re-entrant submission, and leak-free teardown.
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <random>
#include <unordered_set>
#include <vector>

#include "../SwitchQueue.h"
#include "../SwitchSdPool.h"
#include "bazel/testing/require.h"

namespace {

struct TestRequest {
    TestRequest *next;
    uint64_t lba;
    uint32_t sectors;
    uint64_t queuedNs;
    int id;
    bool write;
    bool chained;
    bool pooled;
};

struct Meta {
    int id;
    uint64_t lba;
    uint32_t sectors;
    bool pooled;
};

struct Harness {
    std::vector<TestRequest> slab;
    SwitchSdPool<TestRequest> pool;
    TestRequest *head = nullptr;
    TestRequest *tail = nullptr;
    uint64_t clockNs = 1000;
    bool locked = false;
    bool shutdownDone = false;
    bool failAlloc = false;
    unsigned lockCount = 0;
    unsigned exhaustedCount = 0;
    unsigned allocCount = 0;
    unsigned allocSucceeded = 0;
    unsigned freeCount = 0;
    std::unordered_set<TestRequest *> liveHeap;
    std::vector<int> completedIds;
    std::function<void()> onAllocHook;
    std::function<void(const Meta &)> onFinishHook;

    explicit Harness(uint32_t capacity) : slab(capacity)
    {
        pool.init(capacity ? slab.data() : nullptr, capacity);
    }

    ~Harness()
    {
        for (TestRequest *p : liveHeap)
            delete p;
    }

    void lock()
    {
        REQUIRE(!locked);
        locked = true;
        ++lockCount;
    }

    void unlock()
    {
        REQUIRE(locked);
        locked = false;
    }

    bool isShutdown() const
    {
        REQUIRE(locked);
        return shutdownDone;
    }

    uint64_t nowNs()
    {
        clockNs += 100;
        return clockNs;
    }

    void onExhausted(uint32_t capacity)
    {
        REQUIRE(!locked);
        REQUIRE(capacity == pool.size());
        ++exhaustedCount;
    }

    TestRequest *allocHeap()
    {
        REQUIRE(!locked);
        ++allocCount;
        if (onAllocHook)
            onAllocHook();
        if (failAlloc)
            return nullptr;
        ++allocSucceeded;
        auto *node = new TestRequest{};
        liveHeap.insert(node);
        return node;
    }

    void freeHeap(TestRequest *node)
    {
        REQUIRE(!locked);
        REQUIRE(node != nullptr);
        REQUIRE(!pool.owns(node));
        REQUIRE(liveHeap.erase(node) == 1u);
        ++freeCount;
        delete node;
    }

    void enqueueLocked(TestRequest *node)
    {
        REQUIRE(locked);
        REQUIRE(node->next == nullptr);
        REQUIRE(!node->chained);
        if (node->pooled) {
            REQUIRE(pool.owns(node));
        } else {
            REQUIRE(!pool.owns(node));
            REQUIRE(liveHeap.count(node) == 1u);
        }
        if (tail) {
            tail->next = node;
        } else {
            head = node;
        }
        tail = node;
    }

    Meta capture(const TestRequest &r) const
    {
        REQUIRE(!locked);
        if (r.pooled) {
            REQUIRE(pool.owns(&r));
        } else {
            REQUIRE(liveHeap.count(const_cast<TestRequest *>(&r)) == 1u);
        }
        return {r.id, r.lba, r.sectors, r.pooled};
    }

    void finish(const Meta &m)
    {
        REQUIRE(!locked);
        completedIds.push_back(m.id);
        if (onFinishHook)
            onFinishHook(m);
    }

    SwitchSdPoolSubmitResult submit(int id, uint64_t lba = 0, uint32_t sectors = 8, bool write = false)
    {
        const TestRequest spec = {nullptr, lba, sectors, 0, id, write, true, false};
        return switchSdPoolSubmit(pool, spec, *this);
    }

    template <size_t Chunk = 16> void completeList(TestRequest *list)
    {
        switchSdPoolComplete<Chunk, TestRequest, Meta>(pool, list, *this);
    }

    TestRequest *drainQueue()
    {
        TestRequest *list = head;
        head = tail = nullptr;
        return list;
    }
};

}  // namespace

TEST(SdPool, InitOwnsPopPushAndDetach)
{
    std::vector<TestRequest> slab(4);
    SwitchSdPool<TestRequest> pool;
    pool.init(nullptr, 0);
    REQUIRE(pool.size() == 0 && pool.active() == 0 && pool.available() == 0 && pool.empty());
    REQUIRE(pool.pop() == nullptr && pool.detachIfIdle() == nullptr && !pool.owns(&slab[0]));

    pool.init(slab.data(), 4);
    REQUIRE(pool.size() == 4 && pool.active() == 0 && pool.available() == 4 && !pool.empty());
    for (uint32_t i = 0; i < 4; ++i)
        REQUIRE(pool.owns(&slab[i]));
    TestRequest external{};
    REQUIRE(!pool.owns(&external));
    REQUIRE(!pool.owns(nullptr));
    const auto *unaligned = reinterpret_cast<const TestRequest *>(reinterpret_cast<uintptr_t>(slab.data()) + 1);
    REQUIRE(!pool.owns(unaligned));

    std::vector<TestRequest *> popped;
    for (uint32_t i = 0; i < 4; ++i) {
        TestRequest *n = pool.pop();
        REQUIRE(n != nullptr && pool.owns(n) && n->pooled && n->next == nullptr);
        popped.push_back(n);
    }
    REQUIRE(pool.empty() && pool.active() == 4 && pool.available() == 0);
    REQUIRE(pool.pop() == nullptr);
    REQUIRE(pool.detachIfIdle() == nullptr);

    pool.push(popped[0]);
    REQUIRE(pool.active() == 3 && pool.available() == 1 && !pool.empty());
    REQUIRE(pool.detachIfIdle() == nullptr);

    popped[1]->next = popped[2];
    popped[2]->next = popped[3];
    popped[3]->next = nullptr;
    pool.pushBatch(popped[1], popped[3], 3);
    REQUIRE(pool.active() == 0 && pool.available() == 4);

    // Every slot pops back cleanly after reuse.
    std::unordered_set<TestRequest *> seen;
    for (uint32_t i = 0; i < 4; ++i) {
        TestRequest *n = pool.pop();
        REQUIRE(n != nullptr && seen.insert(n).second);
    }
    REQUIRE(pool.pop() == nullptr);
    for (TestRequest *n : seen)
        pool.push(n);
    REQUIRE(pool.detachIfIdle() == slab.data());
    REQUIRE(pool.size() == 0 && pool.empty() && pool.pop() == nullptr);
}

TEST(SdPool, FastPathSingleLockZeroAlloc)
{
    Harness h(4);
    REQUIRE(h.submit(1, 100, 8, false) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(!h.locked);
    REQUIRE(h.lockCount == 1u);
    REQUIRE(h.allocCount == 0u);
    REQUIRE(h.exhaustedCount == 0u);
    REQUIRE(h.pool.active() == 1u && h.pool.available() == 3u);
    REQUIRE(h.head != nullptr && h.head == h.tail && h.head->pooled && !h.head->chained && h.head->id == 1);
    REQUIRE(h.head->queuedNs > 0);

    h.completeList(h.drainQueue());
    REQUIRE(h.pool.active() == 0u && h.freeCount == 0u && h.liveHeap.empty());
    REQUIRE(h.completedIds == std::vector<int>{1});
}

TEST(SdPool, ShutdownFastPathRejectsWithoutPop)
{
    Harness h(4);
    h.shutdownDone = true;
    REQUIRE(h.submit(1) == SwitchSdPoolSubmitResult::kNotReady);
    REQUIRE(!h.locked);
    REQUIRE(h.lockCount == 1u);
    REQUIRE(h.allocCount == 0u);
    REQUIRE(h.pool.active() == 0u && h.pool.available() == 4u);
    REQUIRE(h.head == nullptr && h.tail == nullptr);
}

TEST(SdPool, OverflowDropsLockBeforeAllocAndFreesOutsideLock)
{
    Harness h(2);
    REQUIRE(h.submit(0) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.submit(1) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.pool.empty() && h.allocCount == 0u && h.lockCount == 2u);

    // Third submit drops lock before allocHeap() and re-acquires to enqueue.
    REQUIRE(h.submit(2) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(!h.locked);
    REQUIRE(h.exhaustedCount == 1u);
    REQUIRE(h.allocCount == 1u);
    REQUIRE(h.lockCount == 4u);
    REQUIRE(h.liveHeap.size() == 1u);
    REQUIRE(h.tail != nullptr && !h.tail->pooled && h.tail->id == 2);

    // Heap allocation failure returns kNoMemory cleanly without holding the lock.
    h.failAlloc = true;
    REQUIRE(h.submit(3) == SwitchSdPoolSubmitResult::kNoMemory);
    REQUIRE(!h.locked);
    REQUIRE(h.exhaustedCount == 2u);
    REQUIRE(h.allocCount == 2u);
    REQUIRE(h.liveHeap.size() == 1u);

    h.completeList(h.drainQueue());
    REQUIRE(h.pool.active() == 0u && h.freeCount == 1u && h.liveHeap.empty());
    REQUIRE((h.completedIds == std::vector<int>{0, 1, 2}));
}

TEST(SdPool, OverflowShutdownRaceFreesOutsideLock)
{
    Harness h(1);
    REQUIRE(h.submit(0) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.pool.empty());

    // Simulate shutdown() setting shutdownDone while queueLock is dropped for IOMalloc.
    h.onAllocHook = [&]() {
        h.shutdownDone = true;
    };
    REQUIRE(h.submit(1) == SwitchSdPoolSubmitResult::kNotReady);
    REQUIRE(!h.locked);
    REQUIRE(h.allocCount == 1u);
    REQUIRE(h.freeCount == 1u);
    REQUIRE(h.liveHeap.empty());
    REQUIRE(h.head == h.tail && h.head->id == 0);

    h.completeList(h.drainQueue());
    REQUIRE(h.pool.active() == 0u && h.liveHeap.empty());
    REQUIRE(h.pool.detachIfIdle() == h.slab.data());
}

TEST(SdPool, ChunkedCompletionReturnsPooledBeforeCallbacksAndFreesHeapOutsideLock)
{
    Harness h(10);
    for (int i = 0; i < 25; ++i)
        REQUIRE(h.submit(i, uint64_t(i) * 8, 8, (i & 1) != 0) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.pool.active() == 10u && h.liveHeap.size() == 15u);

    const unsigned locksBefore = h.lockCount;
    // Chunk=4: chunks 0..2 contain pooled nodes (3 lock holds), chunks 3..6 are heap-only (0 lock holds).
    h.onFinishHook = [&](const Meta &m) {
        REQUIRE(!h.locked);
        if (m.id == 0)
            REQUIRE(h.pool.available() == 4u);
    };
    h.completeList<4>(h.drainQueue());
    REQUIRE(h.lockCount - locksBefore == 3u);
    REQUIRE(h.pool.active() == 0u && h.pool.available() == 10u);
    REQUIRE(h.liveHeap.empty() && h.freeCount == 15u);
    REQUIRE(h.completedIds.size() == 25u);
    for (int i = 0; i < 25; ++i)
        REQUIRE(h.completedIds[size_t(i)] == i);
}

TEST(SdPool, ReentrantSubmitInCallbackNeverDeadlocksAndReusesPool)
{
    Harness h(2);
    REQUIRE(h.submit(10) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.submit(11) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.pool.empty());

    // First completion callback immediately submits a new request; pooled nodes were already returned before finish().
    bool submitted = false;
    h.onFinishHook = [&](const Meta &m) {
        if (!submitted && m.id == 10) {
            submitted = true;
            REQUIRE(h.submit(99) == SwitchSdPoolSubmitResult::kQueued);
        }
    };
    h.completeList<4>(h.drainQueue());
    REQUIRE(submitted);
    REQUIRE(h.allocCount == 0u);
    REQUIRE(h.pool.active() == 1u && h.head != nullptr && h.head->id == 99 && h.head->pooled);

    h.onFinishHook = nullptr;
    h.completeList<4>(h.drainQueue());
    REQUIRE(h.pool.active() == 0u && h.liveHeap.empty());
    REQUIRE(h.pool.detachIfIdle() == h.slab.data());
}

TEST(SdPool, ShutdownDrainMixedPooledAndHeapWithReentrantAttempt)
{
    Harness h(8);
    for (int i = 0; i < 20; ++i)
        REQUIRE(h.submit(i) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(h.pool.active() == 8u && h.liveHeap.size() == 12u);

    h.shutdownDone = true;
    TestRequest *abandoned = h.drainQueue();
    h.onFinishHook = [&](const Meta &) {
        REQUIRE(h.submit(1000) == SwitchSdPoolSubmitResult::kNotReady);
    };
    h.completeList<4>(abandoned);
    REQUIRE(h.completedIds.size() == 20u);
    REQUIRE(h.pool.active() == 0u && h.pool.available() == 8u);
    REQUIRE(h.liveHeap.empty() && h.freeCount == 12u);
    REQUIRE(h.head == nullptr && h.tail == nullptr);
    REQUIRE(h.pool.detachIfIdle() == h.slab.data());
}

TEST(SdPool, RandomizedTrafficWithMergesRetriesAndShutdownDrain)
{
    Harness h(32);
    std::mt19937_64 rng(0x5d9001ULL);
    int nextId = 0;
    size_t queuedCount = 0;
    size_t expectedCompletions = 0;
    h.onFinishHook = [&](const Meta &) {
        if (!h.shutdownDone && (rng() % 5 == 0)) {
            if (h.submit(nextId++, (rng() % 64) * 8, 8, (rng() % 2) != 0) == SwitchSdPoolSubmitResult::kQueued)
                ++queuedCount;
        }
    };

    for (unsigned step = 0; step < 5000; ++step) {
        const unsigned burst = unsigned(rng() % 6);
        for (unsigned i = 0; i < burst; ++i) {
            h.failAlloc = (rng() % 11 == 0);
            const auto res = h.submit(nextId++, (rng() % 64) * 8, 8, (rng() % 2) != 0);
            if (res == SwitchSdPoolSubmitResult::kQueued)
                ++queuedCount;
        }
        h.failAlloc = false;
        if (h.head && (rng() % 2 == 0)) {
            uint64_t start = 0;
            uint32_t sectors = 0;
            uint32_t count = 0;
            if (h.head->write) {
                count = switchGatherWrites(h.head, h.head, 2048, 32, 64, &start, &sectors);
                REQUIRE(switchWriteChainTiles(h.head, start, sectors, count));
            } else {
                count = switchGatherReads(h.head, h.head, 2048, 32, 64, &start, &sectors);
                REQUIRE(switchChainTiles(h.head, start, sectors, count));
            }
            TestRequest *chain = switchUnlinkChain(h.head, h.tail, count);
            queuedCount -= count;
            expectedCompletions += count;
            h.completeList<8>(chain);
        }
        REQUIRE(h.pool.active() + h.pool.available() == h.pool.size());
        REQUIRE(h.pool.active() + h.liveHeap.size() == queuedCount);
    }

    h.shutdownDone = true;
    expectedCompletions += queuedCount;
    h.completeList<8>(h.drainQueue());
    REQUIRE(h.completedIds.size() == expectedCompletions);
    REQUIRE(h.pool.active() == 0u && h.pool.available() == h.pool.size());
    REQUIRE(h.liveHeap.empty() && h.freeCount == h.allocSucceeded);
    REQUIRE(h.pool.detachIfIdle() == h.slab.data());
}

TEST(SdPool, ZeroCapacityDegradesToHeapWithoutExhaustedCallback)
{
    Harness h(0);
    REQUIRE(h.submit(1, 64, 8, false) == SwitchSdPoolSubmitResult::kQueued);
    REQUIRE(!h.locked);
    REQUIRE(h.allocCount == 1u);
    REQUIRE(h.exhaustedCount == 0u);
    REQUIRE(h.head != nullptr && !h.head->pooled && h.head->id == 1);

    h.completeList(h.drainQueue());
    REQUIRE(h.freeCount == 1u && h.liveHeap.empty());
    REQUIRE(h.completedIds == std::vector<int>{1});
}
