// SPDX-License-Identifier: MIT
// Zcull for a loaded depth, as NVK's zcull plane carries it (nvk_image.c's plane, nvk_CmdBeginRendering's LOAD_ZCULL,
// nvk_CmdEndRendering's STORE_ZCULL), within one Metal command buffer: a pass with a region that stores its depth stores
// the region (STORE_ZCULL) into a slot (zcull_slots.h), and a later pass of the same command buffer that loads that depth
// over the same region loads it (LOAD_ZCULL) where it would otherwise take none. Any write the command buffer makes to
// the depth's memory in between leaves the stored region stale, and a pass that invalidated its region stores none.
//
// A pass encoded ahead of its commit cannot know whether a later pass loads its depth, so its STORE_ZCULL word is written
// as a NO_OPERATION and made a store once a later pass of its job loads the region, or before the job is submitted when a
// pass of the command buffer's next range will (settle). A region nothing loads costs one word, and no slot past the job.
//
// A region outlives its job only into the next range of its command buffer (ZcullCarry), and only while no other job
// was submitted on the channel in between. A private depth texture is written by nothing outside the command buffer while
// it runs, so no region is ever loaded over a depth something else wrote, and none is loaded from a slot its store did
// not fill (which kills the context: NVK).
#pragma once
#include <cstdint>
#include <vector>
#include "push.h"
#include "stats.h"
#include "zcull.h"

namespace gm20b {

constexpr uint32_t kZcullStorageAlign = 0x400;                                        // nil_zcull_new's align_B
constexpr uint32_t kLoadZcull = 0x1500, kStoreZcull = 0x1464, kNoOperation = 0x0100;  // NVB197_NO_OPERATION
constexpr uint32_t kZcullStoreWord = Push::hdr(4, SUBC_3D, kStoreZcull, 0);
constexpr uint32_t kZcullUnstoredWord = Push::hdr(4, SUBC_3D, kNoOperation, 0);

// SET_ZCULL_STORAGE's bytes for a region (nil_zcull_new's size_B)
inline uint64_t zcullStorageBytes(const TegraGM20BAbiZcullInfo &dev, const ZcullRegion &r)
{
    return uint64_t(r.aliquotCount) * r.layers * dev.region_byte_multiplier + dev.region_header_size +
           dev.subregion_header_size;
}

inline bool sameZcullRegion(const ZcullRegion &a, const ZcullRegion &b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.layers == b.layers &&
           a.aliquotCount == b.aliquotCount;
}

struct ZcullStored {
    uint64_t zetaVa = 0;                     // the depth texture's address, which passes find it by
    uint64_t memoryVa = 0, memoryBytes = 0;  // the depth's memory, which writes to it are matched against
    uint64_t va = 0, bytes = 0;              // the region's storage, in slot `slot`
    ZcullRegion region;
    uint32_t *store = nullptr;  // its pass's STORE_ZCULL word while still a NO_OPERATION, in the job's unsubmitted push
    int slot = -1;
    uint8_t dir = kZcullLess;  // the direction the region was built in
    bool valid = false;
    bool carried = false;  // stored by an earlier job of the command buffer
};

// A job's stored regions, by depth: no more than one a slot
class ZcullPlanes {
public:
    void clear()
    {
        stored_.clear();
        stores_ = unstored_ = 0;
    }

    bool empty() const
    {
        return stored_.empty();
    }

    const std::vector<ZcullStored> &regions() const
    {
        return stored_;
    }

    // The depth at `zetaVa`'s stored region, if nothing wrote the depth since; zetaVa 0 where none is
    ZcullStored held(uint64_t zetaVa) const
    {
        for (const ZcullStored &s : stored_)
            if (s.zetaVa == zetaVa && s.valid)
                return s;
        return {};
    }

    bool holdsSlot(int slot) const
    {
        for (const ZcullStored &s : stored_)
            if (s.slot == slot)
                return true;
        return false;
    }

    // The slot the depth at `zetaVa` stored its region in, -1: none. Its next store takes it again, after every load of
    // the region in the channel's order.
    int slotOf(uint64_t zetaVa) const
    {
        for (const ZcullStored &s : stored_)
            if (s.zetaVa == zetaVa)
                return s.slot;
        return -1;
    }

    // A pass stored the region `s` describes: the depth's earlier region gives way to it. A depth whose memory is not
    // known (no bytes) keeps none, as no write to it could be seen
    void stored(const ZcullStored &s)
    {
        if (!s.memoryBytes)
            return;
        for (auto it = stored_.begin(); it != stored_.end(); ++it)
            if (it->zetaVa == s.zetaVa) {
                SWITCH_METAL_DIAGNOSTIC(unstored_ += it->store != nullptr);
                stored_.erase(it);
                break;
            }
        stored_.push_back(s);
    }

    // [va, va + bytes) written: a region stored over any of it is stale
    void wrote(uint64_t va, uint64_t bytes)
    {
        for (ZcullStored &s : stored_)
            if (va < s.memoryVa + s.memoryBytes && s.memoryVa < va + bytes)
                s.valid = false;
    }

    // A pass loads the region of the depth at `zetaVa` (held() gave it before the pass's own write): the pass that stored
    // it stores it. Whether an earlier job stored it
    bool loaded(uint64_t zetaVa)
    {
        for (ZcullStored &s : stored_)
            if (s.zetaVa == zetaVa) {
                decide(s);
                return s.carried;
            }
        return false;
    }

