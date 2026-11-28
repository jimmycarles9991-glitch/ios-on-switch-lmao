// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include "OriginalLegacyFramebufferABI.h"
#include "SwitchDPModes.h"
#include "SwitchExternalPlanes.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "../TegraGM20B/SwitchGpuReady.h"
#include "../TegraVIC/SwitchNativeScanout.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

class SwitchDisplayPort;
class TegraDisplayB;

class SwitchExternalScreen : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(SwitchExternalScreen);

public:
    void stop(IOService *provider) override;
    void free() override;
    bool begin(SwitchDisplayPort *transport, IOWorkLoop *workLoop);
    bool publish(const struct switch_dp_mode_list &modes, const uint8_t *edid, unsigned bytes);
    void linkUp(const struct switch_dp_mode &mode, TegraDisplayB *controller);
    void linkDown();
    void modeFailed();
    void detach();
    bool frameEnd(uint64_t stamp = 0, uint64_t continuous = 0);
    void gpuComplete(int status);

    bool requested() const
    {
        return device == 2;
    }

    IOReturn setPowerState(unsigned long ordinal, IOService *whatDevice) override;

    bool isMainDisplay() override
    {
        return false;
    }

    bool startHardware() override
    {
        return true;
    }

    bool getRefreshAttributes(uint64_t *periods, uint32_t internal) override;
    IOReturn getDotPitch(uint32_t *pitch) override;
    IOReturn setDisplayDevice(uint32_t requested) override;
    IOReturn setDigitalOutMode(uint32_t color, uint32_t timing) override;
    IOReturn getDigitalOutState(uint32_t *state) override;
    IOReturn getMatrix(uint32_t location, void *matrix) override;
    IOReturn setMatrix(uint32_t location, const void *matrix) override;
    IOReturn swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t flags) override;
    IOReturn setAccessibilityScale(uint32_t scale) override;
    void kickSwapQueue() override;
    bool validateSwap(const OriginalLegacySwapRequest *request) override;

    bool areTimingsEnabled() const override
    {
        return dcb != nullptr;
    }

    IOReturn collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t) override;
    IOReturn supportedFrameInfo(uint32_t, uint32_t *count, char *, uint32_t *) override;

    IOReturn changeFrameInfo(uint32_t, void *, uint32_t) override
    {
        return kIOReturnUnsupported;
    }

    void platformFrameInfoNotify(void *, bool) override
    {}

    IOReturn sendFrameInfo(void *, const void *) override
    {
        return kIOReturnUnsupported;
    }

    IOReturn platformInternalPlug() override
    {
        return kIOReturnUnsupported;
    }

    IOReturn platformInternalUnplug() override
    {
        return kIOReturnUnsupported;
    }

    void platformRefreshRates() override
    {}

private:
    static constexpr unsigned kLayers = native_planes::kLayers, kWindows = switch_external::kWindows;
    // Every window's surface on the screen and the next frame's, and a spare for each layer to stay mapped across frames.
    // A converted layer takes one slot for VIC's output and none for its source, which VIC maps itself.
    static constexpr unsigned kSlots = 8;
    static_assert(kSlots >= 2 * kWindows);
    static constexpr uint64_t kSlotBase = 0x10000000, kSlotSpan = 68ULL << 20;
    static constexpr uint32_t kTablePages = 1 + kSlots * (kSlotSpan >> 22);
    static_assert(kTablePages == 137);
    static_assert(kTablePages == 1 + TegraSMMUDomain::tablesFor(kSlotBase, uint64_t{kSlots} * kSlotSpan));
    static_assert(kSlotBase + uint64_t{kSlots} * kSlotSpan <= (1ULL << 32));
    static constexpr unsigned kFenceTokens = kWindows * (3 + SWITCH_GPU_BACKING_PROMISES);

    // What a converted slot holds: the source picture and how VIC converted it. A slot's key is set at the latch that
    // shows its frame, so a buffer whose job failed or never ran is never taken for a picture.
    struct ConversionKey {
        uint64_t allocation, generation, content, offset;
        uint32_t format, width, height, divisor, matrix, chromaTop, chromaBottom, valid;
    };

    struct Slot {
        SwitchNativeScanout read;
        SwitchNativeScanout source;  // a conversion's: what VIC reads, kept with the read of its output
        OSObject *surface;           // the IOSurface scanned; none for a conversion's output
        IOMemoryDescriptor *memory;
        switch_gpu_backing *backing;  // the surface's, or the conversion's own buffer
        void *hold;
        tgm_layout block;  // bpp 0: the surface is in rows
        TegraVICImage convertInput;
        TegraVICOp convertOp;
        ConversionKey key, nextKey;
        uint64_t allocationId, generation, offset, used, allocationBytes;
        uint32_t pages, convertWidth, convertHeight, convertPitch;
        bool counted, conversion, convertPending;
    };

    Slot *slots = nullptr;  // kSlots of them, allocated at begin: the object stays within the kernel's 16 KiB

    struct SlotPaCache {
        uint32_t seq;
        const IOMemoryDescriptor *memory;
        addr64_t firstPa;
    } slotPaCache[kSlots] = {};

    static constexpr unsigned kValidatedCache = 4;

    struct ValidatedCacheEntry {
        uint32_t seq;
        uint32_t swapIdValue;
        const OriginalLegacySwapRequest *request;
        uint32_t updated;
        OSObject *surfaces[kLayers];
        native_planes::Layer layers[kLayers];
    };

    ValidatedCacheEntry *validatedCache = nullptr;

    // Each layer as the swaps handed over so far gave it (a layer a swap does not update keeps its surface, retained, and
    // geometry); kept across a link drop, cleared at a detach or a change of geometry.
    struct LayerRecord {
        OSObject *surface;
        native_planes::Layer layer;
    } layerRecords[kLayers] = {};

    SwitchDisplayPort *transport = nullptr;
    IOWorkLoop *loop = nullptr;
    IOLock *colorLock = nullptr;
    IOInterruptEventSource *completion = nullptr, *readySource = nullptr;
    thread_call_t fenceCall = nullptr;
    bool fenceStopping = false;
    switch_gpu_ready_watch *ready = nullptr;
    TegraDisplayB *dcb = nullptr;
    switch_dma_buffer tables = {};
    TegraSMMUDomain domain;
    struct switch_dp_mode_list modes = {};
    struct switch_dp_mode mode = {};
    switch_external::Frame pendingFrame = {};
    // The fence thread's work, fixed on the work loop before it is signalled: the next frame's slots, whether VIC converts
    // into some of them first, and the producers a GPU fence waits for.
    switch_gpu_backing_token fenceTokens[kFenceTokens] = {};
    uint32_t fenceCount = 0, unshownErrors = 0;
    int fenceSlots[kWindows] = {-1, -1, -1};
    bool fenceConverts = false;
    int front[kWindows] = {-1, -1, -1}, drawing[kWindows] = {-1, -1, -1};
    int64_t harmonyMatrix[9] = {};
    int64_t accessibilityMatrix[9] = {};
    uint64_t slotUses = 0, refreshPeriod = 0, interval = 0, linkGeneration = 0, gpuGeneration = 0;
    uint64_t activeGeometry = 0, selectionGeneration = 0;
    int selectedMode = -1;
    uint32_t device = 0;
    bool logicalPM = false, connected = false, mapped = false, gpuPending = false, latched = false, kicking = false;
    const OriginalLegacySwapRequest *pending = nullptr;
    int gpuStatus = 0;
