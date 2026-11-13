// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include <unordered_map>

// A planar or YCbCr texture's image is converted from its surface again once the surface's seed moves (encodeRaw), which a
// CPU write under IOSurfaceLock does. A write the seed does not show is counted here per surface, for the surfaces a converted
// texture is over: one through another texture over the surface (a video player's plane textures), on either driver, or a
// CPU write through Metal. The submit lock orders the counts as the work.
// Every texture over a surface holds its surface's count from its creation (RMTexture::_surfaceWrites), so a submitted
// range counts its written surfaces from its resources' facts, with no lock and no message.
struct SurfaceWriteCount {
    uint32_t holders = 0;  // gLock
    std::atomic<uint32_t> converted{0};
    std::atomic<uint64_t> writes{0};
};

static raw::Mutex gLock;
// IOSurfaceID -> its count, while a texture holds it
static std::unordered_map<uint32_t, SurfaceWriteCount *> gSurfaces;
static std::atomic<uint32_t> gConverted{0};

SurfaceWriteCount *holdSurfaceWrites(IOSurfaceRef s)
{
    std::lock_guard<raw::Mutex> l(gLock);
    SurfaceWriteCount *&count = gSurfaces[IOSurfaceGetID(s)];
    if (!count)
        count = new SurfaceWriteCount;
    count->holders++;
    return count;
}

void releaseSurfaceWrites(IOSurfaceRef s, SurfaceWriteCount *count)
{
    std::lock_guard<raw::Mutex> l(gLock);
    if (--count->holders)
        return;
    gSurfaces.erase(IOSurfaceGetID(s));
    delete count;
}

void surfaceConverted(SurfaceWriteCount *count, bool made)
{
    if (made) {
        count->converted.fetch_add(1, std::memory_order_relaxed);
        gConverted.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    count->converted.fetch_sub(1, std::memory_order_relaxed);
    gConverted.fetch_sub(1, std::memory_order_relaxed);
}

uint64_t surfaceWrites(const SurfaceWriteCount *count)
{
    return count ? count->writes.load(std::memory_order_acquire) : 0;
}

static void noteWrite(SurfaceWriteCount *count)
{
    if (count && count->converted.load(std::memory_order_relaxed))
        count->writes.fetch_add(1, std::memory_order_release);
}

void noteSurfaceWrite(SurfaceWriteCount *count)
{
    if (gConverted.load(std::memory_order_relaxed))
        noteWrite(count);
}

void noteSurfaceWrites(RMCommandBuffer *cb, size_t from, size_t to)
{
    if (!gConverted.load(std::memory_order_relaxed))
        return;
    const RMCommandPlan &plan = cb->_plan;
    const bool cached = plan.forEachRangeResource(from, to, [](const RMRangeResource &resource) {
        if (resource.access & raw::ResourceUse::Write)
            noteWrite(static_cast<SurfaceWriteCount *>(resource.facts.surfaceWrites));
    });
    if (!cached)
        plan.forEachWritten(from, to, [&](void *object) {
            if (const RMResourceFacts *facts = plan.factsOf(object))
                noteWrite(static_cast<SurfaceWriteCount *>(facts->surfaceWrites));
        });
}
