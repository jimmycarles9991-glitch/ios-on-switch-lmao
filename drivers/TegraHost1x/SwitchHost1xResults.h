// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchHost1xResult {
    SwitchHost1xResult *next = nullptr;
    uint64_t first = 0, last = 0, generation = 0;
    uint32_t references = 0;
    int result = 0, physicalResult = 0;
    bool registered = false, completed = false;
};

struct SwitchHost1xFailure {
    uint64_t first, last, generation;
    int result, physicalResult;
};

struct SwitchHost1xResultState {
    static constexpr uint32_t kBuckets = 256, kRecent = 64, kMaximum = 4096;
    IOLock *lock = nullptr;
    SwitchHost1xResult initial[kRecent] = {};
    SwitchHost1xResult *buckets[kBuckets] = {}, *recent[kRecent] = {}, *free = nullptr;
    uint32_t syncpoint = 0, allocated = kRecent, nextRecent = 0, nextFailure = 0;
    uint64_t generation = 1, retiredThrough = 0;
    // A completed job whose record is gone reads as completed, or as its failure while it is among the newest
    // kRecent failures kept here. An older failure pushed out by newer ones reads as completed.
    SwitchHost1xFailure failures[kRecent] = {};
};

// Kernel C++ does not coalesce template inline variables across translation units.
extern SwitchHost1xResultState switch_vic_result_state;
extern SwitchHost1xResultState switch_nvdec_result_state;

template <uint32_t Engine, SwitchHost1xResultState &State =
                               (Engine == SWITCH_GPU_ENGINE_VIC ? switch_vic_result_state : switch_nvdec_result_state)>
class SwitchHost1xResults {
    static constexpr uint32_t kBuckets = SwitchHost1xResultState::kBuckets;
    static constexpr uint32_t kRecent = SwitchHost1xResultState::kRecent;
    static constexpr uint32_t kMaximum = SwitchHost1xResultState::kMaximum;

    static SwitchHost1xResult *find(const switch_gpu_backing_token &token)
    {
        if (token.engine != Engine || token.queue != State.syncpoint)
            return nullptr;
        for (auto *record = State.buckets[token.value % kBuckets]; record; record = record->next)
            if (record->last == token.value && record->generation == token.reset_generation)
                return record;
        return nullptr;
    }

    static void retireRecord(const SwitchHost1xResult &record)
    {
        if (record.last > State.retiredThrough)
            State.retiredThrough = record.last;
        if (record.result >= 0 && record.physicalResult >= 0)
            return;
        State.failures[State.nextFailure] = {record.first, record.last, record.generation, record.result,
                                             record.physicalResult};
        State.nextFailure = (State.nextFailure + 1) % kRecent;
    }

    static const SwitchHost1xFailure *failure(uint64_t sequence, const uint64_t *generation)
    {
        const SwitchHost1xFailure *found = nullptr;
        for (const auto &entry : State.failures)
            if (entry.generation && entry.first <= sequence && sequence <= entry.last &&
                (generation ? entry.generation == *generation : !found || found->generation < entry.generation))
                found = &entry;
        return found;
    }

    static bool retired(uint64_t sequence, const uint64_t *generation)
    {
        return sequence && sequence <= State.retiredThrough && (!generation || *generation == State.generation);
    }

    static bool retiredToken(const switch_gpu_backing_token &token)
    {
        return token.engine == Engine && token.queue == State.syncpoint &&
               retired(token.value, &token.reset_generation);
    }

    static int retiredResult(uint64_t sequence, const uint64_t *generation)
    {
        const auto *entry = failure(sequence, generation);
        return entry && entry->result < 0 ? entry->result : 1;
    }

    static int retiredPhysical(const switch_gpu_backing_token &token)
    {
        const auto *entry = failure(token.value, &token.reset_generation);
        return entry && entry->physicalResult < 0 ? entry->physicalResult : 1;
    }

    static void drop(SwitchHost1xResult *record)
    {
        if (--record->references)
            return;
        if (record->completed)
            retireRecord(*record);
        if (record->registered) {
            auto **at = &State.buckets[record->last % kBuckets];
            while (*at && *at != record)
                at = &(*at)->next;
            if (*at)
                *at = record->next;
        }
        *record = {};
        record->next = State.free;
        State.free = record;
    }

public:
    static bool start(uint32_t id)
    {
        if (State.lock)
            return State.syncpoint == id;
        State.lock = IOLockAlloc();
        if (!State.lock)
            return false;
        State.syncpoint = id;
        for (auto &record : State.initial) {
            record.next = State.free;
            State.free = &record;
        }
        return true;
    }

    static SwitchHost1xResult *allocate()
    {
        IOLockLock(State.lock);
        auto *record = State.free;
        if (record)
            State.free = record->next;
        const bool grow = !record && State.allocated < kMaximum;
        if (grow)
            ++State.allocated;
        IOLockUnlock(State.lock);
        if (grow) {
            record = IONewZero(SwitchHost1xResult, 1);
            if (!record) {
                IOLockLock(State.lock);
                --State.allocated;
                IOLockUnlock(State.lock);
            }
        }
        if (record) {
            *record = {};
            record->references = 1;
        }
        return record;
    }

    static int prepare(SwitchHost1xResult *record, uint64_t first, uint64_t last, switch_gpu_backing_token *token)
    {
        if (!record || !first || last < first || !token)
            return -22;
        IOLockLock(State.lock);
        *token = {last, State.generation, Engine, State.syncpoint};
        const int error = !State.generation ? -75 : record->registered || find(*token) ? -16 : 0;
        if (!error) {
            record->first = first;
            record->last = last;
            record->generation = State.generation;
            record->registered = true;
            record->next = State.buckets[last % kBuckets];
            State.buckets[last % kBuckets] = record;
        }
        IOLockUnlock(State.lock);
        return error;
    }

