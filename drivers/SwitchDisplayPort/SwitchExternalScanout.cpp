// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchExternalScreen.h"
#include "TegraDisplayB.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"
#include "../NativeMobileDisplay/SwitchSurfaceUse.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUPublish.h"
#include "../TegraVIC/TegraVICSurface.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
extern "C" uint64_t native_planes_surface_plane_offset(OSObject *, uint32_t),
    native_planes_surface_plane_bytes_per_row(OSObject *, uint32_t),
    native_planes_surface_plane_height(OSObject *, uint32_t);

bool SwitchExternalScreen::mapDomain()
{
    if (mapped)
        return true;
    cleanDomain();
    char why[128];
    mapped =
        tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY_B, domain.ptb(), TEGRA_SMMU_DCB_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0,
                          (1u << 2) | (1u << 4) | (1u << 6), why, sizeof(why)) == 0;
    if (!mapped)
        IOLog("SwitchDisplayPort: external scanout: %s\n", why);
    return mapped;
}

void SwitchExternalScreen::cleanDomain()
{
    switch_smmu_publish(domain, TEGRA_SMMU_ASID_DISPLAY_B, SWITCH_DCACHE_CLEAN);
}

void SwitchExternalScreen::releaseRead(unsigned index)
{
    auto &slot = slots[index];
    if (slot.counted)
        switch_surface_use_decrement(slot.surface, 0);
    slot.counted = false;
    slot.convertPending = false;
    slot.nextKey = {};
    switch_native_scanout_deliver(&slot.read);
    switch_native_scanout_release(&slot.read);
    switch_native_scanout_release(&slot.source);
}

void SwitchExternalScreen::publishSlotPa(unsigned index, const IOMemoryDescriptor *memory, addr64_t firstPa)
{
    auto &entry = slotPaCache[index];
    const uint32_t seq = __atomic_load_n(&entry.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&entry.seq, seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&entry.memory, memory, __ATOMIC_RELAXED);
    __atomic_store_n(&entry.firstPa, firstPa, __ATOMIC_RELAXED);
    __atomic_store_n(&entry.seq, seq + 2u, __ATOMIC_RELEASE);
}

addr64_t SwitchExternalScreen::cachedFirstPa(const IOMemoryDescriptor *memory) const
{
    for (unsigned i = 0; i < kSlots; ++i) {
        const auto &entry = slotPaCache[i];
        for (unsigned tries = 0; tries < 2; ++tries) {
            const uint32_t s1 = __atomic_load_n(&entry.seq, __ATOMIC_ACQUIRE);
            if (s1 & 1u)
                continue;
            const IOMemoryDescriptor *cachedMem = __atomic_load_n(&entry.memory, __ATOMIC_RELAXED);
            const addr64_t pa = __atomic_load_n(&entry.firstPa, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&entry.seq, __ATOMIC_RELAXED) != s1)
                continue;
            if (cachedMem == memory && pa)
                return pa;
            break;
        }
    }
    return 0;
}

