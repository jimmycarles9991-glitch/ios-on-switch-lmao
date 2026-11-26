// SPDX-License-Identifier: BSD-2-Clause-Patent
// A planned frame's way to DC B: one fence over every window's producers, then all windows written in one request, and
// the swap retired at the frame end that shows it.
#include "SwitchExternalScreen.h"
#include <mach/mach_time.h>
#include "TegraDisplayB.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"
#include "../TegraGM20B/SwitchGpuBridge.h"
#include "../TegraHost1x/SwitchHost1xTokenFence.h"

void SwitchExternalScreen::kickSwapQueue()
{
    if (dcb && switch_abi_load<const void *>(this, vsyncClientList))
        dcb->frameInterrupt(true);
    if (kicking || !dcb || pending || device != 2)
        return;
    kicking = true;
    requestFrame();
    kicking = false;
}

namespace {
bool retryable(int error)
{
    return error == -16 || error == SWITCH_GPU_BACKING_STALE;
}

void fenced(void *context, int status)
{
    static_cast<SwitchExternalScreen *>(context)->gpuComplete(status);
}
}  // namespace

// The producers still writing what the next frame's slots (fenceSlots) scan, into fenceTokens; media: VIC's and NVDEC's
// alone.
uint32_t SwitchExternalScreen::gatherFence(bool &media)
{
    uint32_t count = 0;
    media = true;
    for (unsigned w = 0; w < kWindows; ++w) {
        const int k = fenceSlots[w];
        bool seen = false;
        for (unsigned v = 0; v < w; ++v)
            seen = seen || fenceSlots[v] == k;
        if (k < 0 || seen)
            continue;
        const SwitchNativeScanout &read = slots[k].read;
        switch_gpu_backing_token own[3 + SWITCH_GPU_BACKING_PROMISES] = {};
        const uint32_t n = switch_native_scanout_dependencies(read, own, 3 + SWITCH_GPU_BACKING_PROMISES);
        for (uint32_t t = 0; t < n; ++t) {
            if (!own[t].engine || switch_gpu_backing_token_external_status(&own[t]) == 1)
                continue;
            media = media && (own[t].engine == SWITCH_GPU_ENGINE_VIC || own[t].engine == SWITCH_GPU_ENGINE_NVDEC);
            fenceTokens[count++] = own[t];
        }
    }
    fenceCount = count;
    return count;
}

// The windows are written once every producer of every surface they will scan has finished: at once when none is
// pending, on the media engines' syncpoints when only VIC and NVDEC write them (switch_host1x_token_fence), otherwise
// through a GPU fence. The GPU fence may sleep for a slot and a VIC conversion submits a job, so both go to the fence
// thread; the work loop leaves the frame's slots alone until its completion.
void SwitchExternalScreen::fenceFrame()
{
    fenceConverts = false;
    for (unsigned w = 0; w < kWindows; ++w) {
        fenceSlots[w] = drawing[w];
        fenceConverts = fenceConverts || (drawing[w] >= 0 && slots[drawing[w]].convertPending);
    }
    gpuPending = true;
    if (!fenceConverts) {
        bool media = false;
        const uint32_t count = gatherFence(media);
        if (!count) {
            gpuPending = false;
            latchFrame();
            return;
        }
        if (media && count <= SWITCH_HOST1X_TOKEN_FENCE_MAX) {
            const int result = switch_host1x_token_fence(fenceTokens, count, fenced, this);
            if (!result)
                return;
            if (result != -16 && result != -22) {
                gpuPending = false;
#if DEVELOPMENT || DEBUG
                if (diagScanoutFailures < 8u) {
                    ++diagScanoutFailures;
                    IOLog("SwitchDisplayPort: switch_host1x_token_fence failed err=%d tokens=%u\n", result, count);
                }
#endif
                abandonDrawing();
                const auto *request = pending;
                pending = nullptr;
                dropFrame(request);
                return;
            }
        }
    }
    fenceSource->interruptOccurred(nullptr, nullptr, 0);
}

