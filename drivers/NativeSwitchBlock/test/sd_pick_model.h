// A model of the SD card under the worker's pick (NativeSwitchBlock.cpp pickLocked), for sd_pick_test and
// sd_fence_test. The card takes ~0.58 ms a command plus ~10 us per KiB (r12-sdio's measured model) and 0.4 ms more for a
// write, and keeps two chains in its pipeline: the one on the card and the one prepared behind it, picked as the one
// before starts (worker()). Chains complete in pick order. A "fault" is SpringBoard's main thread taking a page fault: a
// 4 KiB APFS metadata read, then the 16 KiB data read, each issued 0.3 ms after the previous one completes.
//
// Synchronizes are modelled as SwitchSdWriteFence does them: a barrier of one window begins at a set time with the
// window's submitted writes as its target, waits while the window has completed fewer, and must not end before every
// write submitted ahead of it has completed.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "../SwitchSdPick.h"

namespace sdmodel {

constexpr uint32_t kSector = 512;
constexpr uint32_t kMaxSectors = 1024 * 1024 / kSector;
constexpr uint32_t kMaxChain = 256;
constexpr uint64_t kUs = 1000, kMs = 1000 * kUs;
constexpr uint64_t kThinkNs = 300 * kUs;
// switchSdPick's awaitedBeforeNs: no synchronize waiting, or one that began after every write.
constexpr uint64_t kNoSync[2] = {0, 0};
constexpr uint64_t kSyncAfterAll[2] = {~0ULL, ~0ULL};

struct Request {
    Request *next;
    uint64_t lba;
    uint32_t sectors;
    uint8_t klass;
    bool write;
    bool chained;
    uint64_t queuedNs;
    int id;
    bool ui;
    uint64_t doneNs;
    bool shared = false;  // the files window's, else the root window's
    bool done = false;
    bool replay = false;  // the kernel's boot replay read it (switchSdClassOf, switchSdAgeReplay)
};

enum class Rule {
    kOld,
    kNew
};

inline uint64_t serviceNs(uint32_t sectors, bool write)
{
    return 580 * kUs + static_cast<uint64_t>(sectors) * kSector / 1024 * 10 * kUs + (write ? 400 * kUs : 0);
}

// The rule pickLocked had before SwitchSdPick.h, as it was written there.
inline Request *oldFirst(Request *head, const uint32_t queuedReads[4], uint64_t now, Request **prev)
{
    *prev = nullptr;
    Request *first = head;
    if (first->next && first->klass && now - first->queuedNs < 100 * kMs) {
        unsigned better = 0;
        while (better < first->klass && !queuedReads[better])
            ++better;
        if (better < first->klass) {
            for (Request *before = first, *c = first->next; c; before = c, c = c->next) {
                if (c->write || c->klass != better)
                    continue;
                bool overlaps = false;
                for (const Request *older = head; older != c && !overlaps; older = older->next)
                    overlaps = switchOverlaps(older, c);
                if (overlaps)
                    continue;
                *prev = before;
                return c;
            }
        }
    }
    return first;
}

struct Arrival {
    uint64_t at;
    uint64_t lba;
    uint32_t sectors;
    uint8_t klass;
    bool write;
    bool shared = false;
    bool replay = false;
};

// A synchronize of one window, beginning at `at`.
struct Sync {
    uint64_t at;
    bool shared;
};

struct Result {
    uint64_t uiTotalNs = 0;     // first fault issued to last fault's data read done
    uint64_t uiWorstNs = 0;     // one UI request, queued to done
    uint64_t lowerWorstNs = 0;  // one request of the background, queued to done
    uint64_t worstByClass[4] = {};
    uint64_t replayWorstNs = 0;  // one boot replay read, queued to done
    uint64_t endNs = 0;
    size_t maxDepth = 0;
    bool ordered = true;      // no read started while an older overlapping write was still queued, and the reverse
    bool writesFirst = true;  // no write chain started while an older write of either window was still queued
    bool fenced = true;       // no synchronize ended before a write submitted ahead of it completed
    bool intact = true;       // after every pick the queue runs head to tail, depth requests, none of them chained,
                              // and queuedReplay counts its ageable replay reads
    unsigned syncsEnded = 0;
    unsigned outOfOrderWrites = 0;  // write chains that joined a write past an older one of its window still queued
    unsigned unfitChains = 0;       // gathered chains that did not fit, after which merges stay behind the first
};

class Card {
public:
    Card(Rule rule, std::vector<Arrival> background, int faults, uint64_t faultsAt, bool syncWaiting = false)
        : rule(rule), pending(std::move(background)), faultsLeft(faults), syncWaiting(syncWaiting)
    {
        storage.reserve(pending.size() + 2 * static_cast<size_t>(faults) + 1);
        std::sort(pending.begin(), pending.end(), [](const Arrival &a, const Arrival &b) {
            return a.at < b.at;
        });
        if (faults)
            uiNext = faultsAt;
        uiStart = faultsAt;
    }

