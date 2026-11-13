// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_transfer.h"
#include "raw_kit.h"
#include "raw_fence.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace raw {

void TextureTransfers::reset(Kernel &kernel)
{
    if (channel_)
        kernel.closeChannel(channel_);
    channel_ = last_ = next_ = 0;
    fence_ = nullptr;
    for (auto &slot : slots_) {
        slot.pending = false;
        slot.owner.reset();
    }
}

bool TextureTransfers::retire(Kernel &kernel, Slot &slot)
{
    if (!slot.pending)
        return true;
    const auto *fence = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_);
    bool wait = int32_t(*fence_ - slot.sequence) < 0;
    if (!wait) {
        const auto *semantic = reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(fence + 1);
        const auto status = readSemanticFence(semantic, slot.sequence).status;
        if (status == SemanticFenceStatus::Failed)
            return false;
        wait = status == SemanticFenceStatus::Pending;
    }
    if (wait && !kernel.wait(channel_, slot.sequence))
        return false;
    if (kernel.killRecords()) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (fence->error) {
            std::atomic_thread_fence(std::memory_order_acquire);
            if (int32_t(slot.sequence - fence->killed_first) >= 0 && int32_t(fence->killed_last - slot.sequence) >= 0)
                return false;
        }
    } else if (kernel.lost(channel_)) {
        return false;
    }
    slot.pending = false;
    slot.owner.reset();
    return true;
}

bool TextureTransfers::copy(Kernel &kernel, ResourceSet &set, raw::Mutex &allocationLock, const TextureTransfer &region,
                            void *bytes, uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner)
{
    if (!kernel.exactJobs() || !bytes || !region.width || !region.height || !region.planes || !region.bpp ||
        region.bpp > 16 || (region.bpp & (region.bpp - 1)))
        return false;
    const uint64_t line = uint64_t(region.width) * region.bpp;
    if (!row && region.height == 1)
        row = line;
    if (row < line || row > std::numeric_limits<size_t>::max() / region.height)
        return false;
    if (!image)
        image = row * region.height;
    if (image < row * region.height || image > std::numeric_limits<size_t>::max() / region.planes)
        return false;
    const uint64_t pitch = (line + 31) & ~uint64_t(31);
    if (pitch > UINT32_MAX || pitch > UINT64_MAX / region.height)
        return false;
    const uint64_t plane = pitch * region.height;
    if (plane > UINT64_MAX / region.planes)
        return false;
    const uint64_t dataBytes = plane * region.planes;
    const uint64_t commandOffset = (dataBytes + 255) & ~uint64_t(255);
    const uint64_t commandBytes = uint64_t(region.planes) * 32 * sizeof(uint32_t);
    if (commandOffset < dataBytes || commandOffset > UINT64_MAX - commandBytes || commandBytes / 4 >= (1u << 21))
        return false;
    std::lock_guard<raw::Mutex> serial(lock_);
    Slot *opened = open(kernel, allocationLock, commandOffset + commandBytes);
    if (!opened)
        return false;
    Slot &slot = *opened;
    if (write)
        for (uint32_t z = 0; z < region.planes; ++z)
            for (uint32_t y = 0; y < region.height; ++y)
                std::memcpy(slot.memory.cpu + z * plane + y * pitch,
                            static_cast<const uint8_t *>(bytes) + z * image + y * row, line);
    auto *start = reinterpret_cast<uint32_t *>(slot.memory.cpu + commandOffset);
    Push push{start, start + commandBytes / sizeof(uint32_t)};
    SurfaceList uses;
    uses.add(slot.memory.handle, !write);
    set.view(view_);
    const uint32_t removals = view_.removals;
    recorded_.clear();
    for (uint32_t z = 0; z < region.planes; ++z) {
        CopySurface texture = region.texture;
        if (region.layerBytes)
            texture.va += z * region.layerBytes;
        else
            texture.layer += z;
        const CopySurface staging{slot.memory.va + z * plane, uint32_t(pitch)};
        name(kernel, set, uses, texture.va, 1, write, 0);
        emitCopy(push, write ? staging : texture, write ? texture : staging, region.width, region.height,
                 copyTexelRemap(region.bpp), true);
    }
    KernelJob job;
    job.visibility = write ? JobVisibility::Internal : JobVisibility::External;
    if (!submit(kernel, set, slot, commandOffset, start, push.at, uses, removals, job, std::move(owner)))
        return false;
    if (write)
        return true;
    if (!retire(kernel, slot))
        return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    for (uint32_t z = 0; z < region.planes; ++z)
        for (uint32_t y = 0; y < region.height; ++y)
            std::memcpy(static_cast<uint8_t *>(bytes) + z * image + y * row, slot.memory.cpu + z * plane + y * pitch,
                        line);
    return true;
}

