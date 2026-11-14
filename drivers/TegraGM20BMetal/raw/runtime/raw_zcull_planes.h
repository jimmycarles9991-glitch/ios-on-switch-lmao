// SPDX-License-Identifier: MIT
// Zcull for a loaded depth, as NVK's zcull plane carries it (nvk_image.c's plane, nvk_CmdBeginRendering's LOAD_ZCULL,
// nvk_CmdEndRendering's STORE_ZCULL), within one command buffer: a pass with a region that stores a depth a later pass
// loads stores the region (STORE_ZCULL) into the command buffer's upload ring, and a later pass of the same
// command buffer that loads that depth over the same region loads it (LOAD_ZCULL) where it would otherwise take none.
// Any write the command buffer makes to the depth's memory in between leaves the stored region stale, and a pass that
// invalidated its region stores none. A private depth texture is written by nothing outside the command buffer while it
// runs, and no stored region outlives the command buffer, so none is ever loaded from garbage (which kills the context:
// NVK).
#pragma once
#include <cstdint>
#include <vector>
#include "raw_zcull.h"

namespace raw {

constexpr uint32_t kZcullStorageAlign = 0x400;  // nil_zcull_new's align_B
constexpr uint32_t kLoadZcull = 0x1500, kStoreZcull = 0x1464;

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
    uint64_t va = 0, bytes = 0;              // the region's storage
    ZcullRegion region;
    uint8_t dir = kZcullLess;  // the direction the region was built in
    bool valid = false;
};

// A command buffer's stored regions, by depth
class ZcullPlanes {
public:
    void clear()
    {
        stored_.clear();
    }

    // The depth at `zetaVa`'s stored region, if nothing wrote the depth since; zetaVa 0 where none is
    ZcullStored held(uint64_t zetaVa) const
    {
        for (const ZcullStored &s : stored_)
            if (s.zetaVa == zetaVa && s.valid)
                return s;
        return {};
    }

    // A pass stored the region `s` describes: the depth's earlier storage gives way to it. A depth whose memory is not
    // known (no bytes) keeps none, as no write to it could be seen
    void stored(const ZcullStored &s)
    {
        if (!s.memoryBytes)
            return;
        for (ZcullStored &x : stored_)
            if (x.zetaVa == s.zetaVa) {
                x = s;
                return;
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

private:
    std::vector<ZcullStored> stored_;
};

// A pass's zcull at its begin: `ask` (but its loadedRegion) and the region over x, y, width, height and `layers`. It
// loads `held`, its depth's stored region, where that is this region and it does not clear the depth, and stores its
// region at its end where `keep` (it stores a one-layer depth a later pass loads), into the storage it loaded or
// into `storage(bytes)`'s (0: none to have)
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
    }
    if (!keep || layers != 1)
        return;
    if (!z.storage) {
        z.storageBytes = zcullStorageBytes(dev, region);
        z.storage = storage(z.storageBytes);
    }
    z.store = z.storage != 0;
}

// What a pass that stored its region leaves for later passes: the direction its region was built in (no depth test:
// SET_ZCULL_DIR_FORMAT's LESS)
inline ZcullStored zcullStoredBy(const ZcullPass &z, uint64_t zetaVa, uint64_t memoryVa, uint64_t memoryBytes)
{
    ZcullStored s;
    s.zetaVa = zetaVa;
    s.memoryVa = memoryVa;
    s.memoryBytes = memoryBytes;
    s.va = z.storage;
    s.bytes = z.storageBytes;
    s.region = z.region;
    s.dir = z.dir == kZcullGreater ? kZcullGreater : kZcullLess;
    s.valid = true;
    return s;
}

}  // namespace raw