void SwitchExternalScreen::cacheValidatedLayers(const OriginalLegacySwapRequest *request, uint32_t id, uint32_t updated,
                                                const bool hasSurface[], OSObject *const surfaces[],
                                                const native_planes::Layer layers[])
{
    if (!validatedCache)
        return;
    const unsigned idx = static_cast<unsigned>((reinterpret_cast<uintptr_t>(request) >> 6) % kValidatedCache);
    auto &entry = validatedCache[idx];
    uint32_t seq = __atomic_load_n(&entry.seq, __ATOMIC_RELAXED);
    for (unsigned tries = 0; tries < 8; ++tries) {
        if (seq & 1u) {
            seq = __atomic_load_n(&entry.seq, __ATOMIC_RELAXED);
            continue;
        }
        if (__atomic_compare_exchange_n(&entry.seq, &seq, seq + 1u, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            entry.request = request;
            entry.swapIdValue = id;
            entry.updated = updated;
            for (unsigned i = 0; i < kLayers; ++i) {
                entry.surfaces[i] = hasSurface[i] ? surfaces[i] : nullptr;
                if (hasSurface[i])
                    entry.layers[i] = layers[i];
            }
            __atomic_store_n(&entry.seq, seq + 2u, __ATOMIC_RELEASE);
            return;
        }
    }
}

bool SwitchExternalScreen::lookupValidatedLayers(const OriginalLegacySwapRequest *request, uint32_t id,
                                                 uint32_t updated, OSObject *surfaces[],
                                                 native_planes::Layer layers[]) const
{
    if (!validatedCache)
        return false;
    const unsigned idx = static_cast<unsigned>((reinterpret_cast<uintptr_t>(request) >> 6) % kValidatedCache);
    const auto &entry = validatedCache[idx];
    for (unsigned tries = 0; tries < 2; ++tries) {
        const uint32_t s1 = __atomic_load_n(&entry.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)
            continue;
        const auto *req = entry.request;
        const uint32_t entryId = entry.swapIdValue;
        const uint32_t entryUpdated = entry.updated;
        if (req != request || entryId != id || entryUpdated != updated) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&entry.seq, __ATOMIC_RELAXED) == s1)
                return false;
            continue;
        }
        for (unsigned i = 0; i < kLayers; ++i) {
            surfaces[i] = entry.surfaces[i];
            if (surfaces[i])
                layers[i] = entry.layers[i];
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&entry.seq, __ATOMIC_RELAXED) == s1)
            return true;
    }
    return false;
}

void SwitchExternalScreen::releaseSlot(unsigned index)
{
    auto &slot = slots[index];
    releaseRead(index);
    if (slot.backing) {
        if (!slot.conversion)
            switch_gpu_backing_scanout(slot.backing, 0);
        switch_gpu_backing_release(slot.backing);
    }
    if (slot.hold)
        tegra_gm20b_surface_unhold(slot.hold);
    if (slot.pages) {
        domain.unmap(kSlotBase + index * kSlotSpan, slot.pages);
        cleanDomain();
    }
    if (slot.memory) {
        publishSlotPa(index, nullptr, 0);
        slot.memory->complete(kIODirectionOut);
        slot.memory->release();
    }
    OSSafeReleaseNULL(slot.surface);
    slot = {};
}

bool SwitchExternalScreen::slotShown(int index) const
{
    for (unsigned w = 0; w < kWindows; ++w)
        if (index >= 0 && (front[w] == index || drawing[w] == index))
            return true;
    return false;
}

// The GPU's layout of the surface, held while the slot maps it so it is not laid out again under the windows.
bool SwitchExternalScreen::applyLayout(Slot &slot, uint64_t firstPage, uint64_t bytes)
{
    int tiled = 0;
    void *held = tegra_gm20b_surface_hold(firstPage, &tiled);
    slot.block = {};
    if (tegra_gm20b_surface_compressed(firstPage) ||
        (tiled && (tegra_gm20b_surface_layout(firstPage, &slot.block) != 1 || slot.block.bytes > bytes))) {
        slot.block = {};
        if (held)
            tegra_gm20b_surface_unhold(held);
        return false;
    }
    slot.hold = held;
    return true;
}

