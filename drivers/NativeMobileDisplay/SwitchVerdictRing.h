// SPDX-License-Identifier: BSD-2-Clause-Patent
// Records any number of threads publish and one consumer takes in publication order (a bounded queue in D. Vyukov's
// shape). No thread owns the ring, so a publisher claims the next ticket with one compare-and-swap, and only once its
// slot is free; the slot's sequence (its ticket + 1 once written, ticket + kSlots once taken) hands it between them.
// A publisher that finds the ring full drops its record and says so; nothing is overwritten. C++, header only, for the
// driver and its test.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace switch_display {

template <class Record, unsigned kSlots> class VerdictRing {
    static_assert(__is_trivially_copyable(Record), "records are copied in and out as bytes");

public:
    // Any thread. False: the ring was full and the record is dropped.
    bool publish(const Record &record)
    {
        uint64_t ticket = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        for (;;) {
            const uint64_t seq = sequence(ticket);
            if (seq == ticket) {
                if (__atomic_compare_exchange_n(&tail, &ticket, ticket + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    break;
            } else if (seq < ticket) {
                return false;
            } else {
                ticket = __atomic_load_n(&tail, __ATOMIC_RELAXED);
            }
        }
        Slot &slot = slots[ticket % kSlots];
        memcpy(&slot.record, &record, sizeof(record));
        setSequence(ticket, ticket + 1);
        return true;
    }

    // The one consumer: every record published so far, in ticket order, to take(record).
    template <class Take> void drain(Take take)
    {
        while (sequence(head) == head + 1) {
            Record record;
            memcpy(&record, &slots[head % kSlots].record, sizeof(record));
            setSequence(head, head + kSlots);
            ++head;
            take(record);
        }
    }

private:
    // Kept as the sequence less the slot's index, so a zeroed ring has every slot free for its first ticket.
    struct Slot {
        uint64_t base;
        Record record;
    };

    uint64_t sequence(uint64_t ticket) const
    {
        return __atomic_load_n(&slots[ticket % kSlots].base, __ATOMIC_ACQUIRE) + ticket % kSlots;
    }

    void setSequence(uint64_t ticket, uint64_t seq)
    {
        __atomic_store_n(&slots[ticket % kSlots].base, seq - ticket % kSlots, __ATOMIC_RELEASE);
    }

    Slot slots[kSlots] = {};
    uint64_t tail = 0;  // the publishers'
    uint64_t head = 0;  // the consumer's
};

}  // namespace switch_display