    // Synchronizes at set times (each window's fence modelled), in place of a synchronize waiting the whole run.
    void syncAt(std::vector<Sync> syncs)
    {
        std::sort(syncs.begin(), syncs.end(), [](const Sync &a, const Sync &b) {
            return a.at < b.at;
        });
        planned = std::move(syncs);
        fencing = true;
    }

    // A synchronize's waiter leaves the fence (SwitchSdWriteFence::end) this long after its count is reached: the time
    // the woken thread takes to run.
    void wakeAfter(uint64_t ns)
    {
        wakeNs = ns;
    }

    // The rule before the synchronize's start time counted: while any synchronize waits, every write ranks with tier 0.
    void awaitEveryWrite()
    {
        awaitAll = true;
    }

    // pickLocked after a chain did not fit: requests merge only with the requests right behind them.
    void mergeOnlyBehind()
    {
        anywhere = false;
    }

    Result run()
    {
        for (;;) {
            const uint64_t arrival = nextArrival();
            if (active.empty() && arrival == kNever)
                break;
            if (!active.empty() && activeEnd <= arrival) {
                finishActive();
                continue;
            }
            admit(arrival);
        }
        result.endNs = now;
        return result;
    }

private:
    static constexpr uint64_t kNever = ~0ULL;

    struct Barrier {
        uint64_t target, beganNs;
        uint64_t endsAt = kNever;  // set once the count is reached
    };

    struct Fence {
        uint64_t submitted = 0, completed = 0;
        std::vector<Request *> writes;  // in submission order
        std::vector<Barrier> waiting;   // oldest first
    };

    Rule rule;
    std::vector<Arrival> pending;
    size_t pendingAt = 0;
    int faultsLeft;
    bool uiMeta = true;
    uint64_t uiNext = kNever, uiStart = 0;
    bool syncWaiting;
    bool fencing = false;
    bool anywhere = true;
    bool awaitAll = false;
    uint64_t wakeNs = 0;
    std::vector<Sync> planned;
    size_t plannedAt = 0;
    Fence fences[2];
    std::vector<Request> storage;
    Request *head = nullptr, *tail = nullptr;
    uint32_t queuedReads[4] = {};
    uint32_t queuedReplay = 0;
    uint32_t maxWriteSectors = kMaxSectors;  // the driver's maxWriteSectors: the card's own write limit
    SwitchSdPickState state = {};
    size_t depth = 0;
    std::vector<Request *> active, prepared;
    uint64_t activeEnd = 0, now = 0;
    std::mt19937_64 random{7};
    Result result;

    bool waiting(bool shared) const
    {
        if (!fencing)
            return syncWaiting;
        return std::any_of(fences[shared].waiting.begin(), fences[shared].waiting.end(), [&](const Barrier &b) {
            return b.endsAt > now;
        });
    }

    uint64_t newestBeganNs(bool shared) const
    {
        for (auto it = fences[shared].waiting.rbegin(); it != fences[shared].waiting.rend(); ++it) {
            if (it->endsAt > now)
                return it->beganNs;
        }
        return 0;
    }

    // SwitchSdWriteFence::awaitedBeforeNs() of each window.
    void awaited(uint64_t out[2]) const
    {
        for (int w = 0; w < 2; ++w) {
            if (!fencing)
                out[w] = syncWaiting ? ~0ULL : 0;
            else if (awaitAll)
                out[w] = waiting(false) || waiting(true) ? ~0ULL : 0;
            else
                out[w] = newestBeganNs(w == 1);
        }
    }

    uint64_t nextArrival() const
    {
        const uint64_t background = pendingAt < pending.size() ? pending[pendingAt].at : kNever;
        const uint64_t sync = plannedAt < planned.size() ? planned[plannedAt].at : kNever;
        return std::min({background, uiNext, sync});
    }

    void enqueue(const Arrival &a, bool ui)
    {
        storage.push_back({nullptr, a.lba, a.sectors, a.klass, a.write, false, now, static_cast<int>(storage.size()),
                           ui, 0, a.shared, false, a.replay});
        Request *r = &storage.back();
        (tail ? tail->next : head) = r;
        tail = r;
        result.maxDepth = std::max(result.maxDepth, ++depth);
        if (r->write) {
            ++fences[r->shared].submitted;
            fences[r->shared].writes.push_back(r);
        } else {
            ++queuedReads[r->klass];
        }
        if (switchSdAgeable(r))
            ++queuedReplay;
    }

