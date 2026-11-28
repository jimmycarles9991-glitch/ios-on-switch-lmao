// SPDX-License-Identifier: MIT
#pragma once
#include "lock.h"
#include "channel_recovery.h"
#include "kernel.h"
#include "resource_set.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace gm20b {

// One side of a copy-engine copy: pitch (va its first texel's, rows `pitch` apart) or a block-linear image (va its first
// byte, `pitch` its row stride, from texel (x, y) of plane `layer` of its depth)
struct CopySurface {
    uint64_t va = 0;
    uint32_t pitch = 0, x = 0, y = 0, height = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;
    uint32_t depth = 1, layer = 0;
    uint8_t blockDepth = 0;
};

enum class TextureTransferResult : uint8_t {
    Unsupported,
    Complete,
    Failed
};

struct TextureTransfer {
    CopySurface texture;
    uint32_t width = 0, height = 0, planes = 0, bpp = 0;  // bpp: a texel's bytes in the caller's rows
    uint64_t layerBytes = 0;
    // SET_REMAP_COMPONENTS into the texture and out of it where a texel there is not the caller's (zeta_cpu.h); 0:
    // the texel copy of bpp
    uint32_t remapIn = 0, remapOut = 0;
    // A multisampled texture's: `width` x `height` pixels of `samples` samples, its image their cells of the sample
    // grid (fx::msLayout, 1 << sxLog2 x 1 << syLog2 a pixel, `texture` the grid's), the caller's rows each pixel's
    // samples in turn (Metal's getBytes and replaceRegion)
    uint8_t samples = 1, sxLog2 = 0, syLog2 = 0;
};

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// HW Test's failed CPU copy: while `armed`, the calling thread's next TextureTransfers::copy fails as the kernel's
// refusal of its submission does (nothing moved, nothing published). Whether the thread's arm before this one fired.
bool failNextTransfer(bool armed);
#endif

class TextureTransfers {
public:
    // A job of the copy channel; it names `set` only when the texture is one of its members, so that job and the device
    // channel's set jobs follow each other
    bool copy(Kernel &kernel, ResourceSet &set, gm20b::Mutex &allocationLock, const TextureTransfer &region,
              void *bytes, uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner);
    // `bytes` bytes from GPU address `source`, CPU-written memory whose lines are cleaned, to `destination`, with no CPU
    // wait; `owner` held until the copy completed. Later work on either range waits for it: by the set where it lies in a
    // member, else by the kernel's write record. `destination` is a pool image's kind-0 view (Device::copyCallerPages),
    // whose texels are read through 0xfe next: the copy ends with a kind switch.
    bool copyBytes(Kernel &kernel, ResourceSet &set, gm20b::Mutex &allocationLock, uint64_t source,
                   uint64_t destination, uint64_t bytes, std::shared_ptr<void> owner);
    void close(Kernel &kernel, gm20b::Mutex &allocationLock);

private:
    struct Slot {
        Bo memory;
        uint32_t sequence = 0;
        bool pending = false;
        std::shared_ptr<void> owner;
        // Held by one copy's thread from open() until it is done with the slot's memory, lock_ held or not, so no other
        // copy takes the slot meanwhile: set under lock_, given back with one release store
        std::atomic<bool> claimed{false};
    };

    struct Unclaim {
        void operator()(Slot *slot) const
        {
            slot->claimed.store(false, std::memory_order_release);
        }
    };

    using Claim = std::unique_ptr<Slot, Unclaim>;

    bool retire(Kernel &kernel, std::unique_lock<gm20b::UnfairMutex> &serial, Slot &slot);
    void reset(Kernel &kernel);
    Claim claim();
    Claim open(Kernel &kernel, std::unique_lock<gm20b::UnfairMutex> &serial, gm20b::Mutex &allocationLock,
               uint64_t bytes);
    bool name(Kernel &kernel, ResourceSet &set, SurfaceList &uses, uint64_t va, uint64_t bytes, bool write,
              uint32_t flags);
    bool submit(Kernel &kernel, ResourceSet &set, Slot &slot, uint64_t offset, const uint32_t *start,
                const uint32_t *end, SurfaceList &uses, uint32_t removals, KernelJob job, std::shared_ptr<void> owner);
    gm20b::UnfairMutex lock_;
    std::deque<Slot> slots_ = std::deque<Slot>(4);  // grows, never shrinks: a claimed slot stays where it is
    SetView view_;
    std::vector<SetUse> recorded_;
    uint32_t channel_ = 0, next_ = 0;
    // channels reset() closed: a copy that waited without lock_ checks no other thread closed its one meanwhile
    uint32_t resets_ = 0;
    const volatile uint32_t *fence_ = nullptr;
    ChannelRefusal refusal_;
};

}  // namespace gm20b