bool TextureTransfers::copyBytes(Kernel &kernel, ResourceSet &set, raw::Mutex &allocationLock, uint64_t source,
                                 uint64_t destination, uint64_t bytes, std::shared_ptr<void> owner)
{
    if (!kernel.exactJobs() || !source || !destination || !bytes || bytes > UINT32_MAX)
        return false;
    std::lock_guard<raw::Mutex> serial(lock_);
    Slot *slot = open(kernel, allocationLock, kCopyBytesWords * sizeof(uint32_t));
    if (!slot)
        return false;
    auto *start = reinterpret_cast<uint32_t *>(slot->memory.cpu);
    Push push{start, start + kCopyBytesWords};
    emitCopyBytes(push, source, destination, bytes);
    SurfaceList uses;
    uses.add(slot->memory.handle, false);
    set.view(view_);
    const uint32_t removals = view_.removals;
    recorded_.clear();
    KernelJob job;
    job.visibility = JobVisibility::Internal;
    job.hostWritten =
        name(kernel, set, uses, source, bytes, false, TegraGM20BSurfaceRange | TegraGM20BSurfaceHostWritten);
    name(kernel, set, uses, destination, bytes, true, TegraGM20BSurfaceRange);
    return submit(kernel, set, *slot, 0, start, push.at, uses, removals, job, std::move(owner));
}

// lock_ held: the next slot, its last copy retired, with `bytes` of memory, on a live channel; null: none
TextureTransfers::Slot *TextureTransfers::open(Kernel &kernel, raw::Mutex &allocationLock, uint64_t bytes)
{
    const bool killed =
        channel_ && kernel.killRecords() && reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_)->error;
    if (channel_ && (killed || kernel.lost(channel_) || !retire(kernel, slots_[next_])))
        reset(kernel);
    if (!channel_) {
        std::lock_guard<raw::Mutex> allocate(allocationLock);
        if (!kernel.channel(TegraGM20BEngineCopy, channel_, fence_))
            return nullptr;
    }
    Slot &slot = slots_[next_];
    if (slot.memory.bytes < bytes) {
        std::lock_guard<raw::Mutex> allocate(allocationLock);
        Bo replacement;
        if (!kernel.allocControl(std::max<uint64_t>(0x10000, bytes), replacement, ControlUse::TransferSlot))
            return nullptr;
        kernel.free(slot.memory);
        slot.memory = replacement;
    }
    return &slot;
}

// lock_ held, view_ current: [va, va + bytes) named to the job. A set member goes with the set (recorded_), so the device
// channel's latest set job and this one follow each other; anything else is a use of its own with `flags`. True: a member.
bool TextureTransfers::name(Kernel &kernel, ResourceSet &set, SurfaceList &uses, uint64_t va, uint64_t bytes,
                            bool write, uint32_t flags)
{
    if (const SetMember *m = view_.find(va, bytes)) {
        if (write && !(m->access & TegraGM20BSurfaceWrite))
            set.promote(kernel, m->handle);
        recorded_.push_back({m->handle, write ? uint32_t(TegraGM20BSurfaceWrite) : uint32_t(TegraGM20BSurfaceRead)});
        return true;
    }
    uses.addAddress(va, bytes, write, flags);
    return false;
}

// lock_ held: the slot's commands [start, end), `offset` bytes into its memory, run as `job` with `uses` and the members
// recorded since the view of `removals`; the slot pending until they complete
bool TextureTransfers::submit(Kernel &kernel, ResourceSet &set, Slot &slot, uint64_t offset, const uint32_t *start,
                              const uint32_t *end, SurfaceList &uses, uint32_t removals, KernelJob job,
                              std::shared_ptr<void> owner)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    const TegraGM20BAbiPush commands{slot.memory.va + offset, uint32_t(end - start), 0};
    job.channel = channel_;
    job.pushes = &commands;
    job.pushCount = 1;
    uint32_t sequence = 0;
    if (!submitInSet(kernel, set, view_, !recorded_.empty(), job, sequence, nullptr, &uses, &recorded_, removals))
        return false;
    slot.sequence = sequence;
    slot.pending = true;
    slot.owner = std::move(owner);
    last_ = sequence;
    next_ = (next_ + 1) % slots_.size();
    return true;
}

void TextureTransfers::close(Kernel &kernel, raw::Mutex &allocationLock)
{
    std::lock_guard<raw::Mutex> serial(lock_);
    reset(kernel);
    for (auto &slot : slots_) {
        std::lock_guard<raw::Mutex> allocate(allocationLock);
        kernel.free(slot.memory);
    }
}

}  // namespace raw