    void admit(uint64_t at)
    {
        now = at;
        if (plannedAt < planned.size() && planned[plannedAt].at == at) {
            Fence &fence = fences[planned[plannedAt++].shared];
            fence.waiting.push_back({fence.submitted, at});
            endSyncs(fence);
        } else if (uiNext == at) {
            const uint64_t lba = 40000000 + random() % 20000000;
            enqueue({at, lba, uiMeta ? 8u : 32u, 0, false}, true);
            uiNext = kNever;
        } else {
            enqueue(pending[pendingAt++], false);
        }
        if (active.empty()) {
            active = pick();
            activeEnd = now + chainNs(active);
        }
        if (prepared.empty())
            prepared = pick();
    }

    void finishActive()
    {
        now = activeEnd;
        const std::vector<Request *> done = active;
        active = prepared;
        prepared.clear();
        if (active.empty())
            active = pick();
        if (!active.empty())
            activeEnd = now + chainNs(active);
        prepared = pick();
        for (Request *r : done)
            complete(r);
        if (!done.empty() && done.front()->write) {
            Fence &fence = fences[done.front()->shared];
            fence.completed += done.size();
            endSyncs(fence);
        }
    }

    // Ends the barriers whose count is reached, checking each one's writes are all done.
    void endSyncs(Fence &fence)
    {
        for (Barrier &barrier : fence.waiting) {
            if (barrier.endsAt != kNever || fence.completed < barrier.target)
                continue;
            for (uint64_t i = 0; i < barrier.target; ++i)
                result.fenced = result.fenced && fence.writes[i]->done;
            ++result.syncsEnded;
            barrier.endsAt = now + wakeNs;
        }
    }

    void complete(Request *r)
    {
        r->doneNs = now;
        r->done = true;
        const uint64_t latency = now - r->queuedNs;
        if (!r->ui) {
            result.lowerWorstNs = std::max(result.lowerWorstNs, latency);
            result.worstByClass[r->klass] = std::max(result.worstByClass[r->klass], latency);
            if (r->replay)
                result.replayWorstNs = std::max(result.replayWorstNs, latency);
            return;
        }
        result.uiWorstNs = std::max(result.uiWorstNs, latency);
        if (uiMeta) {
            uiMeta = false;
            uiNext = now + kThinkNs;
        } else if (--faultsLeft > 0) {
            uiMeta = true;
            uiNext = now + kThinkNs;
        } else {
            result.uiTotalNs = now - uiStart;
        }
    }

    static uint64_t chainNs(const std::vector<Request *> &chain)
    {
        if (chain.empty())
            return 0;
        const uint64_t low = chain.front()->lba;
        const uint64_t high = chain.back()->lba + chain.back()->sectors;
        return serviceNs(static_cast<uint32_t>(high - low), chain.front()->write);
    }

    // pickLocked's fallback: `first` and the requests right behind it that continue it on the card, in its window and
    // direction (a passing read's chain must not overlap anything it passes either), unlinked by `prev`, first's
    // predecessor, as the driver does. Returns the chain's count; `first` heads it.
    std::vector<SwitchLbaRange> inFlight() const
    {
        std::vector<SwitchLbaRange> ranges;
        for (const Request *r : active)
            ranges.push_back({r->lba, r->sectors});
        return ranges;
    }

    uint32_t unlinkBehind(Request *first, Request *prev)
    {
        const auto overlapsOlder = [&](const Request *later) {
            for (const Request *older = head; prev && older != first; older = older->next) {
                if (switchOverlaps(older, later))
                    return true;
            }
            return false;
        };
        const std::vector<SwitchLbaRange> flight = inFlight();
        const auto overlapsInFlight = [&](const Request *later) {
            for (const SwitchLbaRange &range : flight) {
                if (first->write && switchOverlapsRange(later, range))
                    return true;
            }
            return false;
        };
        Request *last = first;
        uint32_t count = 1;
        uint32_t sectors = first->sectors;
        while (count < kMaxChain && last->next && last->next->shared == first->shared &&
               last->next->write == first->write && last->next->lba == last->lba + last->sectors &&
               sectors + last->next->sectors <= (first->write ? maxWriteSectors : kMaxSectors) &&
               !overlapsOlder(last->next) && !overlapsInFlight(last->next)) {
            last = last->next;
            sectors += last->sectors;
            ++count;
        }
        for (const Request *r = first; r != last->next; r = r->next) {
            for (const Request *older = head; older != first; older = older->next) {
                if ((older->write || r->write) && switchOverlaps(older, r))
                    result.ordered = false;
            }
        }
        if (prev)
            prev->next = last->next;
        else
            head = last->next;
        if (tail == last)
            tail = prev;
        last->next = nullptr;
        return count;
    }