    // A slot no later pass loads from, its region dropped: a stale region's, else the oldest region no pass loaded
    // yet; -1: none
    int reclaim()
    {
        auto pick = stored_.end();
        for (auto it = stored_.begin(); it != stored_.end(); ++it)
            if (!it->valid) {
                pick = it;
                break;
            } else if (it->store && pick == stored_.end()) {
                pick = it;
            }
        if (pick == stored_.end())
            return -1;
        const int slot = pick->slot;
        SWITCH_METAL_DIAGNOSTIC(unstored_ += pick->store != nullptr);
        stored_.erase(pick);
        return slot;
    }

    // Before the job is submitted: the regions bit i of `kept` names (by regions()' order) are stored for the next job
    // of the command buffer, the rest dropped, `released(slot)` told of each dropped one's slot
    template <class Released> void settle(uint32_t kept, Released released)
    {
        uint32_t i = 0;
        for (auto it = stored_.begin(); it != stored_.end(); ++i) {
            if (it->valid && (kept >> i & 1)) {
                decide(*it);
                it->carried = true;
                ++it;
                continue;
            }
            SWITCH_METAL_DIAGNOSTIC(unstored_ += it->store != nullptr);
            released(it->slot);
            it = stored_.erase(it);
        }
    }

    // STORE_ZCULL words made stores, and left NO_OPERATIONs, since clear()
    uint32_t stores() const
    {
        return stores_;
    }

    uint32_t unstored() const
    {
        return unstored_;
    }

    // A copy of `from`'s regions, which keeps them; its counts stay its own
    void copyFrom(const ZcullPlanes &from)
    {
        stored_ = from.stored_;
        stores_ = unstored_ = 0;
    }

    // `from`'s regions, which leaves none; its counts stay its own
    void carryFrom(ZcullPlanes &from)
    {
        stored_.clear();
        stored_.swap(from.stored_);
        stores_ = unstored_ = 0;
    }

private:
    void decide(ZcullStored &s)
    {
        if (!s.store)
            return;
        *s.store = kZcullStoreWord;
        s.store = nullptr;
        SWITCH_METAL_DIAGNOSTIC(stores_++);
    }

    std::vector<ZcullStored> stored_;
    uint32_t stores_ = 0, unstored_ = 0;
};

// A job's regions parked at its submission for the next range of its Metal command buffer `to`, which begins at `from`:
// taken by that range's job when no other job was submitted on the channel since (`submitted`, the channel's last
// submission when parked, on channel `generation`)
struct ZcullCarry {
    ZcullPlanes planes;
    const void *to = nullptr;
    uint64_t from = 0, owner = 0, generation = 0;
    uint32_t submitted = 0;

    bool live(uint64_t channelGeneration, uint32_t lastSubmitted) const
    {
        return to && generation == channelGeneration && submitted == lastSubmitted;
    }
};

// A pass's zcull at its begin: `ask` (but its loadedRegion) and the region over x, y, width, height and `layers`. It
// loads `held`, its depth's stored region, where that is this region and it does not clear the depth, and stores its
// region at its end where `keep` (it stores a one-layer depth), into the storage it loaded or into `storage(bytes)`'s
// (0: none to have)
template <typename Storage>
inline void zcullBegin(ZcullPass &z, ZcullAsk ask, const TegraGM20BAbiZcullInfo &dev, uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height, uint32_t layers, const ZcullStored &held, bool keep,
                       Storage storage)
{
    z = ZcullPass();
    ZcullRegion region;
    const bool laid = zcullRegion(dev, x, y, width, height, region, layers);
    ask.loadedRegion = laid && held.zetaVa && held.valid && sameZcullRegion(held.region, region);
    z.candidate = laid && zcullCandidate(ask);
    if (!z.candidate)
        return;
    z.region = region;
    if (!ask.cleared) {
        z.load = true;
        z.dir = held.dir;
        z.storage = held.va;
        z.storageBytes = held.bytes;
        z.slot = held.slot;
    }
    if (!keep || layers != 1)
        return;
    if (!z.storage) {
        z.storageBytes = zcullStorageBytes(dev, region);
        z.slot = -1;
        z.storage = storage(z.storageBytes, z.slot);
    }
    z.store = z.storage != 0;
}

// What a pass that stored its region leaves for later passes: the direction its region was built in (no depth test:
// SET_ZCULL_DIR_FORMAT's LESS), and its STORE_ZCULL word `store`, a NO_OPERATION
inline ZcullStored zcullStoredBy(const ZcullPass &z, uint64_t zetaVa, uint64_t memoryVa, uint64_t memoryBytes,
                                 uint32_t *store = nullptr)
{
    ZcullStored s;
    s.zetaVa = zetaVa;
    s.memoryVa = memoryVa;
    s.memoryBytes = memoryBytes;
    s.va = z.storage;
    s.bytes = z.storageBytes;
    s.region = z.region;
    s.store = store;
    s.slot = z.slot;
    s.dir = z.dir == kZcullGreater ? kZcullGreater : kZcullLess;
    s.valid = true;
    return s;
}

}  // namespace gm20b