// A conversion VIC refuses retires the swap unshown with this line, the screen keeping its frame; one waiting for the GPU
// (-16, STALE) is tried again when it is ready, as prepareSlot's are.
void SwitchExternalScreen::fenceFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    if (self->fenceConverts) {
        if (const int error = self->runConversions()) {
            if (!retryable(error) && self->unshownErrors++ < 8u)
                IOLog("SwitchDisplayPort: VIC refused a layer's conversion (%d); the swap is not shown\n", error);
            self->gpuComplete(error);
            return;
        }
        bool media = false;
        const uint32_t count = self->gatherFence(media);
        if (!count) {
            self->gpuComplete(0);
            return;
        }
        if (media && count <= SWITCH_HOST1X_TOKEN_FENCE_MAX) {
            const int result = switch_host1x_token_fence(self->fenceTokens, count, fenced, self);
            if (!result)
                return;
            if (result != -16 && result != -22) {
                self->gpuComplete(result);
                return;
            }
        }
    }
    int error = 0;
    for (;;) {
        error = switch_gpu_bridge_fence(self->fenceTokens, self->fenceCount, fenced, self);
        if (error != -16)
            break;
        error = switch_gpu_bridge_wait_room();
        if (error)
            break;
    }
    if (error)
        self->gpuComplete(error);
}

void SwitchExternalScreen::gpuComplete(int status)
{
    __atomic_store_n(&gpuStatus, status, __ATOMIC_RELEASE);
    completion->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchExternalScreen::completionFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    self->gpuPending = false;
    const int status = __atomic_load_n(&self->gpuStatus, __ATOMIC_ACQUIRE);
    if (!self->dcb || self->gpuGeneration != self->linkGeneration || status != 0) {
#if DEVELOPMENT || DEBUG
        if (status != 0 && self->diagScanoutFailures < 8u) {
            ++self->diagScanoutFailures;
            IOLog("SwitchDisplayPort: gpuComplete failed status=%d\n", status);
        }
#endif
        const bool gone = !self->connected || self->gpuGeneration != self->linkGeneration;
        for (unsigned w = 0; w < kWindows; ++w) {
            const int k = self->drawing[w];
            self->drawing[w] = -1;
            if (k < 0 || self->slotShown(k))
                continue;
            if (gone)
                self->releaseSlot(static_cast<unsigned>(k));
            else
                self->releaseRead(static_cast<unsigned>(k));
        }
        const bool showing = self->connected && self->selectedMode >= 0 && self->device == 2;
        if (showing && !gone && self->dcb && retryable(status)) {
            self->pending = nullptr;
            self->retryWhenReady();
            return;
        }
        self->retire(self->pending, false);
        self->pending = nullptr;
        if (!showing)
            self->drainSwapQueue();
        else
            self->kickSwapQueue();
        return;
    }
    self->latchFrame();
}

void SwitchExternalScreen::latchFrame()
{
    dcb->present(pendingFrame);
    latched = true;
}

// The next frame is not shown: its reads go back, the frame on the screen stays.
void SwitchExternalScreen::abandonDrawing()
{
    int taken[kWindows];
    for (unsigned w = 0; w < kWindows; ++w) {
        taken[w] = drawing[w];
        drawing[w] = -1;
    }
    for (const int k : taken)
        if (k >= 0 && !slotShown(k))
            releaseRead(static_cast<unsigned>(k));
}

// The next frame is on the screen: the reads of what it replaced go back.
void SwitchExternalScreen::promoteDrawing()
{
    int replaced[kWindows];
    for (unsigned w = 0; w < kWindows; ++w) {
        replaced[w] = front[w];
        front[w] = drawing[w];
        drawing[w] = -1;
    }
    for (const int k : replaced)
        if (k >= 0 && !slotShown(k))
            releaseRead(static_cast<unsigned>(k));
    for (const int k : front)
        if (k >= 0 && slots[k].nextKey.valid) {
            slots[k].key = slots[k].nextKey;
            slots[k].nextKey = {};
        }
}

void SwitchExternalScreen::dropFrame(const OriginalLegacySwapRequest *request)
{
    if (retire(request, false) && pendingSwapRequest())
        readySource->interruptOccurred(nullptr, nullptr, 0);
}

bool SwitchExternalScreen::retire(const OriginalLegacySwapRequest *request, bool shown)
{
    if (!activateOnlyPendingSwap(request))
        return false;
    switch_external_complete_current(this, shown);
    return true;
}

bool SwitchExternalScreen::frameEnd()
{
    if (!dcb)
        return true;
    if (latched && dcb->latch()) {
        latched = false;
        promoteDrawing();
        retire(pending, true);
        pending = nullptr;
        for (const int k : front)
            if (k >= 0)
                switch_native_scanout_deliver(&slots[k].read);
    }
    const auto *clients = switch_abi_load<const void *>(this, vsyncClientList);
    if (clients)
        native_legacy_vsync_notify(this, mach_absolute_time(), mach_continuous_time(), interval);
    kickSwapQueue();
    if (dcb)
        dcb->frameInterrupt(true);
    return true;
}

