// SPDX-License-Identifier: BSD-2-Clause-Patent
// YUV layers no DC B window can scan as they are (SwitchExternalPlanes.h planSwap), converted by VIC to BGRA rows of
// the screen's own, as the panel converts its overlays (SwitchPlaneConversionDisplay.h). A converted slot keeps its
// buffer and what it was converted from, so a swap that inherits the layer unchanged shows it with no VIC job.
#include "SwitchExternalScreen.h"
#include "TegraDisplayB.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraVIC/SwitchVICScanoutConvert.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

namespace {
void freeConversionPages(void *context)
{
    auto *pages = static_cast<switch_dma_pages *>(context);
    switch_dma_pages_free(pages);
    IODelete(pages, switch_dma_pages, 1);
}

// The buffer VIC converts into, of any size a slot maps (SwitchScanoutBuffer.h caps its buffers at 20 MiB, a 4K BGRA
// is 33 MB).
switch_gpu_backing *conversionBuffer(uint64_t bytes)
{
    auto *owner = IONewZero(switch_dma_pages, 1);
    if (!owner)
        return nullptr;
    if (!switch_dma_pages_alloc(owner, bytes)) {
        IODelete(owner, switch_dma_pages, 1);
        return nullptr;
    }
    auto *backing = switch_gpu_backing_register(owner->pages, owner->count, 0, owner, freeConversionPages);
    if (!backing)
        freeConversionPages(owner);
    return backing;
}

// The source as VIC reads it from the snapshot's pages. A siting VIC cannot convert is read co-sited, as a window would
// scan it.
const char *conversionInput(const native_planes::Surface &s, uint64_t offset, TegraVICImage &input)
{
    TegraVICSurfaceDesc desc = {};
    desc.fourcc = s.format;
    desc.width = s.width;
    desc.height = s.height;
    desc.planes = s.planes;
    desc.length = s.descriptorBytes;
    desc.colorMatrix = s.colorMatrix < SWITCH_VIC_MATRIX_UNSUPPORTED ? s.colorMatrix : uint32_t(SWITCH_VIC_MATRIX_601);
    desc.transfer = s.transfer;
    desc.primaries = s.primaries;
    uint32_t site = 0;
    if (switch_surface_chroma_site(s.chromaTop, s.chromaBottom, &site)) {
        desc.chromaTop = s.chromaTop;
        desc.chromaBottom = s.chromaBottom;
    }
    for (unsigned p = 0; p < (s.planes >= 2 ? 2u : 1u); ++p) {
        desc.planeOffset[p] = s.planes >= 2 ? s.offset[p] : 0;
        desc.planeBytesPerRow[p] = s.stride[p];
        desc.planeHeight[p] = s.planes >= 2 ? s.planeHeight[p] : s.height;
    }
    return tegra_vic_image_from(&desc, offset, &input);
}
}  // namespace

// Whether free slot i takes a conversion of `bytes` before `victim`: an idle conversion buffer before an empty or RGB
// slot, so buffers are allocated only while every one is on the screen or in the frame being built (two for a video:
// the frame shown and the next). Then one with room for it, then the least recently used.
bool SwitchExternalScreen::betterVictim(unsigned i, int victim, uint64_t bytes) const
{
    if (victim < 0)
        return true;
    const Slot &slot = slots[i], &other = slots[victim];
    if (slot.conversion != other.conversion)
        return slot.conversion;
    const bool fits = slot.conversion && slot.allocationBytes >= bytes;
    const bool otherFits = other.conversion && other.allocationBytes >= bytes;
    if (fits != otherFits)
        return fits;
    return slot.used < other.used;
}

