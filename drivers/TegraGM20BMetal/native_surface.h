// SPDX-License-Identifier: MIT
#pragma once
#include "lock.h"
#include "native_plane.h"
#include <IOSurface/IOSurfaceRef.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace gm20b {

// Under the device's lock: the import's view leaves the cache, and is freed once no job or encode holds it
inline void uncacheNative(Import &import, std::atomic<bool> &stale)
{
    import.native->cached = false;
    stale.store(true, std::memory_order_release);
    import.native = nullptr;
}

inline uint32_t surfaceSeed(const Import &import)
{
    return IOSurfaceGetSeed(static_cast<IOSurfaceRef>(const_cast<void *>(import.owner)));
}

const Texture *Device::nativeRead(const Texture &source, NativeSurface *&owner, int &error)
{
    owner = nullptr;
    if (!k_.exactJobs() || !source.import || source.nativePlane == ~0u || source.chain || source.layers != 1 ||
        source.depth != 1 || source.samples != 1 || source.blockW != 1 || source.blockH != 1 ||
        source.type != fx::kTexTwoD)
        return &source;
    uint32_t swizzle = 0;
    memcpy(&swizzle, source.swizzle, sizeof swizzle);
    const NativeSurface::Key key{source.nativePlane, source.width, source.height, source.format, swizzle};
    Import &import = *source.import;
    // The kernel fixes a view's guards when it makes it (SwitchGpuNativeImport.h). The seed, read before the view is made,
    // moves with each decoder publication and CPU write lock that stales them; a GPU write does not move it (requirePitch).
    const uint32_t seed = surfaceSeed(import);
    NativeSurface *surface = import.native;
    // The submit worker alone changes this cache; the retained source prevents final import release.
    if (surface && surface->seed == seed) {
        const auto found = surface->textures.find(key);
        if (found != surface->textures.end()) {
            owner = surface;
            return &found->second;
        }
    }
    std::lock_guard<gm20b::Mutex> lock(lock_);
    if (surface && surface->seed != seed) {
        uncacheNative(import, nativeStale_);
        surface = nullptr;
    }
    if (!surface) {
        auto made = std::make_unique<NativeSurface>();
        made->seed = seed;
        const int result = k_.nativeSurfaceView(import.bo, made->bo, made->layout);
        if (result != KERN_SUCCESS) {
            error = result;
            return &source;
        }
        if (!made->bo.handle)
            return &source;
        made->generation = ++nativeGeneration_;
        surface = made.get();
        nativeSurfaces_.push_back(std::move(made));
        import.native = surface;
    }
    if (!nativePlaneFits(surface->layout, source.nativePlane, source.width, source.height, source.bpp)) {
        // No submitted guard can invalidate a view we did not use. Requery after an unsupported representation.
        uncacheNative(import, nativeStale_);
        return &source;
    }
    auto found = surface->textures.find(key);
    if (found == surface->textures.end()) {
        Texture texture;
        if (!shape(source.width, source.height, source.format, texture)) {
            error = kIOReturnBadArgument;
            return &source;
        }
        const auto &plane = surface->layout.planes[source.nativePlane];
        texture.nativeGeneration = surface->generation;
        texture.va = surface->bo.va + plane.offset;
        texture.pitch = plane.pitch;
        texture.layerBytes = plane.bytes;
        texture.blockLinear = surface->layout.kind == TGM_LAYOUT_KIND;
        texture.blockHeight = uint8_t(plane.block_height_log2);
        texture.renderable = surface->writable && texture.blockLinear && (source.renderable || source.shadowTarget);
        memcpy(texture.swizzle, source.swizzle, sizeof texture.swizzle);
        if (!(texture.tic = allocTic())) {
            error = kIOReturnNoResources;
            return &source;
        }
        writeTic(texture);
        found = surface->textures.emplace(key, texture).first;
    }
    owner = surface;
    return &found->second;
}

