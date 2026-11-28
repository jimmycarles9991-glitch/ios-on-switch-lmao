// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which queued request the SD worker starts its next chain with (NativeSwitchBlock.cpp pickLocked), and the raise-only
// reprioritisation behind doSetPriority. Free of kernel types: test/sd_pick_test.cpp runs this code in a model of the
// card.
//
// A request's rank is its IOStoragePriority class (0 = I/O tier 0, the UI's page-ins) doubled, plus one for a write:
// a read goes before a write of its own class, because a write's completion blocks no thread unless a synchronize
// waits for it. The writes a waiting synchronize counts rank with tier-0 reads (IOSCHED puts metadata writes at tier 1,
// and the waiter may be the UI); a write queued after it began keeps its own tier. `awaitedBeforeNs` holds, per window
// (indexed by `shared`), SwitchSdWriteFence::awaitedBeforeNs(). The oldest request of the best rank goes before a head
// of worse rank, past every older request it is allowed to pass; for a write that is only the oldest write.
//
// Nothing ages back into FIFO order: a head passed over for kSwitchSdStarveNs goes first once, then again after
// kSwitchSdShareNs or kSwitchSdShareChains passes, whichever comes first, so lower classes keep at least one chain in
// kSwitchSdShareChains under a tier-0 flood. A tier-0 read waits for the chain on the card, the chain prepared behind
// it, older tier-0 requests, and those passed-over chains.
#pragma once

#include <stdint.h>

#include "SwitchQueue.h"

constexpr unsigned kSwitchSdPickClasses = 4;
constexpr uint64_t kSwitchSdStarveNs = 100000000;  // 100 ms
constexpr uint64_t kSwitchSdShareNs = 50000000;    // 50 ms
constexpr uint32_t kSwitchSdShareChains = 8;

struct SwitchSdPickState {
    uint64_t lowerDueNs;  // a passed-over head may go first again from here
    uint32_t passes;      // chains taken ahead of the head since it last went first
};

template <typename Request> struct SwitchSdPicked {
    Request *first;
    Request *prev;  // first's predecessor in the queue, nullptr when first is the head
    bool passed;    // first was taken before older requests
    bool starved;   // the head went first because it had been passed over for too long
};

template <typename Request> inline unsigned switchSdRank(const Request *r, const uint64_t awaitedBeforeNs[2])
{
    if (!r->write)
        return 2u * r->klass;
    const uint64_t awaited = awaitedBeforeNs[r->shared ? 1 : 0];
    return awaited && r->queuedNs <= awaited ? 0u : 2u * r->klass + 1u;
}

// The oldest request older than `r` that `r` may not pass (and its predecessor), or nullptr. A read may not pass a
// write it overlaps; a write passes no request it overlaps and no older write, because the write fence counts
// completions in submission order.
template <typename Request> inline Request *switchSdBlocker(Request *head, const Request *r, Request **prev)
{
    *prev = nullptr;
    for (Request *before = nullptr, *older = head; older != r; before = older, older = older->next) {
        if ((older->write && r->write) || ((older->write || r->write) && switchOverlaps(older, r))) {
            *prev = before;
            return older;
        }
    }
    return nullptr;
}

// How far a better request's rank is lent down the requests that hold it back before the pick gives up on it.
constexpr unsigned kSwitchSdLendHops = 4;

// The oldest request of the best rank goes first. One that may not pass an older request lends its rank to the oldest
// request holding it back, and that one's own oldest blocker in turn, until one may go: a tier-0 page-in behind a
// tier-3 write it overlaps, or a write a synchronize waits for behind older writes, does not wait for the share turn.
template <typename Request>
inline SwitchSdPicked<Request> switchSdPick(Request *head, const uint64_t awaitedBeforeNs[2], uint64_t now,
                                            SwitchSdPickState *state)
{
    Request *first = head;
    Request *prev = nullptr;
    unsigned firstRank = switchSdRank(head, awaitedBeforeNs);
    for (Request *before = head, *candidate = head->next; candidate && firstRank;
         before = candidate, candidate = candidate->next) {
        const unsigned rank = switchSdRank(candidate, awaitedBeforeNs);
        if (rank >= firstRank)
            continue;
        Request *go = candidate;
        Request *goPrev = before;
        for (unsigned hops = 0; go; ++hops) {
            Request *blockerPrev = nullptr;
            Request *const blocker = switchSdBlocker(head, go, &blockerPrev);
            if (!blocker)
                break;
            go = hops < kSwitchSdLendHops ? blocker : nullptr;
            goPrev = blockerPrev;
        }
        if (go) {
            first = go;
            prev = goPrev;
            firstRank = rank;
        }
    }
    if (first == head)
        return {head, nullptr, false, false};
    if (now - head->queuedNs >= kSwitchSdStarveNs &&
        (now >= state->lowerDueNs || state->passes >= kSwitchSdShareChains)) {
        state->lowerDueNs = now + kSwitchSdShareNs;
        state->passes = 0;
        return {head, nullptr, false, true};
    }
    ++state->passes;
    return {first, prev, true, false};
}

