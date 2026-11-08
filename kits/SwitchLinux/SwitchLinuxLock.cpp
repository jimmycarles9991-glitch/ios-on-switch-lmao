// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchLinuxLock.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace {
constexpr uint32_t kSpinsPerClockLook = 256;

// A wait shorter than kSpinsPerClockLook spins never reads the clock.
class Patience {
public:
    // False once the lock has been waited on for longer than the spinlock timeout.
    bool wait()
    {
        __asm__ volatile("yield" ::: "memory");
        if (++spins_ % kSpinsPerClockLook)
            return true;
        const uint64_t now = mach_absolute_time();
        if (!deadline_) {
            uint64_t timeout = 0;
            nanoseconds_to_absolutetime(SWITCH_LINUX_SPIN_TIMEOUT_NS, &timeout);
            deadline_ = now + timeout;
        }
        return now < deadline_;
    }

private:
    uint32_t spins_ = 0;
    uint64_t deadline_ = 0;
};
}  // namespace

extern "C" void switch_linux_spin_contended(switch_linux_spinlock *lock, uintptr_t self)
{
    Patience patience;
    for (;;) {
        uintptr_t holder = __atomic_load_n(&lock->holder, __ATOMIC_RELAXED);
        if (!holder) {
            if (__atomic_compare_exchange_n(&lock->holder, &holder, self, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
            continue;
        }
        if (holder == self) {
            panic("SwitchLinux: spinlock %p taken again by its holder, thread %#lx", lock, holder);
        }
        if (!patience.wait()) {
            panic("SwitchLinux: spinlock %p held for over %llu ms by thread %#lx", lock,
                  SWITCH_LINUX_SPIN_TIMEOUT_NS / 1000000ULL, holder);
        }
    }
}

extern "C" void switch_linux_read_contended(switch_linux_rwlock *lock)
{
    Patience patience;
    for (;;) {
        uint32_t seen = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
        if (!(seen & SWITCH_LINUX_RW_WRITER)) {
            if (__atomic_compare_exchange_n(&lock->state, &seen, seen + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
            continue;
        }
        if (!patience.wait()) {
            panic("SwitchLinux: rwlock %p write-held for over %llu ms", lock,
                  SWITCH_LINUX_SPIN_TIMEOUT_NS / 1000000ULL);
        }
    }
}

extern "C" void switch_linux_write_contended(switch_linux_rwlock *lock)
{
    Patience patience;
    for (;;) {
        uint32_t idle = 0;
        if (__atomic_compare_exchange_n(&lock->state, &idle, SWITCH_LINUX_RW_WRITER, true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        if (!patience.wait()) {
            panic("SwitchLinux: rwlock %p busy for over %llu ms (state %#x)", lock,
                  SWITCH_LINUX_SPIN_TIMEOUT_NS / 1000000ULL, idle);
        }
    }
}