    std::vector<Request *> pick()
    {
        if (!head)
            return {};
        Request *prev = nullptr;
        Request *first = head;
        if (rule == Rule::kOld) {
            first = oldFirst(head, queuedReads, now, &prev);
        } else {
            uint64_t awaitedBeforeNs[2];
            awaited(awaitedBeforeNs);
            switchSdAgeReplay(head, now, queuedReads, queuedReplay);
            const SwitchSdPicked<Request> picked = switchSdPick(head, awaitedBeforeNs, now, &state);
            first = picked.first;
            prev = picked.prev;
        }
        for (const Request *older = head; first->write && older != first; older = older->next)
            result.writesFirst = result.writesFirst && !older->write;
        uint64_t start = 0;
        uint32_t sectors = 0;
        uint32_t count = 0;
        Request *chain = nullptr;
        // pickLocked's dequeue with gapsOn false (no gap sink): a gathered chain that does not fit turns merging
        // anywhere off for good, and that pick takes the run behind its first request.
        bool gathered = false;
        if (anywhere) {
            const std::vector<SwitchLbaRange> flight = inFlight();
            bool fits = false;
            if (first->write) {
                count = switchGatherWrites(head, first, maxWriteSectors, kMaxChain, kMaxChain, &start, &sectors,
                                           flight.data(), static_cast<unsigned>(flight.size()),
                                           rule == Rule::kNew && waiting(first->shared));
                fits = switchWriteChainTiles(head, start, sectors, count);
            } else {
                count = switchGatherReads(head, first, kMaxSectors, kMaxChain, kMaxChain, &start, &sectors);
                fits = switchChainFits(head, start, sectors, count, 0, 0);
            }
            if (fits) {
                checkOrder();
                if (first->write)
                    countOutOfOrder(first);
                chain = switchUnlinkChain(head, tail, count);
                gathered = true;
            } else {
                switchClearChain(head);
                anywhere = false;
                ++result.unfitChains;
            }
        }
        if (!gathered) {
            count = unlinkBehind(first, prev);
            chain = first;
        }
        depth -= count;
        checkQueue();
        std::vector<Request *> out;
        out.reserve(count);
        for (Request *r = chain; r; r = r->next) {
            if (!r->write)
                --queuedReads[r->klass];
            if (switchSdAgeable(r))
                --queuedReplay;
            out.push_back(r);
        }
        checkReplayCount();
        return out;
    }

    void countOutOfOrder(const Request *first)
    {
        bool skipped = false;
        for (const Request *r = first; r; r = r->next) {
            if (!r->write || r->shared != first->shared)
                continue;
            if (!r->chained) {
                skipped = true;
            } else if (skipped) {
                ++result.outOfOrderWrites;
                return;
            }
        }
    }

    void checkQueue()
    {
        size_t n = 0;
        const Request *last = nullptr;
        for (const Request *r = head; r; r = r->next, ++n) {
            if (r->chained)
                result.intact = false;
            last = r;
        }
        if (n != depth || last != tail)
            result.intact = false;
    }

    void checkReplayCount()
    {
        uint32_t ageable = 0;
        for (const Request *r = head; r; r = r->next)
            ageable += switchSdAgeable(r);
        if (ageable != queuedReplay)
            result.intact = false;
    }

    // Every marked member: no older unmarked request still queued that it must not pass.
    void checkOrder()
    {
        for (const Request *r = head; r; r = r->next) {
            if (!r->chained)
                continue;
            for (const Request *older = head; older != r; older = older->next) {
                if (!older->chained && (older->write || r->write) && switchOverlaps(older, r))
                    result.ordered = false;
            }
        }
    }
};

inline std::vector<Arrival> scattered(int n, uint64_t spreadNs, uint32_t sectors, uint8_t klass, bool write,
                                      uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back({random() % (spreadNs + 1), random() % 30000000, sectors, klass, write});
    return out;
}

// One file read front to back in 512 KiB reads, one every `everyNs` (the GPU pack warm).
inline std::vector<Arrival> stream(int n, uint64_t everyNs, uint8_t klass)
{
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back(
            {static_cast<uint64_t>(i) * everyNs, 70000000 + static_cast<uint64_t>(i) * 1024, 1024, klass, false});
    return out;
}

inline std::vector<Arrival> join(std::vector<Arrival> a, const std::vector<Arrival> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

inline void report(const char *name, const Result &before, const Result &after)
{
    std::printf("%-40s UI 10 faults %7.1f -> %7.1f ms, worst UI request %6.1f -> %6.1f ms, worst background %7.1f -> "
                "%7.1f ms\n",
                name, before.uiTotalNs / 1e6, after.uiTotalNs / 1e6, before.uiWorstNs / 1e6, after.uiWorstNs / 1e6,
                before.lowerWorstNs / 1e6, after.lowerWorstNs / 1e6);
}

}  // namespace sdmodel
