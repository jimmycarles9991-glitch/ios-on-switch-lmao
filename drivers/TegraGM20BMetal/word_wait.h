// SPDX-License-Identifier: MIT
#pragma once
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <cstdint>

// The kernel's compare-and-wait on a 32-bit word's address and its wake (xnu sys/ulock.h), as libc++'s atomic wait and
// os_unfair_lock sleep: a thread waits for a word to change with no lock taken by either side
extern "C" int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern "C" int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

namespace gm20b {

// A change others wait for with no lock: the changer bumps the word after its change and wakes only where one waits, a
// waiter reads the word before it checks and sleeps only while the word still reads that (each stores before it loads
// the other's: seq_cst), so no change is slept through.
class ChangeWord {
public:
    void changed()
    {
        word_.fetch_add(1);
        if (waiters_.load())
            __ulock_wake(kCompareAndWait | kWakeAll | kNoErrno, &word_, 0);
    }

    template <typename Done> void waitOnce(Done &&done)
    {
        waiters_.fetch_add(1);
        const uint32_t seen = word_.load();
        if (!done())
            __ulock_wait(kCompareAndWait | kNoErrno, &word_, seen, 0);
        waiters_.fetch_sub(1);
    }

    template <typename Done> void waitFor(Done &&done)
    {
        waiters_.fetch_add(1);
        for (;;) {
            const uint32_t seen = word_.load();
            if (done())
                break;
            __ulock_wait(kCompareAndWait | kNoErrno, &word_, seen, 0);
        }
        waiters_.fetch_sub(1);
    }

private:
    static constexpr uint32_t kCompareAndWait = 1, kWakeAll = 0x100, kNoErrno = 0x1000000;
    std::atomic<uint32_t> word_{0}, waiters_{0};
};

// Owner-inheriting wait word (UL_UNFAIR_LOCK, separate address from ChangeWord since sys_ulock
// rejects mixed opcodes on one address with EDOM). While `owner_` holds a live mach thread port
// name, sleepers donate base_pri to that thread through XNU's TURNSTILE_ULOCK turnstile; any
// thread clears or replaces the owner and wakes all waiters via ULF_WAKE_ALLOW_NON_OWNER.
class OwnedWord {
public:
    void setOwner(uint32_t owner)
    {
        const uint32_t prev = owner_.exchange(owner);
        if (prev != owner && waiters_.load())
            __ulock_wake(kUnfairLock | kWakeAll | kWakeAllowNonOwner | kNoErrno, &owner_, 0);
    }

    void clear()
    {
        setOwner(0);
    }

    bool hasOwner() const
    {
        return owner_.load(std::memory_order_acquire) != 0;
    }

    uint32_t owner() const
    {
        return owner_.load(std::memory_order_acquire);
    }

    template <typename Done> bool waitOnce(Done &&done)
    {
        const uint32_t self = currentOwner();
        waiters_.fetch_add(1);
        const uint32_t seen = owner_.load();
        bool waited = false;
        if (!done() && seen != 0 && seen != self) {
            const int r = __ulock_wait(kUnfairLock | kNoErrno, &owner_, seen, 0);
            if (r >= 0 || r == -EINTR) {
                waited = true;
            } else if (r == -EFAULT) {
                (void)owner_.load(std::memory_order_relaxed);
                waited = true;
            }
        }
        waiters_.fetch_sub(1);
        return waited;
    }

    template <typename Done> void waitWhileOwned(Done &&done)
    {
        while (!done()) {
            if (!waitOnce(done))
                break;
        }
    }

    static uint32_t currentOwner()
    {
        return uint32_t(pthread_mach_thread_np(pthread_self()));
    }

private:
    static constexpr uint32_t kUnfairLock = 2, kWakeAll = 0x100, kWakeAllowNonOwner = 0x400, kNoErrno = 0x1000000;
    std::atomic<uint32_t> owner_{0}, waiters_{0};
};

}  // namespace gm20b