// The slot mapping `surface` with a read of it taken, or -16 / SWITCH_GPU_BACKING_STALE to retry when the GPU is ready,
// or -1 when it cannot be scanned. `building`: slots this frame already took.
int SwitchExternalScreen::prepareSlot(OSObject *surface, uint32_t building)
{
    auto *memory = native_legacy_surface_descriptor(surface);
    SwitchNativeScanout snapshot = {};
    switch_gpu_backing *held = nullptr;
    for (unsigned i = 0; i < kSlots; ++i)
        if (slots[i].surface == surface && slots[i].memory == memory)
            held = slots[i].backing;
    if (held) {
        const int heldError = switch_native_scanout_pitch_held(surface, held, &snapshot);
        if (heldError == -16)
            return -16;
        if (heldError) {
            const int error = switch_native_scanout_pitch(surface, &snapshot);
            if (error)
                return error;
        }
    } else {
        const int error = switch_native_scanout_pitch(surface, &snapshot);
        if (error)
            return error;
    }
    const auto &allocation = snapshot.allocation;
    const uint64_t offset = snapshot.pitchOffset;
    const uint64_t bytes = native_legacy_surface_allocation(surface);
    const uint32_t pages = static_cast<uint32_t>((bytes + 4095) / 4096);
    if (!memory || !allocation.pages || !bytes || (offset & 4095) || bytes > kSlotSpan || bytes > memory->getLength() ||
        offset / 4096 > allocation.page_count || pages > allocation.page_count - offset / 4096 || !mapDomain()) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    int victim = -1;
    for (unsigned i = 0; i < kSlots; ++i) {
        auto &slot = slots[i];
        const bool free = !slotShown(static_cast<int>(i)) && !(building & (1u << i));
        if (slot.memory == memory && slot.surface == surface && slot.pages == pages && slot.offset == offset &&
            slot.allocationId == allocation.allocation_id && slot.generation == allocation.allocation_generation) {
            const uint64_t firstPage = allocation.pages[offset / 4096] & ~0xfffULL;
            if (tegra_gm20b_surface_compressed(firstPage)) {
                switch_native_scanout_release(&snapshot);
                if (free)
                    releaseSlot(i);
                return -1;
            }
            if (!slot.hold && !applyLayout(slot, firstPage, bytes)) {
                switch_native_scanout_release(&snapshot);
                if (free && tegra_gm20b_surface_compressed(firstPage))
                    releaseSlot(i);
                return -1;
            }
            if (slot.read.lease) {
                if (slot.read.allocation.content_generation != snapshot.allocation.content_generation ||
                    (snapshot.promiseCount && !switch_native_scanout_same_promises(slot.read, snapshot))) {
                    switch_native_scanout_release(&snapshot);
                    return -1;
                }
                switch_native_scanout_release(&snapshot);
            } else {
                slot.read = snapshot;
            }
            slot.used = ++slotUses;
            if (!slot.counted && switch_display::surfaceUseCounted(surface)) {
                switch_surface_use_increment(surface, 0);
                slot.counted = true;
            }
            return static_cast<int>(i);
        }
        if (free && (victim < 0 || slot.used < slots[victim].used))
            victim = static_cast<int>(i);
    }
    const uint64_t surfW = native_legacy_surface_width(surface);
    const uint64_t surfH = native_legacy_surface_height(surface);
    if (victim < 0 || !surfW || !surfH || surfW > SWITCH_DP_MAX_WIDTH || surfH > SWITCH_DP_MAX_HEIGHT) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    const unsigned index = static_cast<unsigned>(victim);
    releaseSlot(index);
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    if (!domain.map(kSlotBase + index * kSlotSpan, allocation.pages + offset / 4096, pages, false)) {
        domain.unmap(kSlotBase + index * kSlotSpan, pages);
        cleanDomain();
        memory->complete(kIODirectionOut);
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    auto &slot = slots[index];
    slot.read = snapshot;
    slot.memory = memory;
    memory->retain();
    slot.surface = surface;
    surface->retain();
    slot.pages = pages;
    slot.offset = offset;
    slot.allocationId = allocation.allocation_id;
    slot.generation = allocation.allocation_generation;
    slot.allocationBytes = bytes;
    slot.used = ++slotUses;
    slot.backing = snapshot.view.native;
    switch_gpu_backing_retain(slot.backing);
    switch_gpu_backing_scanout(slot.backing, 1);
    slot.counted = switch_display::surfaceUseCounted(surface);
    if (slot.counted)
        switch_surface_use_increment(surface, 0);
    if (!applyLayout(slot, allocation.pages[offset / 4096] & ~0xfffULL, bytes)) {
        releaseSlot(index);
        return -1;
    }
    addr64_t firstPa = offset == 0 ? allocation.pages[offset / 4096] : 0;
    if (!firstPa) {
        IOByteCount segLen = 0;
        firstPa = memory->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    }
    publishSlotPa(index, memory, firstPa);
    cleanDomain();
    return victim;
}

const tgm_layout *SwitchExternalScreen::slotBlock(const Slot &slot)
{
    return slot.block.bpp ? &slot.block : nullptr;
}

native_planes::Layer SwitchExternalScreen::layerOf(const uint8_t *request, unsigned layer, OSObject *surface)
{
    const auto narrow = [](uint64_t v) {
        return v > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(v);
    };
    native_planes::Layer l;
    native_planes::Surface &f = l.surface;
    f.format = native_legacy_surface_format(surface);
    f.planes = native_legacy_surface_planes(surface);
    f.width = narrow(native_legacy_surface_width(surface));
    f.height = narrow(native_legacy_surface_height(surface));
    f.allocation = native_legacy_surface_allocation(surface);
    const IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    f.descriptorBytes = memory && memory->metaCast("IOMemoryDescriptor") ? memory->getLength() : 0;
    l.kind = native_planes::kindOf(f.format, f.planes);
    if (native_planes::isYuv(l.kind)) {
        TegraVICSurfaceDesc color = {};
        switch_vic_surface_color(surface, &color);
        f.colorMatrix = color.colorMatrix;
        f.transfer = color.transfer;
        f.primaries = color.primaries;
        f.chromaTop = color.chromaTop;
        f.chromaBottom = color.chromaBottom;
    }
    if (f.planes >= 2) {
        for (uint32_t p = 0; p < 2; ++p) {
            f.offset[p] = native_planes_surface_plane_offset(surface, p);
            f.stride[p] = narrow(native_planes_surface_plane_bytes_per_row(surface, p));
            f.planeHeight[p] = narrow(native_planes_surface_plane_height(surface, p));
        }
    } else {
        f.stride[0] = narrow(native_legacy_surface_stride(surface));
        f.planeHeight[0] = f.height;
    }
    l.src = native_planes::rectAt(request, swapSourceRects, layer);
    l.dst = native_planes::rectAt(request, swapDestinationRects, layer);
    l.transform = native_planes::wordAt(request, swapTransforms + 4u * layer) & 7u;
    return l;
}

// Every swap the family hands over, shown or drained, records the layers it updates: a later swap inherits them. The
// surface is retained, so an inherited pointer that matches a record is that record's surface.
void SwitchExternalScreen::recordLayers(const uint8_t *request)
{
    const uint32_t updated = native_planes::wordAt(request, swapUpdated);
    OSObject *effective[kLayers] = {};
    memcpy(effective, request + swapEffectiveSurfaces, sizeof(effective));
    uint32_t id = 0;
    memcpy(&id, request + swapId, sizeof(id));
    OSObject *cachedSurfaces[kLayers] = {};
    native_planes::Layer cachedLayers[kLayers];
    const bool cached = lookupValidatedLayers(reinterpret_cast<const OriginalLegacySwapRequest *>(request), id, updated,
                                              cachedSurfaces, cachedLayers);
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!(updated & (1u << i)))
            continue;
        OSObject *surface = effective[i] && effective[i]->metaCast("IOSurface") ? effective[i] : nullptr;
        LayerRecord &record = layerRecords[i];
        if (surface != record.surface) {
            OSSafeReleaseNULL(record.surface);
            if (surface)
                surface->retain();
            record.surface = surface;
        }
        if (surface)
            record.layer = cached && cachedSurfaces[i] == surface ? cachedLayers[i] : layerOf(request, i, surface);
    }
}