#if DEVELOPMENT || DEBUG
    uint32_t diagValidateRejects = 0;
    uint32_t diagScanoutFailures = 0;
    uint32_t diagShapeLogs = 0;
    uint64_t diagShape = 0;
    void noteShape(uint32_t updated, const native_planes::Layer layers[], const bool present[],
                   const switch_external::Plan &plan, const int slotOf[]);
#endif
    void noteUnshown(unsigned layer, int why);
    static void completionFired(OSObject *owner, IOInterruptEventSource *, int);
    void fenceWork();
    void failFence(int result);
    static native_planes::Layer layerOf(const uint8_t *request, unsigned layer, OSObject *surface);
    void publishSlotPa(unsigned index, const IOMemoryDescriptor *memory, addr64_t firstPa);
    addr64_t cachedFirstPa(const IOMemoryDescriptor *memory) const;
    void cacheValidatedLayers(const OriginalLegacySwapRequest *request, uint32_t id, uint32_t updated,
                              const bool hasSurface[], OSObject *const surfaces[], const native_planes::Layer layers[]);
    bool lookupValidatedLayers(const OriginalLegacySwapRequest *request, uint32_t id, uint32_t updated,
                               OSObject *surfaces[], native_planes::Layer layers[]) const;
    void releaseEventSources();
    void publishActiveGeometry();
    void drainSwapQueue();
    void clearLayerState();
    bool applyLayout(Slot &slot, uint64_t firstPage, uint64_t bytes);
    bool mapDomain();
    void cleanDomain();
    void releaseSlot(unsigned slot);
    void releaseRead(unsigned slot);
    bool slotShown(int slot) const;
    int prepareSlot(OSObject *surface, uint32_t building);
    static const tgm_layout *slotBlock(const Slot &slot);
    void recordLayers(const uint8_t *request);
    bool takeLayers(const uint8_t *request, bool present[], native_planes::Layer layers[], OSObject *surfaces[]);
    void releaseUnshown(uint32_t building);
    bool betterVictim(unsigned i, int victim, uint64_t bytes) const;
    int prepareConversion(OSObject *surface, unsigned layer, uint32_t building);
    int runConversions();
    void unshownFrame(int error, unsigned layer, uint32_t building, const OriginalLegacySwapRequest *request);
    [[noreturn]] void unscannable(unsigned layer) const;
    uint32_t gatherFence(bool &media);
    void abandonDrawing();
    void promoteDrawing();
    void fenceFrame();
    void latchFrame();
    bool retire(const OriginalLegacySwapRequest *request, bool shown);
    void dropFrame(const OriginalLegacySwapRequest *request);
    void requestFrame();
    void retryWhenReady();
    static int readyQuery(void *);
    static void readyFired(OSObject *, IOInterruptEventSource *, int);
    bool publishModes(OSString *uuid);
    void publishScanoutLimits();
    bool publishSourceLimits(uint32_t clockHz);
    void clearSinkProperties();
    IOReturn selectOutput(bool timing, uint32_t value);
#include "../NativeMobileDisplay/NativeLegacySwapQueue.h"
};