const Texture *Device::nativeWrite(const Texture &source, NativeSurface *&owner, int &error)
{
    owner = nullptr;
    if (!k_.exactJobs() || !source.import || source.nativePlane == ~0u || source.chain || source.layers != 1 ||
        source.depth != 1 || source.samples != 1 || source.blockW != 1 || source.blockH != 1 ||
        source.type != fx::kTexTwoD || (!source.renderable && !source.shadowTarget))
        return &source;
    uint32_t swizzle = 0;
    memcpy(&swizzle, source.swizzle, sizeof swizzle);
    const NativeSurface::Key key{source.nativePlane, source.width, source.height, source.format, swizzle};
    Import &import = *source.import;
    const uint32_t seed = surfaceSeed(import);
    NativeSurface *surface = import.native;
    if (surface && surface->writable && surface->seed == seed) {
        const auto found = surface->textures.find(key);
        if (found != surface->textures.end() && found->second.renderable) {
            owner = surface;
            return &found->second;
        }
    }
    std::lock_guard<gm20b::Mutex> lock(lock_);
    if (surface && (!surface->writable || surface->seed != seed)) {
        uncacheNative(import, nativeStale_);
        surface = nullptr;
    }
    if (!surface) {
        auto made = std::make_unique<NativeSurface>();
        const int result = k_.nativeSurfaceView(import.bo, made->bo, made->layout, true);
        if (result == kIOReturnBadArgument || result == kIOReturnUnsupported)
            return &source;
        if (result != KERN_SUCCESS) {
            error = result;
            return &source;
        }
        if (!made->bo.handle)
            return &source;
        if (made->layout.kind != TGM_LAYOUT_KIND) {
            k_.free(made->bo);
            return &source;
        }
        // First writable publication moves the surface seed from 0 to 1.
        made->seed = surfaceSeed(import);
        made->writable = true;
        made->generation = ++nativeGeneration_;
        surface = made.get();
        nativeSurfaces_.push_back(std::move(made));
        import.native = surface;
    }
    if (!nativePlaneFits(surface->layout, source.nativePlane, source.width, source.height, source.bpp)) {
        uncacheNative(import, nativeStale_);
        return &source;
    }
    auto found = surface->textures.find(key);
    if (found == surface->textures.end()) {
        Texture texture;
        if (!shape(source.width, source.height, source.format, texture)) {
            error = kIOReturnBadArgument;
            return &source;
        }
        const auto &plane = surface->layout.planes[source.nativePlane];
        texture.nativeGeneration = surface->generation;
        texture.va = surface->bo.va + plane.offset;
        texture.pitch = plane.pitch;
        texture.layerBytes = plane.bytes;
        texture.blockLinear = true;
        texture.blockHeight = uint8_t(plane.block_height_log2);
        texture.renderable = true;
        memcpy(texture.swizzle, source.swizzle, sizeof texture.swizzle);
        if (!(texture.tic = allocTic())) {
            error = kIOReturnNoResources;
            return &source;
        }
        writeTic(texture);
        found = surface->textures.emplace(key, texture).first;
    }
    owner = surface;
    return &found->second;
}

void Device::reapNativeSurfaces(std::vector<Bo> &detached, bool all)
{
    for (auto it = nativeSurfaces_.begin(); it != nativeSurfaces_.end();) {
        NativeSurface &surface = **it;
        const bool unused = !surface.cached && !__atomic_load_n(&surface.encodingReferences, __ATOMIC_ACQUIRE);
        if (all || (unused && (surface.channelGeneration != channelGeneration() || done(surface.lastUse)))) {
            for (const auto &[key, texture] : surface.textures)
                freeTic(texture.tic);
            detached.push_back(surface.bo);
            it = nativeSurfaces_.erase(it);
        } else {
            ++it;
        }
    }
}

