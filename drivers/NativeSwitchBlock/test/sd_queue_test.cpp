// Host test of drivers/NativeSwitchBlock/SwitchQueue.h: which queued reads join the SD worker's read chain. What the
// driver does with the chains is guarded by sd_queue_source_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "../SwitchQueue.h"
#include "../SwitchSdWriteFence.h"
#include "bazel/testing/require.h"

struct Request {
    Request *next;
    uint64_t lba;
    uint32_t sectors;
    bool write;
    bool chained;
    int id;
    bool shared = false;
};

// A queue built from (lba, sectors, write) in queue order; ids are queue positions (0 = oldest).
struct Queue {
    std::vector<Request> storage;
    Request *head = nullptr, *tail = nullptr;

    explicit Queue(const std::vector<std::tuple<uint64_t, uint32_t, bool>> &items) : storage(items.size())
    {
        for (size_t i = 0; i < items.size(); ++i) {
            storage[i] = {nullptr, std::get<0>(items[i]), std::get<1>(items[i]), std::get<2>(items[i]), false, int(i)};
            if (i)
                storage[i - 1].next = &storage[i];
        }
        head = storage.empty() ? nullptr : &storage[0];
        tail = storage.empty() ? nullptr : &storage.back();
    }

    Request *at(int id)
    {
        return &storage[size_t(id)];
    }

    std::vector<int> ids() const
    {
        std::vector<int> out;
        for (const Request *r = head; r; r = r->next)
            out.push_back(r->id);
        return out;
    }

    void checkTail() const
    {
        const Request *last = nullptr;
        for (const Request *r = head; r; r = r->next)
            last = r;
        REQUIRE(tail == last);
    }
};

struct Picked {
    std::vector<int> chain;
    uint64_t start;
    uint32_t sectors;
};

static Picked pick(Queue &q, int first, uint32_t limit = 2048, uint32_t maxCount = 256, unsigned scan = 128,
                   uint32_t maxGap = 0, uint32_t budget = 0)
{
    Picked p{};
    const uint32_t count =
        switchGatherReads(q.head, q.at(first), limit, maxCount, scan, &p.start, &p.sectors, maxGap, budget);
    // What the driver checks before unlinking.
    REQUIRE(switchChainFits(q.head, p.start, p.sectors, count, maxGap, budget));
    if (!maxGap && !budget)
        REQUIRE(switchChainTiles(q.head, p.start, p.sectors, count));
    Request *chain = switchUnlinkChain(q.head, q.tail, count);
    uint64_t at = p.start, gaps = 0;
    for (Request *r = chain; r; r = r->next) {
        // Card order; each gap within the bound (none without gaps).
        REQUIRE(r->lba >= at && r->lba - at <= maxGap && !r->write && !r->chained);
        gaps += r->lba - at;
        at = r->lba + r->sectors;
        p.chain.push_back(r->id);
    }
    REQUIRE(chain->lba == p.start && gaps <= budget);
    REQUIRE(p.chain.size() == count && at == p.start + p.sectors && p.sectors <= limit);
    for (const Request *r = q.head; r; r = r->next)
        REQUIRE(!r->chained);
    q.checkTail();
    return p;
}