#if DEVELOPMENT || DEBUG
// One line for each layer shape a clone swap brings: the layers, their formats, layouts and rectangles, and the windows.
void SwitchExternalScreen::noteShape(uint32_t updated, const native_planes::Layer layers[], const bool present[],
                                     const switch_external::Plan &plan, const int slotOf[])
{
    uint64_t key = 1469598103934665603ULL;
    const auto mix = [&key](uint64_t value) {
        key = (key ^ value) * 1099511628211ULL;
    };
    int blockLog2[kLayers] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!present[i])
            continue;
        const auto &l = layers[i];
        if (const tgm_layout *block = slotOf[i] >= 0 ? slotBlock(slots[slotOf[i]]) : nullptr)
            blockLog2[i] = block->y_log2;
        mix(i);
        mix(l.surface.format);
        mix(static_cast<uint64_t>(l.surface.width) << 32 | l.surface.height);
        mix(uint64_t(blockLog2[i] + 1));
        mix(static_cast<uint64_t>(uint32_t(l.src.x)) << 32 | uint32_t(l.src.y));
        mix(static_cast<uint64_t>(uint32_t(l.src.w)) << 32 | uint32_t(l.src.h));
        mix(static_cast<uint64_t>(uint32_t(l.dst.x)) << 32 | uint32_t(l.dst.y));
        mix(static_cast<uint64_t>(uint32_t(l.dst.w)) << 32 | uint32_t(l.dst.h));
    }
    for (const auto &p : plan.window)
        mix(p.on ? static_cast<uint64_t>(p.layer) << 8 | p.depth : 0xffu);
    mix(plan.unshown);
    mix(plan.convert);
    if (key == diagShape || diagShapeLogs >= 32u)
        return;
    diagShape = key;
    ++diagShapeLogs;
    char line[640];
    int at = snprintf(line, sizeof(line), "updated %#x, windows A:%d B:%d C:%d, converted %#x, unshown %#x", updated,
                      plan.window[0].on ? plan.window[0].layer : -1, plan.window[1].on ? plan.window[1].layer : -1,
                      plan.window[2].on ? plan.window[2].layer : -1, plan.convert, plan.unshown);
    for (unsigned i = 0; i < kLayers && at > 0 && static_cast<size_t>(at) < sizeof(line); ++i) {
        if (!present[i])
            continue;
        const auto &l = layers[i];
        const uint32_t f = l.surface.format;
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                       "; layer %u '%c%c%c%c' %ux%u %s %d src (%d,%d %dx%d) dst (%d,%d %dx%d)", i,
                       static_cast<char>(f >> 24), static_cast<char>(f >> 16), static_cast<char>(f >> 8),
                       static_cast<char>(f), l.surface.width, l.surface.height, blockLog2[i] < 0 ? "rows" : "block",
                       blockLog2[i], l.src.x, l.src.y, l.src.w, l.src.h, l.dst.x, l.dst.y, l.dst.w, l.dst.h);
    }
    IOLog("SwitchDisplayPort: clone swap %s\n", line);
}
#endif

// A swap retired unshown, the screen keeping its frame, for one layer: eight lines a link. why: -2 its inherited
// surface has no record, otherwise the error mapping or converting it.
void SwitchExternalScreen::noteUnshown(unsigned layer, int why)
{
    if (unshownErrors >= 8u)
        return;
    ++unshownErrors;
    if (why == -2) {
        IOLog("SwitchDisplayPort: swap not shown: layer %u's inherited surface has no record\n", layer);
        return;
    }
#if DEVELOPMENT || DEBUG
    const auto &l = layerRecords[layer].layer;
    const uint32_t f = l.surface.format;
    IOLog("SwitchDisplayPort: swap not shown: layer %u (%d) '%c%c%c%c' %ux%u planes %u strides %u/%u src (%d,%d "
          "%dx%d) dst (%d,%d %dx%d)\n",
          layer, why, static_cast<char>(f >> 24), static_cast<char>(f >> 16), static_cast<char>(f >> 8),
          static_cast<char>(f), l.surface.width, l.surface.height, l.surface.planes, l.surface.stride[0],
          l.surface.stride[1], l.src.x, l.src.y, l.src.w, l.src.h, l.dst.x, l.dst.y, l.dst.w, l.dst.h);
#else
    IOLog("SwitchDisplayPort: swap not shown: layer %u (%d)\n", layer, why);
#endif
}