// The slot showing `layer` converted: one that already holds this picture so converted, or a free one with a VIC job
// queued for the fence thread (convertPending). Errors as prepareSlot's.
int SwitchExternalScreen::prepareConversion(OSObject *surface, unsigned layer, uint32_t building)
{
    SwitchNativeScanout source = {};
    if (const int error = switch_native_scanout_pitch(surface, &source))
        return error;
    const auto &allocation = source.allocation;
    const uint64_t page = source.pitchOffset / 4096;
    tgm_layout tiled = {};
    if (allocation.pages && page < allocation.page_count &&
        tegra_gm20b_surface_layout(allocation.pages[page] & ~0xfffULL, &tiled) == 1)
        unscannable(layer);
    const native_planes::Surface &s = layerRecords[layer].layer.surface;
    const switch_external::Conversion c = switch_external::conversionOf(layerRecords[layer].layer);
    const ConversionKey key = {allocation.allocation_id,
                               allocation.allocation_generation,
                               allocation.content_generation,
                               source.pitchOffset,
                               s.format,
                               s.width,
                               s.height,
                               c.divisor,
                               s.colorMatrix,
                               s.chromaTop,
                               s.chromaBottom,
                               1};
    int victim = -1;
    for (unsigned i = 0; i < kSlots; ++i) {
        Slot &slot = slots[i];
        if (slot.conversion && !memcmp(&slot.key, &key, sizeof(key))) {
            switch_native_scanout_release(&source);
            if (!slot.read.lease && !slot.convertPending) {
                if (const int error =
                        switch_gpu_backing_export_read(slot.backing, &slot.read.view.producer, &slot.read.lease))
                    return error;
                slot.read.view.native = slot.backing;
                switch_gpu_backing_retain(slot.backing);
                switch_gpu_backing_describe(slot.backing, &slot.read.allocation);
            }
            slot.used = ++slotUses;
            return static_cast<int>(i);
        }
        if (!slotShown(static_cast<int>(i)) && !(building & (1u << i)) && betterVictim(i, victim, c.bytes))
            victim = static_cast<int>(i);
    }
    TegraVICImage input = {};
    if (victim < 0 || c.bytes > kSlotSpan || conversionInput(s, source.pitchOffset, input) || !mapDomain()) {
        switch_native_scanout_release(&source);
        return victim < 0 ? -11 : -95;
    }
    const unsigned index = static_cast<unsigned>(victim);
    Slot &slot = slots[index];
    if (slot.conversion && slot.allocationBytes >= c.bytes) {
        releaseRead(index);
    } else {
        releaseSlot(index);
        switch_gpu_backing *buffer = conversionBuffer(c.bytes);
        switch_gpu_backing_info info = {};
        if (buffer)
            switch_gpu_backing_describe(buffer, &info);
        if (!buffer || !domain.map(kSlotBase + index * kSlotSpan, info.pages, info.page_count, false)) {
            if (buffer) {
                domain.unmap(kSlotBase + index * kSlotSpan, info.page_count);
                switch_gpu_backing_release(buffer);
            }
            cleanDomain();
            switch_native_scanout_release(&source);
            return -12;
        }
        cleanDomain();
        slot.backing = buffer;
        slot.pages = info.page_count;
        slot.allocationBytes = uint64_t(info.page_count) * 4096;
        slot.conversion = true;
    }
    slot.source = source;
    slot.key = {};
    slot.nextKey = key;
    slot.convertInput = input;
    slot.convertOp = {};
    slot.convertOp.srcW = uint64_t(s.width) << 16;
    slot.convertOp.srcH = uint64_t(s.height) << 16;
    slot.convertOp.dstW = c.width;
    slot.convertOp.dstH = c.height;
    slot.convertWidth = c.width;
    slot.convertHeight = c.height;
    slot.convertPitch = c.pitch;
    slot.convertPending = true;
    slot.used = ++slotUses;
    return victim;
}

// The fence thread, before it fences the frame: VIC's jobs for the frame's converted slots. The work loop leaves the
// frame's slots alone until the frame completes.
int SwitchExternalScreen::runConversions()
{
    for (const int k : fenceSlots) {
        if (k < 0 || !slots[k].convertPending)
            continue;
        Slot &slot = slots[k];
        slot.convertPending = false;
        if (const int error =
                switch_vic_scanout_convert(&slot.source, &slot.convertInput, slot.backing, slot.convertWidth,
                                           slot.convertHeight, slot.convertPitch, &slot.convertOp, &slot.read))
            return error;
    }
    return 0;
}

// A swap that cannot be shown whole now: -16 and STALE wait for the GPU, anything else retires it unshown and the screen
// keeps its frame.
void SwitchExternalScreen::unshownFrame(int error, unsigned layer, uint32_t building,
                                        const OriginalLegacySwapRequest *request)
{
    releaseUnshown(building);
    if (error == -16 || error == SWITCH_GPU_BACKING_STALE) {
        retryWhenReady();
        return;
    }
    noteUnshown(layer, error);
    dropFrame(request);
}

// A layer validateSwap accepted that no window reads, as it is or converted: checkSwap holds every layer to what imageOf
// scans in rows, the GPU lays out only RGB block-linear, and a conversion's BGRA keeps the DDA within an RGB window's.
// A fourth visible layer is one checkSwap did not count (an earlier swap left it on), and clone_update shows at most
// layers 0 or 1 and 2 (SwitchExternalPlanes.h); so a YUV layer VIC cannot convert always finds a window (Plan::unshown).
void SwitchExternalScreen::unscannable(unsigned layer) const
{
    const auto &l = layerRecords[layer].layer;
    const uint32_t f = l.surface.format;
    panic(
        "SwitchDisplayPort: accepted layer %u '%c%c%c%c' %ux%u src (%d,%d %dx%d) dst (%d,%d %dx%d) has no DC B window",
        layer, static_cast<char>(f >> 24), static_cast<char>(f >> 16), static_cast<char>(f >> 8), static_cast<char>(f),
        l.surface.width, l.surface.height, l.src.x, l.src.y, l.src.w, l.src.h, l.dst.x, l.dst.y, l.dst.w, l.dst.h);
}
