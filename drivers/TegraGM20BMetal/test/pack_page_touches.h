// SPDX-License-Identifier: MIT
// The CPU's first touch of each page of a range of the pack's mapping, from a watch's making to its end: the pages are
// protected, and the fault handler opens each one it is asked for and notes it. A page no process keeps resident read
// this way is a card read on the device.
#pragma once
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>

namespace pack_touches {

constexpr uint32_t kMaxNoted = 4096;

inline std::atomic<uintptr_t> gBegin{0}, gEnd{0};
inline std::atomic<uint32_t> gTouches{0};
inline uintptr_t gPage = 0;
inline uintptr_t gNoted[kMaxNoted];
inline struct sigaction gPreviousBus, gPreviousSegv;

inline void onFault(int sig, siginfo_t *info, void *)
{
    const uintptr_t at = uintptr_t(info->si_addr);
    if (at >= gBegin.load() && at < gEnd.load()) {
        const uintptr_t page = at & ~(gPage - 1);
        const uint32_t n = gTouches.fetch_add(1);
        if (n < kMaxNoted)
            gNoted[n] = page;
        mprotect(reinterpret_cast<void *>(page), gPage, PROT_READ);
        return;
    }
    sigaction(sig, sig == SIGBUS ? &gPreviousBus : &gPreviousSegv, nullptr);
}

class PageTouches {
public:
    PageTouches(const uint8_t *begin, const uint8_t *end)
        : from_((uintptr_t(begin) + uintptr_t(getpagesize()) - 1) & ~(uintptr_t(getpagesize()) - 1))
    {
        gPage = uintptr_t(getpagesize());
        const uintptr_t to = uintptr_t(end) & ~(gPage - 1);
        bytes_ = to > from_ ? to - from_ : 0;
        gTouches = 0;
        gBegin = from_;
        gEnd = from_ + bytes_;
        struct sigaction a = {};
        a.sa_sigaction = onFault;
        a.sa_flags = SA_SIGINFO;
        sigemptyset(&a.sa_mask);
        sigaction(SIGBUS, &a, &gPreviousBus);
        sigaction(SIGSEGV, &a, &gPreviousSegv);
        if (bytes_)
            mprotect(reinterpret_cast<void *>(from_), bytes_, PROT_NONE);
    }

    ~PageTouches()
    {
        if (bytes_)
            mprotect(reinterpret_cast<void *>(from_), bytes_, PROT_READ);
        gBegin = gEnd = 0;
        sigaction(SIGBUS, &gPreviousBus, nullptr);
        sigaction(SIGSEGV, &gPreviousSegv, nullptr);
    }

    uint64_t bytes() const
    {
        return bytes_;
    }

    uint32_t touches() const
    {
        return gTouches.load();
    }

    bool watched(uintptr_t at) const
    {
        return at >= from_ && at < from_ + bytes_;
    }

    // Whether the page holding `at` was touched since the watch began (a page outside the watch: yes)
    bool touched(uintptr_t at) const
    {
        if (!watched(at))
            return true;
        const uintptr_t page = at & ~(gPage - 1);
        const uint32_t n = gTouches.load();
        for (uint32_t i = 0; i < n && i < kMaxNoted; i++) {
            if (gNoted[i] == page)
                return true;
        }
        return false;
    }

private:
    uintptr_t from_ = 0;
    uint64_t bytes_ = 0;
};

}  // namespace pack_touches
