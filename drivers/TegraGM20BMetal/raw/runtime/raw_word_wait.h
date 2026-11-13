// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>

// The kernel's compare-and-wait on a 32-bit word's address and its wake (xnu sys/ulock.h), as libc++'s atomic wait and
// os_unfair_lock sleep: a thread waits for a word to change with no lock taken by either side
extern "C" int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern "C" int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

namespace raw {

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

}  // namespace raw
