// SPDX-License-Identifier: MIT
#pragma once
#include "raw_lock.h"
#include "raw_kernel.h"
#include "raw_resource_set.h"
#include <array>
#include <memory>
#include <mutex>

namespace raw {

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
    uint32_t width = 0, height = 0, planes = 0, bpp = 0;
    uint64_t layerBytes = 0;
};

class TextureTransfers {
public:
    // A job of the copy channel; it names `set` only when the texture is one of its members, so that job and the device
    // channel's set jobs follow each other
    bool copy(Kernel &kernel, ResourceSet &set, raw::Mutex &allocationLock, const TextureTransfer &region, void *bytes,
              uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner);
    // `bytes` bytes from GPU address `source`, CPU-written memory whose lines are cleaned, to `destination`, with no CPU
    // wait; `owner` held until the copy completed. Later work on either range waits for it: by the set where it lies in a
    // member, else by the kernel's write record.
    bool copyBytes(Kernel &kernel, ResourceSet &set, raw::Mutex &allocationLock, uint64_t source, uint64_t destination,
                   uint64_t bytes, std::shared_ptr<void> owner);
    void close(Kernel &kernel, raw::Mutex &allocationLock);

private:
    struct Slot {
        Bo memory;
        uint32_t sequence = 0;
        bool pending = false;
        std::shared_ptr<void> owner;
    };

    bool retire(Kernel &kernel, Slot &slot);
    void reset(Kernel &kernel);
    Slot *open(Kernel &kernel, raw::Mutex &allocationLock, uint64_t bytes);
    bool name(Kernel &kernel, ResourceSet &set, SurfaceList &uses, uint64_t va, uint64_t bytes, bool write,
              uint32_t flags);
    bool submit(Kernel &kernel, ResourceSet &set, Slot &slot, uint64_t offset, const uint32_t *start,
                const uint32_t *end, SurfaceList &uses, uint32_t removals, KernelJob job, std::shared_ptr<void> owner);
    raw::Mutex lock_;
    std::array<Slot, 4> slots_;
    SetView view_;
    std::vector<SetUse> recorded_;
    uint32_t channel_ = 0, last_ = 0, next_ = 0;
    const volatile uint32_t *fence_ = nullptr;
};

}  // namespace raw
