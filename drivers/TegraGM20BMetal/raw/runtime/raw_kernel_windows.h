// SPDX-License-Identifier: MIT
// The windows a command buffer's kernels reach buffers bound off 16 bytes through (raw_kernel_shadow.h
// KernelUse::windowSlot): one per buffer, address and length, in memory of the command buffer's own that lives until it
// completes. KernelShadows orders their fills and copy backs as it does a texture's image's: the window is its own
// object, so the same buffer bound in place, or at another offset, is another resource over the same bytes.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include "raw_kernel_shadow.h"

namespace raw {

class KernelWindows {
public:
    struct Window {
        const void *buffer;
        ByteSpan span;  // the buffer's bytes the window holds
        uint64_t va;    // the window, on 16
    };

    // Each window use of `uses` with its window as its object, made now from `transient(bytes)` where it has none yet
    template <typename Transient> void name(std::vector<KernelUse> &uses, Transient &&transient)
    {
        for (KernelUse &u : uses) {
            if (u.windowSlot < 0)
                continue;
            Window *w = nullptr;
            for (Window &x : windows_)
                if (x.buffer == u.object && x.span.va == u.span.va && x.span.bytes == u.span.bytes)
                    w = &x;
            if (!w)
                w = &windows_.emplace_back(Window{u.object, u.span, transient(u.span.bytes)});
            u.object = w;
        }
    }

    const Window *find(const void *object) const
    {
        for (const Window &w : windows_)
            if (&w == object)
                return &w;
        return nullptr;
    }

private:
    std::deque<Window> windows_;
};

}  // namespace raw
