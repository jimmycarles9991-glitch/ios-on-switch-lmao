// SPDX-License-Identifier: MIT
// The memory stored zcull regions live in (zcull_planes.h): a few slots, each the size of the largest one-layer region,
// made as they are first wanted and kept for the device's life. Only STORE_ZCULL and LOAD_ZCULL touch a slot, and every
// job runs on the device's one channel in submission order, so a later job may store into a slot without waiting: its
// store comes after every load an earlier job made from it, as NVK reuses an image's zcull plane every frame.
//
// A slot belongs to the job that last took it (its owner, CommandBuffer::zcullOwner_). Another job may take it unless
// it holds a region a parked carry still owes the next range of its Metal command buffer (`reserved`).
#pragma once
#include <cstdint>
#include "kernel.h"
#include "zcull_planes.h"

namespace gm20b {

class ZcullSlots {
public:
    static constexpr uint32_t kSlots = 4;

    // The largest one-layer region's storage (zcullStorageBytes over every aliquot)
    static uint64_t slotBytes(const TegraGM20BAbiZcullInfo &dev)
    {
        const uint64_t bytes = uint64_t(dev.aliquot_total) * dev.region_byte_multiplier + dev.region_header_size +
                               dev.subregion_header_size;
        return (bytes + kZcullStorageAlign - 1) & ~uint64_t(kZcullStorageAlign - 1);
    }

    // A slot `owner` holds none of and `reserved` (0: no owner) holds none of, made by `make(bo, bytes)` where every
    // made one is taken; -1: none
    template <class Make> int take(uint64_t owner, uint64_t reserved, uint64_t bytes, Make make)
    {
        for (uint32_t i = 0; i < made_; i++)
            if (owner_[i] != owner && (!reserved || owner_[i] != reserved)) {
                owner_[i] = owner;
                return int(i);
            }
        if (made_ == kSlots || !make(bo_[made_], bytes))
            return -1;
        owner_[made_] = owner;
        return int(made_++);
    }

    // Between jobs: a slot made ahead where the next job could take none (every made one a live carry's, `reserved`),
    // so it is not made mid-encode. The first is made where a depth first needs one: a process that draws no depth
    // keeps no slot.
    template <class Make> void makeAhead(uint64_t reserved, uint64_t bytes, Make make)
    {
        if (!made_)
            return;
        for (uint32_t i = 0; i < made_; i++)
            if (!reserved || owner_[i] != reserved)
                return;
        if (made_ < kSlots && make(bo_[made_], bytes))
            owner_[made_++] = 0;
    }

    void release(int slot)
    {
        owner_[slot] = 0;
    }

    // The slots `held`'s regions are in belong to `owner` again, and `owner` holds no other
    void keepFor(uint64_t owner, const ZcullPlanes &held)
    {
        for (uint32_t i = 0; i < made_; i++)
            if (held.holdsSlot(int(i)))
                owner_[i] = owner;
            else if (owner_[i] == owner)
                owner_[i] = 0;
    }

    uint64_t va(int slot) const
    {
        return bo_[slot].va;
    }

    uint64_t owner(int slot) const
    {
        return owner_[slot];
    }

    uint32_t made() const
    {
        return made_;
    }

    const Bo &bo(uint32_t slot) const
    {
        return bo_[slot];
    }

private:
    Bo bo_[kSlots];
    uint64_t owner_[kSlots] = {};
    uint32_t made_ = 0;
};

}  // namespace gm20b