static void basics()
{
    // A lone read.
    {
        Queue q({{100, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE(p.chain == std::vector<int>{0} && p.start == 100 && p.sectors == 8 && !q.head && !q.tail);
    }
    // Queued in rising order right behind each other (the old rule's case): one chain, one pass.
    {
        Queue q({{100, 8, false}, {108, 8, false}, {116, 16, false}, {500, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE((p.chain == std::vector<int>{0, 1, 2}) && p.sectors == 32);
        REQUIRE(q.ids() == std::vector<int>{3});
    }
    // Continuations anywhere in the queue, before and after, in any order: one chain in card order.
    {
        Queue q({{108, 8, false}, {900, 8, false}, {116, 8, false}, {100, 8, false}, {2000, 8, true}, {92, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE((p.chain == std::vector<int>{5, 3, 0, 2}) && p.start == 92 && p.sectors == 32);
        REQUIRE((q.ids() == std::vector<int>{1, 4}));
    }
    // The picked read may sit in the middle (a passing read): older requests before it stay queued, in order.
    {
        Queue q({{5000, 8, false}, {100, 8, false}, {6000, 8, true}, {108, 8, false}});
        Picked p = pick(q, 1);
        REQUIRE((p.chain == std::vector<int>{1, 3}));
        REQUIRE((q.ids() == std::vector<int>{0, 2}));
    }
}

static void ordering()
{
    // A read that continues the chain but overlaps an older queued write stays behind that write.
    {
        Queue q({{100, 8, false}, {108, 8, true}, {108, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE(p.chain == std::vector<int>{0});
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    // ...also when the write only partly overlaps it.
    {
        Queue q({{100, 8, false}, {110, 2, true}, {108, 8, false}});
        REQUIRE(pick(q, 0).chain == std::vector<int>{0});
    }
    // A newer read of blocks an older read also wants is not pulled ahead of it.
    {
        Queue q({{100, 8, false}, {104, 8, false}, {108, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE(p.chain ==
                std::vector<int>{0});  // 104+8 overlaps nothing older but does not continue; 108 overlaps 104
    }
    // An older read that continues the chain joins even past a newer write (it was ahead of that write anyway).
    {
        Queue q({{108, 8, false}, {200, 8, true}, {100, 8, false}});
        Picked p = pick(q, 2);  // a passing read picked from the back
        REQUIRE((p.chain == std::vector<int>{2, 0}) && p.start == 100);
        REQUIRE(q.ids() == std::vector<int>{1});
    }
    // Writes never join a read chain, even when they continue it.
    {
        Queue q({{100, 8, false}, {108, 8, true}, {116, 8, false}});
        Picked p = pick(q, 0);
        REQUIRE(p.chain == std::vector<int>{0});
    }
    // A write earlier in the queue than a contiguous read but not overlapping it does not stop it.
    {
        Queue q({{100, 8, false}, {5000, 8, true}, {108, 8, false}});
        REQUIRE((pick(q, 0).chain == std::vector<int>{0, 2}));
    }
}

static void limits()
{
    // The command's length: 1 MiB of sectors, never more; what does not fit stays queued.
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 300; ++i)
            items.push_back({uint64_t(i) * 8, 8, false});
        Queue q(items);
        Picked p = pick(q, 0, 2048, 256, 1000);
        REQUIRE(p.chain.size() == 256 && p.sectors == 2048);
        REQUIRE(q.ids().size() == 44 && q.ids().front() == 256);
    }
    // The request count.
    {
        Queue q({{0, 8, false}, {8, 8, false}, {16, 8, false}, {24, 8, false}});
        REQUIRE(pick(q, 0, 2048, 3).chain.size() == 3);
    }
    // A continuation that would pass the length limit is skipped, a shorter one still joins.
    {
        Queue q({{0, 8, false}, {8, 64, false}, {72, 8, false}, {1000, 8, false}});
        Picked p = pick(q, 0, 64);
        REQUIRE(p.chain == std::vector<int>{0});
        Queue r({{0, 8, false}, {8, 56, false}, {64, 8, false}});
        REQUIRE((pick(r, 0, 64).chain == std::vector<int>{0, 1}));
    }
    // Only the first `scan` queued requests are looked at.
    {
        Queue q({{0, 8, false}, {500, 8, false}, {600, 8, false}, {8, 8, false}});
        REQUIRE(pick(q, 0, 2048, 256, 3).chain == std::vector<int>{0});
        Queue r({{0, 8, false}, {500, 8, false}, {600, 8, false}, {8, 8, false}});
        REQUIRE((pick(r, 0, 2048, 256, 4).chain == std::vector<int>{0, 3}));
    }
    // Falling order still gathers everything (one request per pass).
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 40; ++i)
            items.push_back({uint64_t(39 - i) * 8, 8, false});
        Queue q(items);
        Picked p = pick(q, 0);
        REQUIRE(p.chain.size() == 40 && p.start == 0 && p.chain.front() == 39 && !q.head);
    }
}

// Random queues: every invariant the driver relies on, and that nothing that could still join was left out.
static void randomQueues()
{
    std::mt19937_64 random(0x5dc0de);
    for (unsigned round = 0; round < 20000; ++round) {
        const unsigned n = 1 + unsigned(random() % 40);
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t lba = (random() % 64) * 8;
            const uint32_t sectors = 8 * uint32_t(1 + random() % (random() % 4 ? 2 : 8));
            items.push_back({lba, sectors, random() % 5 == 0});
        }
        Queue q(items);
        std::vector<int> reads;
        for (unsigned i = 0; i < n; ++i)
            if (!std::get<2>(items[i]))
                reads.push_back(int(i));
        if (reads.empty())
            continue;
        const int first = reads[random() % reads.size()];
        const uint32_t limit = random() % 2 ? 2048 : 8 * uint32_t(1 + random() % 16);
        if (std::get<1>(items[size_t(first)]) > limit)
            continue;
        const std::vector<int> before = q.ids();
        Picked p = pick(q, first, limit, 256, 128);
        std::vector<bool> taken(n);
        for (int id : p.chain)
            taken[size_t(id)] = true;
        REQUIRE(taken[size_t(first)]);
        // Every member other than `first` overlaps no older request that is still queued.
        for (int id : p.chain) {
            if (id == first)
                continue;
            const Request &r = q.storage[size_t(id)];
            for (int older = 0; older < id; ++older)
                if (!taken[size_t(older)])
                    REQUIRE(!switchOverlaps(&q.storage[size_t(older)], &r));
        }
        // The rest of the queue keeps its order.
        std::vector<int> rest;
        for (int id : before)
            if (!taken[size_t(id)])
                rest.push_back(id);
        REQUIRE(q.ids() == rest);
        // Maximal: no queued read continues the chain within the limit without overlapping an older queued request.
        for (int id : rest) {
            const Request &r = q.storage[size_t(id)];
            if (r.write)
                continue;
            if (r.lba != p.start + p.sectors && r.lba + r.sectors != p.start)
                continue;
            if (p.sectors + r.sectors > limit)
                continue;
            bool overlaps = false;
            for (int older = 0; older < id && !overlaps; ++older)
                overlaps = !taken[size_t(older)] && switchOverlaps(&q.storage[size_t(older)], &r);
            REQUIRE(overlaps);
        }
    }
}

// The driver's check before it unlinks: a marked set that does not tile its range exactly is refused, and clearing the
// marks leaves the queue as it was.
static void tiling()
{
    Queue q({{100, 8, false}, {108, 8, false}, {104, 8, false}, {116, 8, true}, {124, 8, false}});
    auto mark = [&](std::vector<int> ids) {
        switchClearChain(q.head);
        for (int id : ids)
            q.at(id)->chained = true;
    };
    mark({0, 1});
    REQUIRE(switchChainTiles(q.head, 100, 16, 2));
    REQUIRE(!switchChainTiles(q.head, 100, 24, 2));  // a hole (lengths short of the range)
    REQUIRE(!switchChainTiles(q.head, 100, 16, 3));  // a member missing
    REQUIRE(!switchChainTiles(q.head, 104, 12, 2));  // a member outside the range
    mark({0, 1, 2});
    REQUIRE(!switchChainTiles(q.head, 100, 24, 3));  // overlapping members, even with the right total
    mark({0, 1, 3});
    REQUIRE(!switchChainTiles(q.head, 100, 24, 3));  // a write
    mark({});
    REQUIRE(switchChainTiles(q.head, 100, 0, 0));
    mark({4, 0});
    switchClearChain(q.head);
    for (const Request *r = q.head; r; r = r->next)
        REQUIRE(!r->chained);
    REQUIRE((q.ids() == std::vector<int>{0, 1, 2, 3, 4}));
}

// r14-sdcq: reads that are near the chain but do not continue it join through a gap, within the bounds.
static void gaps()
{
    // A read 8 sectors past the chain's end joins (gap 8); one 65 sectors past does not (bound 64).
    {
        Queue q({{100, 8, false}, {116, 8, false}, {189, 8, false}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE((p.chain == std::vector<int>{0, 1}) && p.start == 100 && p.sectors == 24);
        REQUIRE(q.ids() == std::vector<int>{2});
    }
    // ...before the chain's start too, and exactly at the bound.
    {
        Queue q({{1000, 8, false}, {928, 8, false}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE((p.chain == std::vector<int>{1, 0}) && p.start == 928 && p.sectors == 80);
    }
    // Without gap bounds nothing changes: the same queue tiles only.
    {
        Queue q({{100, 8, false}, {116, 8, false}});
        REQUIRE(pick(q, 0).chain == std::vector<int>{0});
    }
    // The budget: two 40-sector gaps fit 128 but not 64; the second stays queued.
    {
        Queue q({{100, 8, false}, {148, 8, false}, {196, 8, false}});
        REQUIRE((pick(q, 0, 2048, 256, 128, 64, 128).chain == std::vector<int>{0, 1, 2}));
        Queue r({{100, 8, false}, {148, 8, false}, {196, 8, false}});
        Picked p = pick(r, 0, 2048, 256, 128, 64, 64);
        REQUIRE((p.chain == std::vector<int>{0, 1}) && r.ids() == std::vector<int>{2});
    }
    // A read inside a gap fills it (taken in a later pass, or in the same one after the far read).
    {
        Queue q({{100, 8, false}, {124, 8, false}, {108, 8, false}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE((p.chain == std::vector<int>{0, 2, 1}) && p.sectors == 32);
    }
    // ...but not when it overlaps a member: it stays queued.
    {
        Queue q({{100, 8, false}, {124, 8, false}, {104, 8, false}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE((p.chain == std::vector<int>{0, 1}) && q.ids() == std::vector<int>{2});
    }
    // The command length counts the gaps.
    {
        Queue q({{0, 8, false}, {40, 8, false}});
        REQUIRE(pick(q, 0, 40, 256, 128, 64, 128).chain == std::vector<int>{0});
        Queue r({{0, 8, false}, {40, 8, false}});
        REQUIRE((pick(r, 0, 48, 256, 128, 64, 128).chain == std::vector<int>{0, 1}));
    }
    // A gap never lets a read pass an older write it overlaps, nor take a write.
    {
        Queue q({{100, 8, false}, {120, 8, true}, {120, 8, false}, {140, 8, true}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE(p.chain == std::vector<int>{0} && (q.ids() == std::vector<int>{1, 2, 3}));
    }
    // A write between the members on the card (in a gap) is no obstacle: the gap's bytes are discarded.
    {
        Queue q({{100, 8, false}, {110, 4, true}, {120, 8, false}});
        Picked p = pick(q, 0, 2048, 256, 128, 64, 128);
        REQUIRE((p.chain == std::vector<int>{0, 2}) && q.ids() == std::vector<int>{1});
    }
    // switchChainFits: the bounds it checks.
    {
        Queue q({{100, 8, false}, {116, 8, false}, {180, 8, false}});
        auto mark = [&](std::vector<int> ids) {
            switchClearChain(q.head);
            for (int id : ids)
                q.at(id)->chained = true;
        };
        mark({0, 1});
        REQUIRE(switchChainFits(q.head, 100, 24, 2, 8, 8) && !switchChainFits(q.head, 100, 24, 2, 7, 8) &&
                !switchChainFits(q.head, 100, 24, 2, 8, 7) && !switchChainTiles(q.head, 100, 24, 2));
        REQUIRE(!switchChainFits(q.head, 100, 28, 2, 64, 64));  // the range must end with a member
        REQUIRE(!switchChainFits(q.head, 96, 28, 2, 64, 64));   // ...and start with one
        mark({0, 1, 2});
        REQUIRE(switchChainFits(q.head, 100, 88, 3, 56, 64) && !switchChainFits(q.head, 100, 88, 3, 55, 64));
        switchClearChain(q.head);
    }
}

// Random queues with gap bounds: whatever joins fits the bounds, and no read moves ahead of an older overlapping request.
static void randomGaps()
{
    std::mt19937_64 random(0x9a95);
    for (unsigned round = 0; round < 20000; ++round) {
        const unsigned n = 1 + unsigned(random() % 40);
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (unsigned i = 0; i < n; ++i)
            items.push_back({(random() % 128) * 4, 4 * uint32_t(1 + random() % 4), random() % 5 == 0});
        Queue q(items);
        std::vector<int> reads;
        for (unsigned i = 0; i < n; ++i)
            if (!std::get<2>(items[i]))
                reads.push_back(int(i));
        if (reads.empty())
            continue;
        const int first = reads[random() % reads.size()];
        const uint32_t maxGap = uint32_t(random() % 3) * 16, budget = maxGap * uint32_t(random() % 3);
        const uint32_t limit = random() % 2 ? 2048 : 16 * uint32_t(1 + random() % 8);
        if (std::get<1>(items[size_t(first)]) > limit)
            continue;
        Picked p = pick(q, first, limit, 256, 128, maxGap, budget);  // pick() checks bounds, order and fit
        std::vector<bool> taken(n);
        for (int id : p.chain)
            taken[size_t(id)] = true;
        for (int id : p.chain) {
            if (id == first)
                continue;
            for (int older = 0; older < id; ++older)
                if (!taken[size_t(older)])
                    REQUIRE(!switchOverlaps(&q.storage[size_t(older)], &q.storage[size_t(id)]));
        }
    }
}

static Picked pickWrites(Queue &q, int first, uint32_t limit = 2048, uint32_t maxCount = 256, unsigned scan = 128,
                         const std::vector<SwitchLbaRange> &inFlight = {})
{
    Picked p{};
    const uint32_t count =
        switchGatherWrites(q.head, q.at(first), limit, maxCount, scan, &p.start, &p.sectors,
                           inFlight.empty() ? nullptr : inFlight.data(), static_cast<unsigned>(inFlight.size()));
    REQUIRE(switchWriteChainTiles(q.head, p.start, p.sectors, count));
    Request *chain = switchUnlinkChain(q.head, q.tail, count);
    uint64_t at = p.start;
    for (Request *r = chain; r; r = r->next) {
        REQUIRE(r->lba == at && r->write && !r->chained);
        at = r->lba + r->sectors;
        p.chain.push_back(r->id);
    }
    REQUIRE(chain->lba == p.start && p.chain.size() == count && at == p.start + p.sectors && p.sectors <= limit);
    for (const Request *r = q.head; r; r = r->next)
        REQUIRE(!r->chained);
    q.checkTail();
    return p;
}

// A synchronize's barrier counts write completions (SwitchSdWriteFence.h). While it waits, a write submitted after it
// must not complete in the place of an older write it waits for: W1 at 100 and W2 at 500 are queued, the barrier begins,
// then W3 at 108 arrives. Joined to W1's chain, W3 completed beside W1 and the barrier read done with W2 still queued.
static void writeFenceOrder()
{
    for (bool waiting : {true, false}) {
        Queue q({{100, 8, true}, {500, 8, true}, {108, 8, true}});
        SwitchSdWriteFence fence;
        fence.submitted = 2;
        SwitchSdWriteFence::Barrier barrier;
        if (waiting)
            fence.begin(barrier);
        ++fence.submitted;
        Picked p{};
        const uint32_t count = switchGatherWrites(q.head, q.at(0), 2048, 256, 128, &p.start, &p.sectors, nullptr, 0,
                                                  fence.waiters != nullptr);
        REQUIRE(switchWriteChainTiles(q.head, p.start, p.sectors, count));
        Request *chain = switchUnlinkChain(q.head, q.tail, count);
        for (Request *r = chain; r; r = r->next)
            p.chain.push_back(r->id);
        fence.finish(count, false);
        if (waiting) {
            REQUIRE(p.chain == std::vector<int>{0});
            REQUIRE(fence.completed < barrier.target);
            fence.end(barrier, false);
        } else {
            REQUIRE((p.chain == std::vector<int>{0, 2}));
        }
        REQUIRE(!fence.waiters);
    }
}

static void writeTiling()
{
    Queue q({{100, 8, true}, {108, 8, true}, {104, 8, true}, {116, 8, false}, {124, 8, true}});
    auto mark = [&](std::vector<int> ids) {
        switchClearChain(q.head);
        for (int id : ids)
            q.at(id)->chained = true;
    };
    mark({0, 1});
    REQUIRE(switchWriteChainTiles(q.head, 100, 16, 2));
    REQUIRE(!switchWriteChainTiles(q.head, 100, 24, 2));
    REQUIRE(!switchWriteChainTiles(q.head, 100, 16, 3));
    REQUIRE(!switchWriteChainTiles(q.head, 104, 12, 2));
    mark({0, 1, 2});
    REQUIRE(!switchWriteChainTiles(q.head, 100, 24, 3));
    mark({0, 1, 3});
    REQUIRE(!switchWriteChainTiles(q.head, 100, 24, 3));
    mark({0, 4});
    REQUIRE(!switchWriteChainTiles(q.head, 100, 32, 2));
    mark({});
    REQUIRE(switchWriteChainTiles(q.head, 100, 0, 0));
    REQUIRE(!switchWriteChainTiles(q.head, 100, 8, 0));
    REQUIRE(!switchChainFitsDirection(q.head, 100, 0, 0, 8, 8, true));
    switchClearChain(q.head);
}

static void writeBasics()
{
    {
        Queue q({{100, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE(p.chain == std::vector<int>{0} && p.start == 100 && p.sectors == 8 && !q.head && !q.tail);
    }
    {
        Queue q({{100, 8, true}, {108, 8, true}, {116, 16, true}, {500, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE((p.chain == std::vector<int>{0, 1, 2}) && p.sectors == 32);
        REQUIRE(q.ids() == std::vector<int>{3});
    }
    {
        Queue q({{108, 8, true}, {900, 8, false}, {116, 8, true}, {100, 8, true}, {2000, 8, true}, {92, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE((p.chain == std::vector<int>{5, 3, 0, 2}) && p.start == 92 && p.sectors == 32);
        REQUIRE((q.ids() == std::vector<int>{1, 4}));
    }
    // Strict contiguity only: a 1-sector gap prevents merging.
    {
        Queue q({{100, 8, true}, {109, 8, true}, {116, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE(p.chain == std::vector<int>{0} && p.sectors == 8);
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    // Falling order merges into rising card order.
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 40; ++i)
            items.push_back({uint64_t(39 - i) * 8, 8, true});
        Queue q(items);
        Picked p = pickWrites(q, 0);
        REQUIRE(p.chain.size() == 40 && p.start == 0 && p.sectors == 320 && p.chain.front() == 39 && !q.head);
    }
}

static void writeOrdering()
{
    // WAR: a write must not pass an older overlapping read (full or partial).
    {
        Queue q({{100, 8, true}, {108, 8, false}, {108, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE(p.chain == std::vector<int>{0});
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    {
        Queue q({{100, 8, true}, {110, 2, false}, {108, 8, true}});
        REQUIRE(pickWrites(q, 0).chain == std::vector<int>{0});
    }
    // WAW: a newer write behind an over-limit or partially overlapping older write stays behind it.
    {
        Queue q({{100, 8, true}, {108, 12, true}, {108, 8, true}});
        Picked p = pickWrites(q, 0, 16);
        REQUIRE(p.chain == std::vector<int>{0});
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    {
        Queue q({{100, 8, true}, {112, 8, true}, {108, 8, true}});
        Picked p = pickWrites(q, 0);
        REQUIRE(p.chain == std::vector<int>{0});
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    // Reads never join a write chain.
    {
        Queue q({{100, 8, true}, {108, 8, false}, {116, 8, true}});
        REQUIRE(pickWrites(q, 0).chain == std::vector<int>{0});
    }
    // Non-overlapping older reads and writes do not block a contiguous write.
    {
        Queue q({{100, 8, true}, {5000, 8, false}, {6000, 8, true}, {108, 8, true}});
        REQUIRE((pickWrites(q, 0).chain == std::vector<int>{0, 3}));
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
    // An older write ahead of a newer overlapping read still joins before that read.
    {
        Queue q({{108, 8, true}, {108, 8, false}, {100, 8, true}});
        Picked p = pickWrites(q, 2);
        REQUIRE((p.chain == std::vector<int>{2, 0}) && p.start == 100);
        REQUIRE(q.ids() == std::vector<int>{1});
    }
}

static void writeInFlight()
{
    // Candidate overlapping an active in-flight range is skipped while the opposite end still joins.
    {
        Queue q({{100, 8, true}, {108, 8, true}, {92, 8, true}});
        Picked p = pickWrites(q, 0, 2048, 256, 128, {{108, 8}});
        REQUIRE((p.chain == std::vector<int>{2, 0}) && p.start == 92 && p.sectors == 16);
        REQUIRE(q.ids() == std::vector<int>{1});
    }
    // Multiple in-flight ranges (active, prepared, stashed), partial overlap, empty range ignored, touching allowed.
    {
        Queue q({{100, 8, true}, {108, 8, true}, {92, 8, true}, {84, 8, true}, {116, 8, true}});
        const std::vector<SwitchLbaRange> slots = {{0, 0}, {112, 4}, {76, 8}, {500, 16}};
        Picked p = pickWrites(q, 0, 2048, 256, 128, slots);
        REQUIRE((p.chain == std::vector<int>{3, 2, 0}) && p.start == 84 && p.sectors == 24);
        REQUIRE((q.ids() == std::vector<int>{1, 4}));
    }
    // An older write blocked by inFlight also blocks a newer overlapping write outside inFlight.
    {
        Queue q({{100, 8, true}, {108, 8, true}, {108, 4, true}});
        Picked p = pickWrites(q, 0, 2048, 256, 128, {{112, 8}});
        REQUIRE(p.chain == std::vector<int>{0});
        REQUIRE((q.ids() == std::vector<int>{1, 2}));
    }
}

static void writeLimits()
{
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 300; ++i)
            items.push_back({uint64_t(i) * 8, 8, true});
        Queue q(items);
        Picked p = pickWrites(q, 0, 2048, 256, 1000);
        REQUIRE(p.chain.size() == 256 && p.sectors == 2048);
        REQUIRE(q.ids().size() == 44 && q.ids().front() == 256);
    }
    {
        Queue q({{0, 8, true}, {8, 8, true}, {16, 8, true}, {24, 8, true}});
        REQUIRE(pickWrites(q, 0, 2048, 3).chain.size() == 3);
    }
    {
        Queue q({{100, 8, true}, {500, 8, true}, {600, 8, true}, {108, 8, true}});
        REQUIRE(pickWrites(q, 0, 2048, 256, 3).chain == std::vector<int>{0});
        Queue r({{100, 8, true}, {500, 8, true}, {600, 8, true}, {108, 8, true}});
        REQUIRE((pickWrites(r, 0, 2048, 256, 4).chain == std::vector<int>{0, 3}));
    }
}

static void writeRandomQueues()
{
    std::mt19937_64 random(0x77726974ULL);
    for (unsigned round = 0; round < 20000; ++round) {
        const unsigned n = 1 + unsigned(random() % 40);
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t lba = (random() % 64) * 8;
            const uint32_t sectors = 8 * uint32_t(1 + random() % (random() % 4 ? 2 : 8));
            items.push_back({lba, sectors, random() % 4 != 0});
        }
        Queue q(items);
        std::vector<int> writes;
        for (unsigned i = 0; i < n; ++i)
            if (std::get<2>(items[i]))
                writes.push_back(int(i));
        if (writes.empty())
            continue;
        const int first = (round % 2 == 0 && std::get<2>(items[0])) ? 0 : writes[random() % writes.size()];
        const uint32_t limit = random() % 2 ? 2048 : 8 * uint32_t(1 + random() % 16);
        if (std::get<1>(items[size_t(first)]) > limit)
            continue;
        std::vector<SwitchLbaRange> inFlight;
        const unsigned slots = unsigned(random() % 4);
        for (unsigned s = 0; s < slots; ++s)
            inFlight.push_back({(random() % 64) * 8, random() % 5 == 0 ? 0u : 8u * uint32_t(1 + random() % 4)});
        const std::vector<int> before = q.ids();
        Picked p = pickWrites(q, first, limit, 256, 128, inFlight);
        std::vector<bool> taken(n);
        for (int id : p.chain)
            taken[size_t(id)] = true;
        REQUIRE(taken[size_t(first)]);
        for (int id : p.chain) {
            if (id == first)
                continue;
            const Request &r = q.storage[size_t(id)];
            for (const SwitchLbaRange &active : inFlight)
                REQUIRE(!switchOverlapsRange(&r, active));
            for (int older = 0; older < id; ++older)
                if (!taken[size_t(older)])
                    REQUIRE(!switchOverlaps(&q.storage[size_t(older)], &r));
        }
        std::vector<int> rest;
        for (int id : before)
            if (!taken[size_t(id)])
                rest.push_back(id);
        REQUIRE(q.ids() == rest);
        // Maximal: every remaining contiguous write within limit overlaps inFlight or an older unchained request.
        for (int id : rest) {
            const Request &r = q.storage[size_t(id)];
            if (!r.write)
                continue;
            if (r.lba != p.start + p.sectors && r.lba + r.sectors != p.start)
                continue;
            if (p.sectors + r.sectors > limit)
                continue;
            bool blocked = false;
            for (const SwitchLbaRange &active : inFlight)
                blocked |= switchOverlapsRange(&r, active);
            for (int older = 0; older < id && !blocked; ++older)
                blocked = !taken[size_t(older)] && switchOverlaps(&q.storage[size_t(older)], &r);
            REQUIRE(blocked);
        }
        // When `first` is head (as in pickLocked), executing the gathered chain then `rest` must match strict FIFO.
        if (first == 0) {
            constexpr size_t kDiskSectors = 64 * 8 + 64;
            std::vector<int> fifoDisk(kDiskSectors, -1), mergedDisk(kDiskSectors, -1);
            std::vector<std::vector<int>> fifoReads(n), mergedReads(n);
            auto runOp = [&](int id, std::vector<int> &disk, std::vector<std::vector<int>> &reads) {
                const Request &r = q.storage[size_t(id)];
                for (uint32_t s = 0; s < r.sectors; ++s) {
                    const size_t idx = size_t(r.lba + s);
                    if (r.write)
                        disk[idx] = id;
                    else
                        reads[size_t(id)].push_back(disk[idx]);
                }
            };
            for (int id : before)
                runOp(id, fifoDisk, fifoReads);
            for (int id : p.chain)
                runOp(id, mergedDisk, mergedReads);
            for (int id : rest)
                runOp(id, mergedDisk, mergedReads);
            REQUIRE(mergedDisk == fifoDisk);
            REQUIRE(mergedReads == fifoReads);
        }
    }
}

TEST(SdQueue, Tiling)
{
    tiling();
}

TEST(SdQueue, Basics)
{
    basics();
}

TEST(SdQueue, Ordering)
{
    ordering();
}

TEST(SdQueue, Limits)
{
    limits();
}

TEST(SdQueue, RandomQueues)
{
    randomQueues();
}

TEST(SdQueue, Gaps)
{
    gaps();
}

TEST(SdQueue, RandomGaps)
{
    randomGaps();
}

TEST(SdQueue, WriteFenceOrder)
{
    writeFenceOrder();
}

TEST(SdQueue, WriteTiling)
{
    writeTiling();
}

TEST(SdQueue, WriteBasics)
{
    writeBasics();
}

TEST(SdQueue, WriteOrdering)
{
    writeOrdering();
}

TEST(SdQueue, WriteInFlight)
{
    writeInFlight();
}

TEST(SdQueue, WriteLimits)
{
    writeLimits();
}

TEST(SdQueue, WriteRandomQueues)
{
    writeRandomQueues();
}

TEST(SdQueue, DistinctWindowsCannotShareCommands)
{
    Queue q({{100, 8, false}, {108, 8, false}, {120, 8, false}});
    q.at(1)->shared = true;
    q.at(2)->shared = true;
    REQUIRE(pick(q, 0, 2048, 256, 128, 64, 64).chain == std::vector<int>{0});
    REQUIRE((pick(q, 1, 2048, 256, 128, 64, 64).chain == std::vector<int>{1, 2}));
    Queue invalid({{100, 8, false}, {108, 8, false}});
    invalid.at(1)->shared = true;
    invalid.at(0)->chained = invalid.at(1)->chained = true;
    REQUIRE(!switchChainTiles(invalid.head, 100, 16, 2));

    Queue qw({{100, 8, true}, {108, 8, true}, {116, 8, true}});
    qw.at(1)->shared = true;
    qw.at(2)->shared = true;
    REQUIRE(pickWrites(qw, 0).chain == std::vector<int>{0});
    REQUIRE((pickWrites(qw, 1).chain == std::vector<int>{1, 2}));
    Queue invalidWrite({{100, 8, true}, {108, 8, true}});
    invalidWrite.at(1)->shared = true;
    invalidWrite.at(0)->chained = invalidWrite.at(1)->chained = true;
    REQUIRE(!switchWriteChainTiles(invalidWrite.head, 100, 16, 2));
}