// The boot replay's reads (the kernel's prefetch thread, kernel/switch/vm/prefetch.c) are tier 0 by policy but read
// ahead of use: they queue one class below the UI's demand reads. A tier-0 thread can still wait on a replay read: on a
// VM page it holds busy, which XNU's busy-page raise (doSetPriority) lifts, or on a buffer the replay's vget or pagein
// read for APFS, which no raise reaches. So a replay read that has waited kSwitchSdReplayAgeNs goes back to tier 0's
// class (switchSdAgeReplay), and the UI waits at most that long behind it.
constexpr uint8_t kSwitchSdReplayClass = 1;
constexpr uint64_t kSwitchSdReplayAgeNs = 20000000;  // 20 ms

// The kernel's boot replay thread (kernel/switch/vm/prefetch_vm.c), set for the length of the replay. The drivers are
// linked into the kernel; host tests have no definition and never call it.
extern "C" bool switch_vm_prefetch_is_replay_thread(void *thread) __attribute__((weak));

inline uint8_t switchSdClassOf(uint8_t asked, bool replay)
{
    return replay && asked < kSwitchSdReplayClass ? kSwitchSdReplayClass : asked;
}

// A queued request the ager may move: a replay read in the replay class. A replay read asked at a lower priority keeps
// it. Their count (queuedReplay) gates the ager's walk, so tier-1 and cryptex reads in class 1 cost a pick nothing.
template <typename Request> inline bool switchSdAgeable(const Request *r)
{
    return r->replay && !r->write && r->klass == kSwitchSdReplayClass;
}

// Puts each replay read queued for kSwitchSdReplayAgeNs or more back in class 0, keeping queuedReads (reads only) and
// queuedReplay in step. Returns how many it moved.
template <typename Request>
inline uint32_t switchSdAgeReplay(Request *head, uint64_t now, uint32_t queuedReads[kSwitchSdPickClasses],
                                  uint32_t &queuedReplay)
{
    uint32_t aged = 0;
    for (Request *r = head; r && queuedReplay; r = r->next) {
        if (!switchSdAgeable(r) || now - r->queuedNs < kSwitchSdReplayAgeNs)
            continue;
        --queuedReads[r->klass];
        ++queuedReads[0];
        --queuedReplay;
        r->klass = 0;
        ++aged;
    }
    return aged;
}

// Raises every queued request overlapping [lba, lba + sectors) to `klass` (never lowers one), keeping queuedReads and
// queuedReplay in step. Returns how many it raised. Requests already taken into a chain are not in the queue and keep
// their class.
template <typename Request>
inline uint32_t switchSdPromote(Request *head, uint64_t lba, uint64_t sectors, uint8_t klass,
                                uint32_t queuedReads[kSwitchSdPickClasses], uint32_t &queuedReplay)
{
    uint32_t raised = 0;
    for (Request *r = sectors ? head : nullptr; r; r = r->next) {
        if (r->klass <= klass || !(r->lba < lba + sectors && lba < r->lba + r->sectors))
            continue;
        if (!r->write) {
            --queuedReads[r->klass];
            ++queuedReads[klass];
        }
        const bool wasAgeable = switchSdAgeable(r);
        r->klass = klass;
        if (wasAgeable != switchSdAgeable(r))
            wasAgeable ? --queuedReplay : ++queuedReplay;
        ++raised;
    }
    return raised;
}
