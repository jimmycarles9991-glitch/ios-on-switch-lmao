// SwitchCryptexCache.h, the cryptex direct read map's block cache, against a model: every request the
// cache takes completes exactly once, with the image's bytes unless a read it waited for failed; a block is never
// read twice at once nor read while held; a window is never renamed or retired while a request holds it or a read fills
// it; and the cache's own invariants hold after every step. What SwitchCryptex.cpp does with it is guarded by
// cryptex_source_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "../SwitchCryptexCache.h"

namespace {

uint64_t content(uint8_t image, uint64_t block)
{
    uint64_t z = (uint64_t(image) << 48) + block + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

constexpr uint64_t kPoison = 0xdeadbeefdeadbeefull;

// The cache with its windows, each window's "bytes" one uint64_t per block (the block's content id).
struct Rig {
    std::vector<SwitchWindow> windows;
    std::vector<std::vector<uint64_t>> slots;
    std::vector<SwitchWindow *> buckets;
    SwitchCache cache;
    uint64_t imageBlocks[2] = {};

    Rig(uint32_t count, uint32_t bucketCount, uint64_t image0, uint64_t image1)
        : windows(count), slots(count, std::vector<uint64_t>(kSwitchWindowBlocks, kPoison)), buckets(bucketCount)
    {
        for (uint32_t i = 0; i < count; ++i)
            windows[i].bytes = slots[i].data();
        cache.init(windows.data(), count, buckets.data(), bucketCount);
        imageBlocks[0] = image0;
        imageBlocks[1] = image1;
    }

    uint64_t *bytes(SwitchWindow *w)
    {
        return static_cast<uint64_t *>(w->bytes);
    }

    // The card's read: the image's blocks into the window, or poison for a failed one.
    SwitchCacheRequest *finish(const SwitchCacheRead &r, bool ok)
    {
        for (uint32_t b = r.first; b < r.first + r.count; ++b)
            bytes(r.window)[b] = ok ? content(r.window->image, r.window->tag + b) : kPoison;
        return cache.finished(r.window, r.first, r.count, ok);
    }

    // Copies a ready request's blocks out of its windows; false if any differs from the image.
    bool copyOut(const SwitchCacheRequest &r, uint8_t image, uint64_t first, uint32_t blocks)
    {
        const uint64_t firstTag = first - first % kSwitchWindowBlocks;
        for (uint64_t b = first; b < first + blocks; ++b) {
            const uint32_t i = uint32_t((b - firstTag) / kSwitchWindowBlocks);
            if (i >= r.windows)
                return false;
            SwitchWindow *w = r.window[i];
            const uint32_t at = uint32_t(b - w->tag);
            if (w->image != image || w->tag != firstTag + uint64_t(i) * kSwitchWindowBlocks ||
                at >= kSwitchWindowBlocks)
                return false;
            if (!(w->valid & (1u << at)) || bytes(w)[at] != content(image, b))
                return false;
        }
        return true;
    }
};

std::vector<SwitchCacheRequest *> list(SwitchCacheRequest *ready)
{
    std::vector<SwitchCacheRequest *> out;
    for (; ready; ready = ready->nextReady)
        out.push_back(ready);
    return out;
}

TEST(CryptexCache, AColdMissReadsItsWholeWindowOnceAndNeighboursWaitForIt)
{
    Rig rig(4, 8, 100, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest a{}, b{}, c{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 2, 1, 100, SwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(reads[0].first, 0u);
    EXPECT_EQ(reads[0].count, kSwitchWindowBlocks);
    EXPECT_EQ(a.waiting, 1u);
    SwitchCacheRead none[32];
    uint32_t m = 0;
    ASSERT_TRUE(rig.cache.plan(&b, 0, 3, 2, 100, SwitchCache::kReadAround, none, 32, &m));
    EXPECT_EQ(m, 0u) << "a block being read is read again";
    EXPECT_EQ(b.waiting, 1u);
    EXPECT_EQ(rig.cache.check(), nullptr);
    std::vector<SwitchCacheRequest *> ready = list(rig.finish(reads[0], true));
    EXPECT_EQ(ready.size(), 2u);
    EXPECT_TRUE(rig.copyOut(a, 0, 2, 1));
    EXPECT_TRUE(rig.copyOut(b, 0, 3, 2));
    rig.cache.release(&a, true);
    rig.cache.release(&b, true);
    ASSERT_TRUE(rig.cache.plan(&c, 0, kSwitchWindowBlocks - 1, 1, 100, SwitchCache::kReadAround, none, 32, &m));
    EXPECT_EQ(m, 0u);
    EXPECT_EQ(c.waiting, 0u) << "a held block is not ready";
    EXPECT_TRUE(rig.copyOut(c, 0, kSwitchWindowBlocks - 1, 1));
    rig.cache.release(&c, true);
    EXPECT_EQ(rig.cache.stats().ready, 1u);
    EXPECT_EQ(rig.cache.stats().reads, 1u);
    EXPECT_EQ(rig.cache.stats().served, 4u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// A request waiting on a read another request issued is joined (SwitchCryptex.cpp then raises that read to its
// priority); one waiting only on reads its own plan issued is not.
TEST(CryptexCache, ARequestWaitingOnAnotherRequestsReadIsJoined)
{
    Rig rig(4, 8, 100, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest a{}, b{}, c{}, d{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 2, 1, 100, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_FALSE(a.joined);
    SwitchCacheRead none[32];
    uint32_t m = 0;
    ASSERT_TRUE(rig.cache.plan(&b, 0, 3, 2, 100, SwitchCache::kReadAround, none, 32, &m));
    EXPECT_TRUE(b.joined);
    ASSERT_TRUE(rig.cache.plan(&c, 0, 0, kSwitchWindowBlocks, 100, SwitchCache::kCoveredOnly, none, 32, &m));
    EXPECT_TRUE(c.joined);
    for (SwitchCacheRequest *r : list(rig.finish(reads[0], true)))
        rig.cache.release(r, true);
    ASSERT_TRUE(rig.cache.plan(&d, 0, 4, 1, 100, SwitchCache::kReadAround, none, 32, &m));
    EXPECT_FALSE(d.joined) << "every block it needs is held";
    rig.cache.release(&d, true);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// A window is urgent while a tier-0 request waits on its reads or XNU raised them (SwitchCryptex.cpp issueRead raises a
// fill planned before the mark once it is queued); the mark goes when nothing is reading the window, and on reuse.
TEST(CryptexCache, AWindowIsUrgentWhileATierZeroRequestWaitsOnItsReads)
{
    Rig rig(1, 8, 100, 10);
    SwitchCacheRead reads[32], none[32];
    uint32_t n = 0, m = 0;
    SwitchCacheRequest a{}, b{}, c{}, d{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 2, 1, 100, SwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    SwitchWindow *w = reads[0].window;
    EXPECT_FALSE(w->urgent) << "its own read, at its own priority";
    ASSERT_TRUE(rig.cache.plan(&b, 0, 3, 1, 100, SwitchCache::kReadAround, none, 32, &m));
    EXPECT_FALSE(w->urgent) << "a joiner below tier 0";
    ASSERT_TRUE(rig.cache.plan(&c, 0, 4, 1, 100, SwitchCache::kReadAround, none, 32, &m, true));
    EXPECT_TRUE(c.joined);
    EXPECT_TRUE(w->urgent);
    for (SwitchCacheRequest *r : list(rig.finish(reads[0], true)))
        rig.cache.release(r, true);
    EXPECT_FALSE(w->urgent) << "nothing reading it";

    rig.cache.markUrgent(0, 0, kSwitchWindowBlocks);
    EXPECT_FALSE(w->urgent) << "a raise of blocks it holds";
    rig.cache.markUrgent(0, 0, 0);
    ASSERT_TRUE(rig.cache.plan(&d, 0, 40, 1, 100, SwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(reads[0].window, w) << "the one window is reused";
    rig.cache.markUrgent(0, 40, 1);
    EXPECT_TRUE(w->urgent) << "XNU raised a fill being read";
    for (SwitchCacheRequest *r : list(rig.finish(reads[0], true)))
        rig.cache.release(r, true);
    EXPECT_FALSE(w->urgent);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, TheLastWindowStopsAtTheImageEnd)
{
    constexpr uint64_t kImage = kSwitchWindowBlocks + 3;
    Rig rig(2, 4, kImage, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest a{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, kSwitchWindowBlocks + 1, 1, kImage, SwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(reads[0].first, 0u);
    EXPECT_EQ(reads[0].count, 3u);
    SwitchCacheRequest past{};
    uint32_t m = 0;
    EXPECT_FALSE(rig.cache.plan(&past, 0, kImage - 1, 2, kImage, SwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_FALSE(rig.cache.plan(&past, 1, 10, 1, 10, SwitchCache::kReadAround, reads + 1, 31, &m));
}

TEST(CryptexCache, ReadAheadReadsExactlyItsBlocksAndRequestsFindThem)
{
    Rig rig(4, 8, 200, 10);
    SwitchCacheRead reads[kSwitchWindowReads];
    uint32_t n = 0;
    std::vector<SwitchCacheRead> issued;
    constexpr uint64_t kStart = 2;
    constexpr uint64_t kEnd = kSwitchWindowBlocks + 3;
    for (uint64_t at = kStart; at < kEnd;) {
        const uint32_t covered = rig.cache.readAhead(0, at, kEnd - at, 200, reads, &n);
        ASSERT_GT(covered, 0u);
        issued.insert(issued.end(), reads, reads + n);
        at += covered;
    }
    ASSERT_EQ(issued.size(), 2u);
    EXPECT_EQ(issued[0].first, kStart);
    EXPECT_EQ(issued[0].count, kSwitchWindowBlocks - kStart);
    EXPECT_EQ(issued[1].first, 0u);
    EXPECT_EQ(issued[1].count, 3u);
    EXPECT_NE(issued[0].window, issued[1].window);
    // A request inside the read-ahead waits for it and reads only the rest of its window.
    SwitchCacheRequest a{};
    SwitchCacheRead around[32];
    ASSERT_TRUE(rig.cache.plan(&a, 0, 3, 2, 200, SwitchCache::kReadAround, around, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(around[0].first, 0u);
    EXPECT_EQ(around[0].count, kStart);
    EXPECT_EQ(a.waiting, 1u);
    EXPECT_TRUE(list(rig.finish(around[0], true)).empty());
    EXPECT_EQ(list(rig.finish(issued[0], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(a, 0, 3, 2));
    rig.cache.release(&a, true);
    EXPECT_TRUE(list(rig.finish(issued[1], true)).empty());
    // Read ahead again: nothing left to read.
    EXPECT_EQ(rig.cache.readAhead(0, kStart, kEnd - kStart, 200, reads, &n), kSwitchWindowBlocks - kStart);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(rig.cache.stats().aheadBlocks, kEnd - kStart);
    EXPECT_EQ(rig.cache.stats().aheadServed, 2u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, ALargeRequestIsTakenOnlyWhenItsBlocksAreAllHere)
{
    constexpr uint32_t kTwoWindows = 2 * kSwitchWindowBlocks;
    Rig rig(4, 8, 300, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest big{};
    EXPECT_FALSE(rig.cache.plan(&big, 0, 0, kTwoWindows, 300, SwitchCache::kCoveredOnly, reads, 32, &n));
    EXPECT_EQ(rig.cache.stats().refused, 1u);
    for (uint64_t at = 0; at < kTwoWindows;)
        at += rig.cache.readAhead(0, at, kTwoWindows - at, 300, reads + (at ? 1 : 0), &n);
    ASSERT_TRUE(rig.cache.plan(&big, 0, 0, kTwoWindows, 300, SwitchCache::kCoveredOnly, reads + 2, 30, &n));
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(big.waiting, 2u);
    EXPECT_TRUE(list(rig.finish(reads[0], true)).empty());
    EXPECT_EQ(list(rig.finish(reads[1], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(big, 0, 0, kTwoWindows));
    rig.cache.release(&big, true);
    // Not all here: refused, nothing held.
    SwitchCacheRequest wider{};
    EXPECT_FALSE(rig.cache.plan(&wider, 0, 0, kTwoWindows + 1, 300, SwitchCache::kCoveredOnly, reads, 32, &n));
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, AFailedReadFailsItsWaitersAndIsReadAgain)
{
    Rig rig(2, 4, 64, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest a{}, b{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 3, 1, 64, SwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    const std::vector<SwitchCacheRequest *> ready = list(rig.finish(reads[0], false));
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_TRUE(a.failed);
    rig.cache.release(&a, false);
    ASSERT_TRUE(rig.cache.plan(&b, 0, 3, 1, 64, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 1u);
    EXPECT_FALSE(b.failed);
    EXPECT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(b, 0, 3, 1));
    rig.cache.release(&b, true);
    EXPECT_EQ(rig.cache.stats().failed, 1u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, WindowsInUseAreNeverReusedOrRetired)
{
    Rig rig(1, 2, 200, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0, m = 0;
    SwitchCacheRequest a{}, b{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 3, 1, 200, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_FALSE(rig.cache.plan(&b, 0, kSwitchWindowBlocks + 2, 1, 200, SwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_EQ(rig.cache.retire(0), nullptr);
    EXPECT_EQ(rig.cache.readAhead(0, 2 * kSwitchWindowBlocks, 4, 200, reads + 1, &m), 0u);
    ASSERT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    EXPECT_FALSE(rig.cache.plan(&b, 0, kSwitchWindowBlocks + 2, 1, 200, SwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_EQ(rig.cache.retire(0), nullptr);
    rig.cache.release(&a, true);
    ASSERT_TRUE(rig.cache.plan(&b, 0, kSwitchWindowBlocks + 2, 1, 200, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(rig.cache.stats().evicted, 1u) << "unserved blocks of the first window were evicted";
    EXPECT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    rig.cache.release(&b, true);
    SwitchWindow *w = rig.cache.retire(0);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(w->retired);
    EXPECT_EQ(rig.cache.live(), 0u);
    EXPECT_FALSE(rig.cache.plan(&a, 0, 3, 1, 200, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// A window whose blocks have all been served (they are in the files' page cache) is reused before one still holding
// blocks nobody asked for yet.
TEST(CryptexCache, FullyServedWindowsAreReusedFirst)
{
    constexpr uint32_t kHalf = kSwitchWindowBlocks / 2;
    Rig rig(2, 4, 1000, 10);
    SwitchCacheRead reads[32];
    uint32_t n = 0;
    SwitchCacheRequest a{}, b{}, c{};
    // Window 0: read ahead (full window) and served whole; window 1: read around one block.
    ASSERT_EQ(rig.cache.readAhead(0, 0, kSwitchWindowBlocks, 1000, reads, &n), kSwitchWindowBlocks);
    ASSERT_EQ(n, 1u);
    rig.finish(reads[0], true);
    ASSERT_TRUE(rig.cache.plan(&b, 0, kSwitchWindowBlocks, 1, 1000, SwitchCache::kReadAround, reads, 32, &n));
    rig.finish(reads[0], true);
    rig.cache.release(&b, true);
    ASSERT_TRUE(rig.cache.plan(&a, 0, 0, kHalf, 1000, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 0u);
    rig.cache.release(&a, true);
    ASSERT_TRUE(
        rig.cache.plan(&a, 0, kHalf, kSwitchWindowBlocks - kHalf, 1000, SwitchCache::kReadAround, reads, 32, &n));
    rig.cache.release(&a, true);
    // A new window takes window 0 (all served), though window 1 was used longer ago.
    ASSERT_TRUE(rig.cache.plan(&c, 0, 500, 1, 1000, SwitchCache::kReadAround, reads, 32, &n));
    rig.finish(reads[0], true);
    rig.cache.release(&c, true);
    EXPECT_EQ(rig.cache.stats().evicted, 0u);
    ASSERT_TRUE(rig.cache.plan(&b, 0, kSwitchWindowBlocks + 1, 1, 1000, SwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 0u) << "window 1 was reused instead";
    rig.cache.release(&b, true);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// Random requests, read-aheads, read completions (in any order, some failing), copies and retirements against the
// model.
TEST(CryptexCache, RandomTrafficAgainstAModel)
{
    std::mt19937_64 rng(19);
    unsigned long requests = 0, refused = 0, failedRequests = 0, readsDone = 0;
    for (int round = 0; round < 400; ++round) {
        const uint32_t windowCount = 1 + uint32_t(rng() % 24);
        const uint32_t bucketCount = 1u << (rng() % 6);
        const uint64_t size0 = 1 + rng() % 2000, size1 = 1 + rng() % 300;
        Rig rig(windowCount, bucketCount, size0, size1);
        const bool failures = rng() % 3 == 0;

        struct Request {
            SwitchCacheRequest cr{};
            uint8_t image = 0;
            uint64_t first = 0;
            uint32_t blocks = 0;
            bool ready = false, done = false, mayFail = false;
        };

        std::deque<std::unique_ptr<Request>> live;
        std::vector<Request *> ready;

        struct Read {
            SwitchCacheRead r;
            uint8_t image;
            uint64_t tag;
        };

        std::vector<Read> inflight;
        std::map<std::pair<uint8_t, uint64_t>, int> reading;  // (image, block) -> reads in flight

        auto issue = [&](const SwitchCacheRead *reads, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                const SwitchCacheRead &r = reads[i];
                ASSERT_TRUE(r.count && r.first + r.count <= kSwitchWindowBlocks);
                for (uint32_t b = r.first; b < r.first + r.count; ++b) {
                    ASSERT_LT(r.window->tag + b, rig.imageBlocks[r.window->image]) << "read past the image";
                    const int before = reading[{r.window->image, r.window->tag + b}]++;
                    ASSERT_EQ(before, 0) << "a block read twice at once";
                }
                inflight.push_back({r, r.window->image, r.window->tag});
            }
        };
        auto becameReady = [&](SwitchCacheRequest *list) {
            for (; list; list = list->nextReady) {
                Request *q = static_cast<Request *>(list->owner);
                ASSERT_FALSE(q->ready) << "a request made ready twice";
                q->ready = true;
                ready.push_back(q);
            }
        };

        for (int step = 0; step < 600; ++step) {
            const unsigned op = unsigned(rng() % 100);
            if (op < 35) {  // a request
                auto q = std::make_unique<Request>();
                q->cr.owner = q.get();
                q->image = uint8_t(rng() % 4 == 0);
                const uint64_t size = rig.imageBlocks[q->image];
                const bool large = rng() % 6 == 0;
                constexpr uint32_t kMaxSmall = kSwitchWindowBlocks / 2;
                q->first = rng() % size;
                const uint64_t room = size - q->first;
                q->blocks =
                    uint32_t(std::min<uint64_t>(room, large ? kMaxSmall + 1 + rng() % 240 : 1 + rng() % kMaxSmall));
                SwitchCacheRead reads[kSwitchAroundWindows * kSwitchWindowReads];
                uint32_t n = 0;
                const auto mode = q->blocks > kMaxSmall ? SwitchCache::kCoveredOnly : SwitchCache::kReadAround;
                if (!rig.cache.plan(&q->cr, q->image, q->first, q->blocks, size, mode, reads,
                                    kSwitchAroundWindows * kSwitchWindowReads, &n)) {
                    ASSERT_EQ(n, 0u);
                    ASSERT_EQ(q->cr.windows, 0u) << "a refused request holds windows";
                    ++refused;
                    continue;
                }
                if (mode == SwitchCache::kCoveredOnly)
                    ASSERT_EQ(n, 0u) << "a large request read something";
                issue(reads, n);
                ++requests;
                if (!q->cr.waiting) {
                    q->ready = true;
                    ready.push_back(q.get());
                }
                live.push_back(std::move(q));
            } else if (op < 45) {  // read ahead over a range
                const uint8_t image = uint8_t(rng() % 4 == 0);
                const uint64_t size = rig.imageBlocks[image];
                const uint64_t first = rng() % size, end = std::min<uint64_t>(size, first + 1 + rng() % 128);
                for (uint64_t at = first; at < end;) {
                    SwitchCacheRead reads[kSwitchWindowReads];
                    uint32_t n = 0;
                    const uint32_t covered = rig.cache.readAhead(image, at, end - at, size, reads, &n);
                    if (!covered)
                        break;
                    issue(reads, n);
                    at += covered;
                }
            } else if (op < 80 && !inflight.empty()) {  // a read finishes
                const size_t i = rng() % inflight.size();
                const Read r = inflight[i];
                inflight.erase(inflight.begin() + long(i));
                ASSERT_EQ(r.r.window->tag, r.tag) << "a window renamed while a read fills it";
                ASSERT_FALSE(r.r.window->retired);
                for (uint32_t b = r.r.first; b < r.r.first + r.r.count; ++b)
                    --reading[{r.image, r.tag + b}];
                const bool ok = !failures || rng() % 8;
                if (!ok)
                    for (auto &q : live)
                        if (!q->done && q->image == r.image && q->first < r.tag + r.r.first + r.r.count &&
                            r.tag + r.r.first < q->first + q->blocks)
                            q->mayFail = true;
                becameReady(rig.finish(r.r, ok));
                ++readsDone;
            } else if (op < 97 && !ready.empty()) {  // a ready request copies out and lets go
                const size_t i = rng() % ready.size();
                Request *q = ready[i];
                ready.erase(ready.begin() + long(i));
                ASSERT_FALSE(q->done) << "a request completed twice";
                ASSERT_EQ(q->cr.waiting, 0u);
                if (q->cr.failed) {
                    ASSERT_TRUE(q->mayFail) << "a request failed with no failed read of its blocks";
                    ++failedRequests;
                } else {
                    ASSERT_TRUE(rig.copyOut(q->cr, q->image, q->first, q->blocks)) << "wrong bytes copied out";
                }
                rig.cache.release(&q->cr, !q->cr.failed);
                q->done = true;
            } else if (op >= 97) {  // memory given back
                if (SwitchWindow *w = rig.cache.retire(uint32_t(rng() % (windowCount + 1)))) {
                    ASSERT_TRUE(w->retired);
                    ASSERT_EQ(w->users, 0u);
                    ASSERT_EQ(w->reading, 0u);
                    std::fill(rig.slots[size_t(w - rig.windows.data())].begin(),
                              rig.slots[size_t(w - rig.windows.data())].end(), kPoison);
                }
            }
            const char *why = rig.cache.check();
            ASSERT_EQ(why, nullptr) << why << " (round " << round << ", step " << step << ")";
        }
        // Drain: every read finishes, every request completes.
        while (!inflight.empty() || !ready.empty()) {
            if (!inflight.empty()) {
                const Read r = inflight.back();
                inflight.pop_back();
                for (uint32_t b = r.r.first; b < r.r.first + r.r.count; ++b)
                    --reading[{r.image, r.tag + b}];
                becameReady(rig.finish(r.r, true));
            }
            while (!ready.empty()) {
                Request *q = ready.back();
                ready.pop_back();
                ASSERT_TRUE(q->cr.failed ? q->mayFail : rig.copyOut(q->cr, q->image, q->first, q->blocks));
                failedRequests += q->cr.failed;
                rig.cache.release(&q->cr, !q->cr.failed);
                q->done = true;
            }
        }
        for (auto &q : live)
            ASSERT_TRUE(q->done) << "a request never completed";
        ASSERT_EQ(rig.cache.check(), nullptr);
        const SwitchCacheStats &s = rig.cache.stats();
        ASSERT_LE(s.aheadServed, s.aheadBlocks);
        ASSERT_LE(s.aheadServed, s.served);
    }
    printf("cryptex cache: %lu requests (%lu refused), %lu reads, %lu failed requests\n", requests, refused, readsDone,
           failedRequests);
    EXPECT_GT(failedRequests, 0u);
    EXPECT_GT(refused, 0u);
}

TEST(CryptexCache, TryHitInlineAndLarge)
{
    Rig rig(8, 32, 64, 64);
    SwitchCacheRead reads[kSwitchWindowReads];
    uint32_t n = 0;
    EXPECT_EQ(rig.cache.readAhead(0, 0, kSwitchWindowBlocks, rig.imageBlocks[0], reads, &n), kSwitchWindowBlocks);
    for (uint32_t i = 0; i < n; ++i)
        rig.finish(reads[i], true);
    EXPECT_EQ(rig.cache.readAhead(0, kSwitchWindowBlocks, kSwitchWindowBlocks, rig.imageBlocks[0], reads, &n),
              kSwitchWindowBlocks);
    for (uint32_t i = 0; i < n; ++i)
        rig.finish(reads[i], true);
    ASSERT_EQ(rig.cache.check(), nullptr);

    SwitchCacheRequest req = {};
    bool copied = false;
    uint32_t copiedBlocks = 0;
    const int small = rig.cache.tryHit(
        &req, 0, 6, 4, rig.imageBlocks[0], true,
        [&](const SwitchWindow *w, uint64_t from, uint64_t to) {
            copiedBlocks += static_cast<uint32_t>(to - from);
            const uint64_t *slot = static_cast<const uint64_t *>(w->bytes);
            for (uint64_t b = from; b < to; ++b) {
                if (slot[b - w->tag] != content(0, b))
                    return false;
            }
            return true;
        },
        &copied);
    EXPECT_EQ(small, 1);
    EXPECT_TRUE(copied);
    EXPECT_EQ(copiedBlocks, 4u);
    EXPECT_EQ(rig.cache.check(), nullptr);

    const int large = rig.cache.tryHit(
        &req, 0, 2, 12, rig.imageBlocks[0], false,
        [](const SwitchWindow *, uint64_t, uint64_t) {
            return false;
        },
        &copied);
    EXPECT_EQ(large, 2);
    EXPECT_EQ(req.windows, 2u);
    EXPECT_TRUE(rig.copyOut(req, 0, 2, 12));
    rig.cache.release(&req, true);
    EXPECT_EQ(rig.cache.check(), nullptr);

    EXPECT_EQ(rig.cache.tryHit(
                  &req, 0, 32, 2, rig.imageBlocks[0], true,
                  [](const SwitchWindow *, uint64_t, uint64_t) {
                      return false;
                  },
                  &copied),
              0);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

}  // namespace