void CommandBuffer::requirePitch(const Import *source, bool writes)
{
    if (!source)
        return;
    if (std::find(pitchImports_.begin(), pitchImports_.end(), source) == pitchImports_.end())
        pitchImports_.push_back(source);
    // A GPU write of the surface invalidates its native publication in the kernel without moving its seed
    if (writes && source->native) {
        std::lock_guard<gm20b::Mutex> lock(dev_->lock_);
        uncacheNative(*const_cast<Import *>(source), dev_->nativeStale_);
    }
}

bool CommandBuffer::nativeReadsCurrent() const
{
    const auto current = [](const std::map<const Texture *, const Texture *> &views) {
        for (const auto &[source, view] : views) {
            if (view == source)
                continue;
            const Import &import = *source->import;
            if (!import.native || import.native->generation != view->nativeGeneration ||
                import.native->seed != surfaceSeed(import))
                return false;
        }
        return true;
    };
    return current(nativeReads_) && current(nativeWrites_);
}

bool CommandBuffer::pitchAfterNativeRead() const
{
    const auto conflicts = [this](const std::map<const Texture *, const Texture *> &views) {
        for (const auto &[source, view] : views)
            if (view != source &&
                std::find(pitchImports_.begin(), pitchImports_.end(), source->import) != pitchImports_.end())
                return true;
        return false;
    };
    return conflicts(nativeReads_) || conflicts(nativeWrites_);
}

const Texture *CommandBuffer::readTexture(const Texture *source)
{
    if (!source || !source->import || source->nativePlane == ~0u ||
        std::find(pitchImports_.begin(), pitchImports_.end(), source->import) != pitchImports_.end())
        return source;
    if (const auto found = nativeReads_.find(source); found != nativeReads_.end())
        return found->second;
    NativeSurface *owner = nullptr;
    const Texture *view = dev_->nativeRead(*source, owner, nativeError);
    nativeReads_.emplace(source, view);
    if (owner && std::find(nativeSurfaces_.begin(), nativeSurfaces_.end(), owner) == nativeSurfaces_.end()) {
        __atomic_fetch_add(&owner->encodingReferences, 1u, __ATOMIC_RELAXED);
        nativeSurfaces_.push_back(owner);
    }
    return view;
}

const Texture *CommandBuffer::writeTexture(const Texture *source)
{
    if (!source || !source->import || source->nativePlane == ~0u ||
        std::find(pitchImports_.begin(), pitchImports_.end(), source->import) != pitchImports_.end())
        return source;
    if (const auto found = nativeWrites_.find(source); found != nativeWrites_.end())
        return found->second;
    NativeSurface *owner = nullptr;
    const Texture *view = dev_->nativeWrite(*source, owner, nativeError);
    nativeWrites_.emplace(source, view);
    if (owner && std::find(nativeSurfaces_.begin(), nativeSurfaces_.end(), owner) == nativeSurfaces_.end()) {
        __atomic_fetch_add(&owner->encodingReferences, 1u, __ATOMIC_RELAXED);
        nativeSurfaces_.push_back(owner);
    }
    return view;
}

void CommandBuffer::invalidateNativeReads()
{
    std::lock_guard<gm20b::Mutex> lock(dev_->lock_);
    for (NativeSurface *surface : nativeSurfaces_) {
        surface->cached = false;
        dev_->nativeStale_.store(true, std::memory_order_release);
        for (const auto &[key, import] : dev_->surfaces_)
            if (import->native == surface)
                import->native = nullptr;
    }
}

void CommandBuffer::releaseNativeReads()
{
    for (NativeSurface *surface : nativeSurfaces_)
        __atomic_fetch_sub(&surface->encodingReferences, 1u, __ATOMIC_RELEASE);
    nativeSurfaces_.clear();
    nativeReads_.clear();
    nativeWrites_.clear();
}

void RenderEncoder::setTexture(int stage, uint32_t slot, const Texture *texture)
{
    texture = cb_->readTexture(texture);
    const uint32_t at = stage == 1 ? 0 : 1;
    if (slot < kStageTextures && textures_[at][slot] != texture) {
        textures_[at][slot] = texture;
        textureMoved(at, slot);
    }
}

}  // namespace gm20b