    static void cancel(SwitchHost1xResult **owned)
    {
        if (!owned || !*owned)
            return;
        IOLockLock(State.lock);
        auto *record = *owned;
        *owned = nullptr;
        if (record->registered && record->references > 1 && record->generation == State.generation) {
            ++State.generation;
        }
        record->result = -125;
        IOLockWakeup(State.lock, record, false);
        drop(record);
        IOLockUnlock(State.lock);
    }

    static void complete(SwitchHost1xResult **owned, int result)
    {
        if (!owned || !*owned)
            return;
        IOLockLock(State.lock);
        auto *record = *owned;
        *owned = nullptr;
        record->result = result < 0 ? result : 1;
        record->completed = true;
        if (auto *old = State.recent[State.nextRecent])
            drop(old);
        State.recent[State.nextRecent] = record;
        State.nextRecent = (State.nextRecent + 1) % kRecent;
        IOLockWakeup(State.lock, record, false);
        IOLockUnlock(State.lock);
    }

    static void physical(SwitchHost1xResult *record, int result)
    {
        if (!record)
            return;
        IOLockLock(State.lock);
        record->physicalResult = result < 0 ? result : 1;
        IOLockUnlock(State.lock);
    }

    static int physicalStatus(const switch_gpu_backing_token *token)
    {
        if (!token)
            return -22;
        IOLockLock(State.lock);
        const auto *record = find(*token);
        const int result = record                 ? record->physicalResult
                           : retiredToken(*token) ? retiredPhysical(*token)
                                                  : SWITCH_GPU_BACKING_STALE;
        IOLockUnlock(State.lock);
        return result;
    }

    static void semantic(const switch_gpu_backing_token &token, int result)
    {
        IOLockLock(State.lock);
        if (auto *record = find(token)) {
            record->result = result < 0 ? result : 1;
            IOLockWakeup(State.lock, record, false);
        }
        IOLockUnlock(State.lock);
    }

    static int status(const switch_gpu_backing_token *token)
    {
        if (!token)
            return -22;
        IOLockLock(State.lock);
        const auto *record = find(*token);
        const int result = record                 ? record->result
                           : retiredToken(*token) ? retiredResult(token->value, &token->reset_generation)
                                                  : SWITCH_GPU_BACKING_STALE;
        IOLockUnlock(State.lock);
        return result;
    }

    static int retain(const switch_gpu_backing_token *token)
    {
        if (!token)
            return -22;
        IOLockLock(State.lock);
        auto *record = find(*token);
        int result = record || retiredToken(*token) ? 0 : SWITCH_GPU_BACKING_STALE;
        if (record) {
            if (record->references == UINT32_MAX)
                result = -75;
            else
                ++record->references;
        }
        IOLockUnlock(State.lock);
        return result;
    }

    static void release(const switch_gpu_backing_token *token)
    {
        if (!token)
            return;
        IOLockLock(State.lock);
        if (auto *record = find(*token))
            drop(record);
        IOLockUnlock(State.lock);
    }

    static int wait(const switch_gpu_backing_token *token)
    {
        if (!token)
            return -22;
        IOLockLock(State.lock);
        auto *record = find(*token);
        if (!record) {
            const int result =
                retiredToken(*token) ? retiredResult(token->value, &token->reset_generation) : SWITCH_GPU_BACKING_STALE;
            IOLockUnlock(State.lock);
            return result;
        }
        if (record->references == UINT32_MAX) {
            IOLockUnlock(State.lock);
            return -75;
        }
        ++record->references;
        while (!record->result)
            IOLockSleep(State.lock, record, THREAD_UNINT);
        const int result = record->result;
        drop(record);
        IOLockUnlock(State.lock);
        return result;
    }

    static int waitLegacy(uint64_t sequence)
    {
        if (!sequence)
            return 0;
        IOLockLock(State.lock);
        SwitchHost1xResult *found = nullptr;
        for (auto *record = State.buckets[sequence % kBuckets]; record; record = record->next)
            if (record->last == sequence && (!found || found->generation < record->generation))
                found = record;
        if (!found)
            for (auto *bucket : State.buckets)
                for (auto *record = bucket; record; record = record->next)
                    if (record->first <= sequence && sequence <= record->last &&
                        (!found || found->generation < record->generation))
                        found = record;
        if (!found) {
            const int result = retired(sequence, nullptr) ? retiredResult(sequence, nullptr) : SWITCH_GPU_BACKING_STALE;
            IOLockUnlock(State.lock);
            return result == 1 ? 0 : result;
        }
        if (found->references == UINT32_MAX) {
            IOLockUnlock(State.lock);
            return -75;
        }
        ++found->references;
        while (!found->result)
            IOLockSleep(State.lock, found, THREAD_UNINT);
        const int result = found->result;
        drop(found);
        IOLockUnlock(State.lock);
        return result == 1 ? 0 : result;
    }

    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        SwitchHost1xResult *value = nullptr;

        ~Reservation()
        {
            cancel(&value);
        }

        int renew()
        {
            cancel(&value);
            value = allocate();
            return value ? 0 : -12;
        }

        SwitchHost1xResult *take()
        {
            auto *result = value;
            value = nullptr;
            return result;
        }
    };
};
