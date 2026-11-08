// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include <unordered_map>

// A planar or YCbCr texture's image is converted from its surface again once the surface's seed moves (encodeRaw), which a
// CPU write under IOSurfaceLock does. A write the seed does not show is counted here per surface, for the surfaces a converted
// texture is over: one through another texture over the surface (a video player's plane textures), on either driver, or a
// CPU write through Metal. The submit lock orders the counts as the work.
struct SurfaceConversions {
    uint32_t textures = 0;
    uint64_t writes = 0;
};

static std::mutex gLock;
static std::unordered_map<uint32_t, SurfaceConversions>
    gSurfaces;  // IOSurfaceID -> its converted textures and the writes since
static std::atomic<uint32_t> gConverted{0};

void surfaceConverted(IOSurfaceRef s, bool made)
{
    std::lock_guard<std::mutex> l(gLock);
    const uint32_t id = IOSurfaceGetID(s);
    if (made) {
        gSurfaces[id].textures++;
        gConverted.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    gConverted.fetch_sub(1, std::memory_order_relaxed);
    auto it = gSurfaces.find(id);
    if (it != gSurfaces.end() && !--it->second.textures)
        gSurfaces.erase(it);
}

uint64_t surfaceWrites(IOSurfaceRef s)
{
    std::lock_guard<std::mutex> l(gLock);
    auto it = gSurfaces.find(IOSurfaceGetID(s));
    return it == gSurfaces.end() ? 0 : it->second.writes;
}

void noteSurfaceWrite(IOSurfaceRef s)
{
    if (!gConverted.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> l(gLock);
    auto it = gSurfaces.find(IOSurfaceGetID(s));
    if (it != gSurfaces.end())
        it->second.writes++;
}

static void written(void *o)
{
    id object = (__bridge id)o;
    if (![object isKindOfClass:[RMTexture class]])
        return;
    RMTexture *t = (RMTexture *)object;
    while (t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (t->_surface)
        noteSurfaceWrite(t->_surface);
}

void noteSurfaceWrites(RMCommandBuffer *cb, size_t from, size_t to)
{
    if (gConverted.load(std::memory_order_relaxed))
        forEachWritten(cb, from, to, written);
}