// The layers a kicked swap shows: an updated layer as the swap gives it, any other as the records hold it. False when an
// inherited surface has no record (its geometry is not known), so the swap cannot be shown whole.
bool SwitchExternalScreen::takeLayers(const uint8_t *request, bool present[], native_planes::Layer layers[],
                                      OSObject *surfaces[])
{
    recordLayers(request);
    OSObject *effective[kLayers] = {};
    memcpy(effective, request + swapEffectiveSurfaces, sizeof(effective));
    for (unsigned i = 0; i < kLayers; ++i) {
        OSObject *surface = effective[i] && effective[i]->metaCast("IOSurface") ? effective[i] : nullptr;
        LayerRecord &record = layerRecords[i];
        if (!surface) {
            OSSafeReleaseNULL(record.surface);
        } else if (surface != record.surface) {
            noteUnshown(i, -2);
            return false;
        }
        present[i] = surface != nullptr;
        layers[i] = record.layer;
        surfaces[i] = surface;
    }
    return true;
}

void SwitchExternalScreen::releaseUnshown(uint32_t building)
{
    for (unsigned i = 0; i < kSlots; ++i)
        if ((building & (1u << i)) && !slotShown(static_cast<int>(i)))
            releaseRead(i);
}

void SwitchExternalScreen::requestFrame()
{
    const auto *request = pendingSwapRequest();
    if (!request)
        return;
    const auto *bytes = reinterpret_cast<const uint8_t *>(request);
    if ((native_planes::wordAt(bytes, swapUpdated) & ~native_planes::wordAt(bytes, swapReady)) &
        native_planes::kLayerBits) {
        if (dcb)
            dcb->frameInterrupt(true);
        return;
    }
    bool present[kLayers] = {};
    native_planes::Layer layers[kLayers];
    OSObject *surfaces[kLayers] = {};
    if (!takeLayers(bytes, present, layers, surfaces)) {
        dropFrame(request);
        return;
    }
    // RGB layers are mapped first: their image depends on the layout the GPU gave them. A YUV layer is in rows (the GPU
    // lays out no planar or packed YUV block-linear) and is mapped, or converted, once the plan says which.
    native_planes::Image images[kLayers];
    int slotOf[kLayers] = {-1, -1, -1, -1};
    uint32_t building = 0;
    const uint32_t width = mode.hactive, height = mode.vactive;
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!present[i])
            continue;
        if (native_planes::isYuv(layers[i].kind)) {
            if (!switch_external::imageFor(layers[i], nullptr, width, height, images[i]))
                unscannable(i);
            continue;
        }
        const int prepared = prepareSlot(surfaces[i], building);
        if (prepared < 0) {
            unshownFrame(prepared, i, building, request);
            return;
        }
        building |= 1u << prepared;
        slotOf[i] = prepared;
        if (!switch_external::imageFor(layers[i], slotBlock(slots[prepared]), width, height, images[i]))
            unscannable(i);
    }
    switch_external::WindowState states[kWindows];
    for (unsigned w = 0; w < kWindows; ++w)
        states[w] = dcb->windowState(w);
    switch_external::Plan plan;
    if (!switch_external::planSwap(present, layers, images, states, width, height, plan))
        unscannable(static_cast<unsigned>(__builtin_ctz(plan.convert)));
    if (plan.unshown)
        unscannable(static_cast<unsigned>(__builtin_ctz(plan.unshown)));
    for (const auto &placement : plan.window) {
        const unsigned i = static_cast<unsigned>(placement.layer);
        if (!placement.on || !native_planes::isYuv(layers[i].kind))
            continue;
        const bool convert = (plan.convert & (1u << i)) != 0;
        const int prepared = convert ? prepareConversion(surfaces[i], i, building) : prepareSlot(surfaces[i], building);
        if (prepared < 0) {
            unshownFrame(prepared, i, building, request);
            return;
        }
        building |= 1u << prepared;
        slotOf[i] = prepared;
        if (!convert && slotBlock(slots[prepared]))
            unscannable(i);
    }
    for (unsigned w = 0; w < kWindows; ++w)
        drawing[w] = plan.window[w].on ? slotOf[plan.window[w].layer] : -1;
    releaseUnshown(building);
    uint32_t iova[kLayers] = {};
    for (unsigned i = 0; i < kLayers; ++i)
        if (slotOf[i] >= 0)
            iova[i] = static_cast<uint32_t>(kSlotBase + static_cast<uint64_t>(slotOf[i]) * kSlotSpan);
    pendingFrame = switch_external::frameOf(plan, images, iova, dcb->pixelClockHz());
    pending = request;
    gpuGeneration = linkGeneration;
#if DEVELOPMENT || DEBUG
    noteShape(native_planes::wordAt(bytes, swapUpdated), layers, present, plan, slotOf);
#endif
    fenceFrame();
}
