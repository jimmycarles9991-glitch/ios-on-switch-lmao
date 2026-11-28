// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchExternalScreen.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <mach/mach_time.h>
#include "SwitchDPProperties.h"
#include "SwitchDisplayPortService.h"
#include "TegraDisplayB.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"
#include "../NativeMobileDisplay/SwitchSurfaceUse.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);

OSDefineMetaClassAndStructors(SwitchExternalScreen, IOMobileFramebufferLegacy);

namespace {
constexpr uint32_t kLocationAccessibility = 2u, kLocationHarmony = 9u;
constexpr int64_t kMatrixOne = static_cast<int64_t>(1ULL << 32);

void setIdentityMatrix(int64_t matrix[9])
{
    memset(matrix, 0, sizeof(int64_t) * 9);
    matrix[0] = matrix[4] = matrix[8] = kMatrixOne;
}

uint64_t periodTicks(uint64_t period)
{
    const uint64_t ns = (period >> 32) * 1000000000u + ((uint64_t(uint32_t(period)) * 1000000000u) >> 32);
    uint64_t ticks;
    nanoseconds_to_absolutetime(ns, &ticks);
    return ticks;
}

}

void SwitchExternalScreen::releaseEventSources()
{
    __atomic_store_n(&fenceStopping, true, __ATOMIC_RELEASE);
    if (fenceCall) {
        if (thread_call_cancel_wait(fenceCall))
            release();
        thread_call_free(fenceCall);
        fenceCall = nullptr;
    }
    if (loop) {
        if (readySource)
            loop->removeEventSource(readySource);
        if (completion)
            loop->removeEventSource(completion);
    }
    OSSafeReleaseNULL(readySource);
    OSSafeReleaseNULL(completion);
    loop = nullptr;
}

void SwitchExternalScreen::stop(IOService *provider)
{
    if (connected) {
        connected = false;
        tegra_gm20b_set_external_scanout(0);
    }
    if (logicalPM) {
        PMstop();
        logicalPM = false;
    }
    releaseEventSources();
    IOMobileFramebufferLegacy::stop(provider);
}

void SwitchExternalScreen::free()
{
    if (connected) {
        connected = false;
        tegra_gm20b_set_external_scanout(0);
    }
    if (logicalPM) {
        PMstop();
        logicalPM = false;
    }
    releaseEventSources();
    if (colorLock) {
        IOLockFree(colorLock);
        colorLock = nullptr;
    }
    clearLayerState();
    if (slots) {
        for (unsigned i = 0; i < kSlots; ++i)
            releaseSlot(i);
        IODelete(slots, Slot, kSlots);
        slots = nullptr;
    }
    if (validatedCache) {
        IODelete(validatedCache, ValidatedCacheEntry, kValidatedCache);
        validatedCache = nullptr;
    }
    if (!mapped)
        switch_dma_free(&tables);
    IOMobileFramebufferLegacy::free();
}

bool SwitchExternalScreen::begin(SwitchDisplayPort *port, IOWorkLoop *workLoop)
{
    if (!workLoop || workLoop != getWorkLoop())
        return false;
    slots = IONewZero(Slot, kSlots);
    validatedCache = IONewZero(ValidatedCacheEntry, kValidatedCache);
    if (!slots || !validatedCache)
        return false;
    // DC B's page tables are one physically contiguous run, taken at boot: hours in, a dock finds memory too fragmented
    // for a run this long, and every swap would then search for it again.
    if (!switch_dma_alloc(&tables, kTablePages * 4096u) || !domain.init(tables.va, tables.pa, kTablePages) ||
        !domain.reserve(kSlotBase, kSlots * kSlotSpan)) {
        IOLog("SwitchDisplayPort: external scanout: no %u KiB contiguous below 4 GiB for DC B's SMMU tables\n",
              kTablePages * 4u);
        return false;
    }
    transport = port;
    loop = workLoop;
    setIdentityMatrix(harmonyMatrix);
    setIdentityMatrix(accessibilityMatrix);
    colorLock = IOLockAlloc();
    completion = IOInterruptEventSource::interruptEventSource(this, &completionFired);
    readySource = IOInterruptEventSource::interruptEventSource(this, &readyFired);
    fenceStopping = false;
    fenceCall = thread_call_allocate_with_options(
        [](thread_call_param_t owner, thread_call_param_t) {
            auto *self = static_cast<SwitchExternalScreen *>(owner);
            self->fenceWork();
            self->release();
        },
        this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!colorLock || !completion || !readySource || !fenceCall ||
        loop->addEventSource(completion) != kIOReturnSuccess || loop->addEventSource(readySource) != kIOReturnSuccess) {
        releaseEventSources();
        return false;
    }
    auto *node = IORegistryEntry::fromPath("/arm-io/display-external", gIODTPlane);
    auto *nub = OSTypeAlloc(IOPlatformDevice);
    auto *platform = IOService::getPlatform();
    const bool nubReady = node && nub && platform && nub->init(node, gIODTPlane) && nub->attach(platform) &&
                          nub->start(platform);
    OSSafeReleaseNULL(node);
    if (!nubReady) {
        OSSafeReleaseNULL(nub);
        releaseEventSources();
        return false;
    }
    setName("SwitchExternalDisplay");
    setProperty("external", kOSBooleanTrue);
    setProperty("IOMFBDisplayExternal", kOSBooleanTrue);
    setInternalDisplay(false);
    publishScanoutLimits();
    if (!attach(nub) || !IOMobileFramebufferLegacy::start(nub)) {
        IOService::detach(nub);
        OSSafeReleaseNULL(nub);
        releaseEventSources();
        return false;
    }
    static IOPMPowerState states[2] = {};
    states[0].version = states[1].version = 1;
    states[1].capabilityFlags = kIOPMDeviceUsable;
    PMinit();
    logicalPM = initialized;
    if (!logicalPM) {
        IOMobileFramebufferLegacy::stop(nub);
        IOService::detach(nub);
        OSSafeReleaseNULL(nub);
        releaseEventSources();
        return false;
    }
    nub->joinPMtree(this);
    IOReturn status = registerPowerDriver(this, states, 2);
    if (status == kIOReturnSuccess)
        status = changePowerStateToPriv(1);
    if (status != kIOReturnSuccess) {
        PMstop();
        logicalPM = false;
        IOMobileFramebufferLegacy::stop(nub);
        IOService::detach(nub);
        OSSafeReleaseNULL(nub);
        releaseEventSources();
        return false;
    }
    OSSafeReleaseNULL(nub);
    return true;
}

IOReturn SwitchExternalScreen::setPowerState(unsigned long ordinal, IOService *whatDevice)
{
    if (ordinal > 1)
        return kIOReturnUnsupported;
    const IOReturn status = IOMobileFramebufferLegacy::setPowerState(ordinal, whatDevice);
    if (whatDevice != nullptr && status >= 0 && loop != nullptr) {
        loop->runAction(
            [](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
                auto *self = static_cast<SwitchExternalScreen *>(owner);
                const auto state = reinterpret_cast<uintptr_t>(argument);
                if (state == 0) {
                    self->drainSwapQueue();
                } else if (self->dcb != nullptr) {
                    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
                        power = {1, 0, 0, 1, 1, {}};
                    self->setDisplayPowerState(&power, false);
                    self->kickSwapQueue();
                }
                return kIOReturnSuccess;
            },
            this, reinterpret_cast<void *>(static_cast<uintptr_t>(ordinal)));
    }
    return status;
}

namespace {
struct Limit { const char *key; uint64_t value; };
bool publishLimits(IOService *screen, const char *name, const Limit *limits, unsigned count, unsigned bits)
{
    auto *dictionary = OSDictionary::withCapacity(count);
    bool ok = dictionary != nullptr;
    for (unsigned i = 0; ok && i < count; ++i) {
        auto *number = OSNumber::withNumber(limits[i].value, bits);
        ok = number && dictionary->setObject(limits[i].key, number);
        OSSafeReleaseNULL(number);
    }
    if (ok)
        ok = screen->setProperty(name, dictionary);
    OSSafeReleaseNULL(dictionary);
    return ok;
}
}

#if SWITCH_IPSW_KIND_24B119
static_assert(switch_external::kPackedYuvUnsupported, "24B119 copies detached video to packed 10-bit without the key");
#endif

void SwitchExternalScreen::publishScanoutLimits()
{
    const auto &published = switch_external::kPublished;
    constexpr unsigned count = sizeof(published) / sizeof(published[0]);
    for (unsigned first = 0; first < count;) {
        unsigned end = first + 1;
        while (end < count && !strcmp(published[end].dictionary, published[first].dictionary))
            ++end;
        auto *dictionary = OSDictionary::withCapacity(end - first);
        bool ok = dictionary != nullptr;
        for (unsigned i = first; ok && i < end; ++i) {
            auto *number = OSNumber::withNumber(published[i].value, published[i].bits);
            ok = number && dictionary->setObject(published[i].key, number);
            OSSafeReleaseNULL(number);
        }
        if (ok)
            setProperty(published[first].dictionary, dictionary);
        OSSafeReleaseNULL(dictionary);
        first = end;
    }
    if (switch_external::kPackedYuvUnsupported)
        setProperty("IOMFBYUVPackedUnSupported", true);
    setProperty("IOMFBDisableFlip", true);
    publishSourceLimits(SWITCH_DP_MAX_PIXEL_KHZ * 1000u);
}

bool SwitchExternalScreen::publishSourceLimits(uint32_t clockHz)
{
    const Limit source[] = {{"MaxSrcRectWidth", SWITCH_DP_MAX_WIDTH},
                            {"MaxSrcRectTotal", uint64_t{SWITCH_DP_MAX_WIDTH} * SWITCH_DP_MAX_HEIGHT},
                            {"MaxSrcBufferWidth", SWITCH_DP_MAX_WIDTH}, {"MaxSrcBufferHeight", SWITCH_DP_MAX_HEIGHT},
                            {"MaxVideoSrcDownscalingWidth", SWITCH_DP_MAX_WIDTH}, {"VideoClock", clockHz},
                            {"PixelClock", clockHz}, {"IOMFBCompressionSupport", 0}};
    return publishLimits(this, "IOMFBMaxSrcPixels", source, 8, 64);
}

bool SwitchExternalScreen::publishModes(OSString *uuid)
{
    auto *timings = OSArray::withCapacity(modes.count);
    auto *preferred = OSArray::withCapacity(1);
    auto *colors = OSArray::withCapacity(1);
    auto *color = switch_dp_color_element();
    bool ok = timings && preferred && colors && color && colors->setObject(color);
    unsigned best = 0;
    for (unsigned i = 1; i < modes.count; ++i)
        if (switch_dp_prefer(&modes.modes[i], &modes.modes[best]))
            best = i;
    uint32_t clockKHz = 0;
    for (unsigned i = 0; ok && i < modes.count; ++i) {
        modes.modes[i].preferred = i == best;
        auto *timing = switch_dp_timing_element(&modes.modes[i], i + 1, colors);
        ok = timing && timings->setObject(timing);
        if (ok && i == best)
            ok = preferred->setObject(timing);
        if (clockKHz < modes.modes[i].pixel_khz)
            clockKHz = modes.modes[i].pixel_khz;
        OSSafeReleaseNULL(timing);
    }
    auto *wire = OSDictionary::withCapacity(1);
    auto *name = OSString::withCString("DP");
    ok = ok && timings->getCount() && wire && name && wire->setObject("Downstream", name);
    ok = ok && publishSourceLimits(clockKHz * 1000u) && setProperty("TimingElements", timings) &&
         setProperty("PreferredTimingElements", preferred) && setProperty("ColorElements", colors) &&
         setProperty("Transport", wire) && setProperty("IOMFBUUID", uuid);
    if (!ok)
        IOLog("SwitchDisplayPort: no publishable external display timings\n");
    OSSafeReleaseNULL(name);
    OSSafeReleaseNULL(wire);
    OSSafeReleaseNULL(color);
    OSSafeReleaseNULL(colors);
    OSSafeReleaseNULL(preferred);
    OSSafeReleaseNULL(timings);
    return ok;
}

void SwitchExternalScreen::publishActiveGeometry()
{
    uint64_t geometry = 0;
    if (connected && device == 2 && selectedMode >= 0 && static_cast<unsigned>(selectedMode) < modes.count) {
        const auto &selected = modes.modes[selectedMode];
        geometry = (static_cast<uint64_t>(selected.hactive) << 32) | selected.vactive;
    }
    __atomic_store_n(&activeGeometry, geometry, __ATOMIC_RELEASE);
}

void SwitchExternalScreen::drainSwapQueue()
{
    if (!gpuPending) {
        for (const auto *request = pendingSwapRequest(); request; request = pendingSwapRequest()) {
            // Unplugged, the records would hold the clone's surfaces until free(): detach cleared them.
            if (connected)
                recordLayers(reinterpret_cast<const uint8_t *>(request));
            if (!retire(request, false))
                break;
        }
    }
}

bool SwitchExternalScreen::publish(const struct switch_dp_mode_list &list, const uint8_t *edid, unsigned bytes)
{
    selectedMode = -1;
    publishActiveGeometry();
    auto *uuid = switch_dp_edid_uuid(edid);
    modes = list;
    const bool published = uuid && publishModes(uuid) && setProperty("EDID", const_cast<uint8_t *>(edid), bytes);
    OSSafeReleaseNULL(uuid);
    if (!published) {
        connected = false;
        publishActiveGeometry();
        clearSinkProperties();
        return false;
    }
    connected = true;
    publishActiveGeometry();
    unsigned best = 0;
    for (unsigned i = 0; i < modes.count; ++i)
        if (modes.modes[i].preferred) {
            best = i;
            break;
        }
    if (modes.count) {
        refreshPeriod = switch_dp_refresh_period(&modes.modes[best]);
        interval = periodTicks(refreshPeriod);
        if (!native_legacy_set_display_refresh_properties(this))
            IOLog("SwitchDisplayPort: could not publish the preferred refresh interval\n");
    }
    tegra_gm20b_set_external_scanout(1);
    setProperty("IOMFBHotplugKeysChangedKey", kOSBooleanTrue);
    IOMobileFramebufferLegacy::hotPlugNotify(1);
    return true;
}

void SwitchExternalScreen::clearLayerState()
{
    for (auto &record : layerRecords) {
        OSSafeReleaseNULL(record.surface);
        record = {};
    }
}

void SwitchExternalScreen::linkUp(const struct switch_dp_mode &timing, TegraDisplayB *controller)
{
    if (mode.hactive != timing.hactive || mode.vactive != timing.vactive) {
        for (unsigned i = 0; i < kSlots; ++i)
            if (slots[i].surface && !slotShown(static_cast<int>(i)))
                releaseSlot(i);
        clearLayerState();
    }
    unshownErrors = 0;
#if DEVELOPMENT || DEBUG
    __atomic_store_n(&diagValidateRejects, 0u, __ATOMIC_RELAXED);
    diagScanoutFailures = 0;
    diagShapeLogs = 0;
    diagShape = 0;
#endif
    mode = timing;
    dcb = controller;
    setDisplayWidth(mode.hactive);
    setDisplayHeight(mode.vactive);
    refreshPeriod = switch_dp_refresh_period(&mode);
    interval = periodTicks(refreshPeriod);
    if (!native_legacy_set_display_refresh_properties(this))
        IOLog("SwitchDisplayPort: could not publish the selected refresh interval\n");
    dcb->frameInterrupt(true);
    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
        power = {1, 0, 0, 1, 1, {}};
    setDisplayPowerState(&power, false);
    kickSwapQueue();
}

void SwitchExternalScreen::linkDown()
{
    if (!dcb)
        return;
    dcb = nullptr;
    ++linkGeneration;
    switch_gpu_ready_cancel(&ready);
    latched = false;
    for (auto &shown : front) {
        const int k = shown;
        shown = -1;
        if (k >= 0 && !slotShown(k))
            releaseRead(static_cast<unsigned>(k));
    }
    if (!gpuPending) {
        abandonDrawing();
        retire(pending, false);
        pending = nullptr;
    }
    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
        power = {1, 0, 0, 0, 1, {}};
    setDisplayPowerState(&power, false);
}

void SwitchExternalScreen::detach()
{
    for (auto &shown : front)
        shown = -1;
    for (unsigned i = 0; i < kSlots; ++i)
        if (!gpuPending || !slotShown(static_cast<int>(i)))
            releaseSlot(i);
    clearLayerState();
    const bool wasConnected = connected;
    ++selectionGeneration;
    connected = false;
    selectedMode = -1;
    device = 0;
    refreshPeriod = 0;
    interval = 0;
    setDisplayWidth(0);
    setDisplayHeight(0);
    publishActiveGeometry();
    clearSinkProperties();
    drainSwapQueue();
    if (wasConnected) {
        IOMobileFramebufferLegacy::hotPlugNotify(0);
        tegra_gm20b_set_external_scanout(0);
    }
}

void SwitchExternalScreen::modeFailed()
{
    selectedMode = -1;
    publishActiveGeometry();
    drainSwapQueue();
}

void SwitchExternalScreen::clearSinkProperties()
{
    removeProperty("IOMFBDisplayRefresh");
    removeProperty("IOMFBHotplugKeysChangedKey");
    removeProperty("TimingElements");
    removeProperty("PreferredTimingElements");
    removeProperty("ColorElements");
    removeProperty("Transport");
    removeProperty("IOMFBUUID");
    removeProperty("EDID");
    publishSourceLimits(SWITCH_DP_MAX_PIXEL_KHZ * 1000u);
}

bool SwitchExternalScreen::getRefreshAttributes(uint64_t *periods, uint32_t internal)
{
    if (!refreshPeriod)
        return IOMobileFramebufferLegacy::getRefreshAttributes(periods, internal);
    constexpr uint64_t externalLongest = (1ull << 32) / 24;
    periods[0] = refreshPeriod;
    periods[1] = interval;
    periods[2] = internal || refreshPeriod >= externalLongest ? refreshPeriod : externalLongest;
    periods[3] = periods[2] == refreshPeriod ? interval : periodTicks(externalLongest);
    periods[4] = periods[5] = 0;
    return true;
}

IOReturn SwitchExternalScreen::getDotPitch(uint32_t *pitch)
{
    if (!pitch)
        return kIOReturnBadArgument;
    *pitch = 0;
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::getDigitalOutState(uint32_t *state)
{
    if (!state)
        return kIOReturnBadArgument;
    *state = connected ? 1 : 0;
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::getMatrix(uint32_t location, void *matrix)
{
    if (!matrix || (location != kLocationHarmony && location != kLocationAccessibility))
        return kIOReturnBadArgument;
    if (!colorLock)
        return kIOReturnNotReady;
    IOLockLock(colorLock);
    memcpy(matrix, location == kLocationHarmony ? harmonyMatrix : accessibilityMatrix, sizeof(harmonyMatrix));
    IOLockUnlock(colorLock);
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::setMatrix(uint32_t location, const void *matrix)
{
    if (!matrix || (location != kLocationHarmony && location != kLocationAccessibility))
        return kIOReturnBadArgument;
    if (!colorLock)
        return kIOReturnNotReady;
    int64_t value[9];
    memcpy(value, matrix, sizeof(value));
    IOLockLock(colorLock);
    memcpy(location == kLocationHarmony ? harmonyMatrix : accessibilityMatrix, value, sizeof(value));
    IOLockUnlock(colorLock);
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t)
{
    if (location != kLocationAccessibility)
        return kIOReturnBadArgument;
    if (!colorLock)
        return kIOReturnNotReady;
    int64_t value[9];
    setIdentityMatrix(value);
    if (matrix) {
        int64_t words[15];
        memcpy(words, matrix, sizeof(words));
        if (words[0] || words[1] || words[2] || words[12] || words[13] || words[14])
            return kIOReturnUnsupported;
        memcpy(value, &words[3], sizeof(value));
    }
    IOLockLock(colorLock);
    memcpy(accessibilityMatrix, value, sizeof(value));
    IOLockUnlock(colorLock);
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::setAccessibilityScale(uint32_t)
{
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::setDisplayDevice(uint32_t requested)
{
    if (requested > 2)
        return kIOReturnBadArgument;
    return selectOutput(false, requested);
}

IOReturn SwitchExternalScreen::setDigitalOutMode(uint32_t color, uint32_t timing)
{
    if (color != 1 || !timing)
        return kIOReturnUnsupported;
    return selectOutput(true, timing - 1);
}

IOReturn SwitchExternalScreen::selectOutput(bool timing, uint32_t value)
{
    struct Selection { bool timing; uint32_t value; } selection = {timing, value};
    return loop->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<SwitchExternalScreen *>(owner);
        const auto &s = *static_cast<Selection *>(argument);
        if (s.timing && (!self->connected || s.value >= self->modes.count))
            return kIOReturnUnsupported;
        const int index = s.timing ? static_cast<int>(s.value) : self->selectedMode;
        const uint32_t device = s.timing ? self->device : s.value;
        if (device == 2 && !self->connected)
            return kIOReturnNotReady;
        const int prevMode = self->selectedMode;
        const uint32_t prevDevice = self->device;
        uint32_t prevWidth = 0, prevHeight = 0;
        self->getDisplayDimensions(&prevWidth, &prevHeight);
        const uint64_t selGen = ++self->selectionGeneration;
        self->selectedMode = index;
        self->device = device;
        if (device == 2 && index >= 0) {
            self->setDisplayWidth(self->modes.modes[index].hactive);
            self->setDisplayHeight(self->modes.modes[index].vactive);
        }
        self->publishActiveGeometry();
        const auto result = index < 0 && device == 2
                                ? kIOReturnSuccess
                                : self->transport->selectMode(index, device == 2);
        if (result == kIOReturnSuccess) {
            if (self->selectionGeneration == selGen) {
                if (device != 2)
                    self->drainSwapQueue();
            }
        } else if (self->selectionGeneration == selGen) {
            self->device = prevDevice;
            if (!self->dcb) {
                self->selectedMode = -1;
                self->setDisplayWidth(0);
                self->setDisplayHeight(0);
            } else {
                self->selectedMode = prevMode;
                self->setDisplayWidth(prevWidth);
                self->setDisplayHeight(prevHeight);
            }
            self->publishActiveGeometry();
            if (!self->dcb)
                self->drainSwapQueue();
        }
        return result;
    }, this, &selection);
}

// Refuses only what the published limits keep out (Core Animation sends a refused swap again every frame): a layer no
// window takes (SwitchExternalPlanes.h checkSwap) and a compressed surface (IOMFBMaxSrcPixels' IOMFBCompressionSupport
// is 0). A surface whose memory cannot be wired here is left to the frame, which does not show it.
bool SwitchExternalScreen::validateSwap(const OriginalLegacySwapRequest *request)
{
    const uint64_t geometry = __atomic_load_n(&activeGeometry, __ATOMIC_ACQUIRE);
    if (!request)
        return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(request);
    const uint32_t updated = native_planes::wordAt(bytes, swapUpdated);
    const uint32_t background = native_planes::wordAt(bytes, swapBackground);
    OSObject *surfaces[kLayers] = {};
    memcpy(surfaces, bytes + swapSurfaces, sizeof(surfaces));
    bool hasSurface[kLayers] = {};
    native_planes::Layer layers[kLayers];
    bool badObject = false, compressedSurface = false;
    for (unsigned i = 0; i < kLayers; ++i) {
        if ((updated & (1u << i)) == 0u || !surfaces[i])
            continue;
        if (!geometry)
            return false;
        hasSurface[i] = true;
        if (!surfaces[i]->metaCast("IOSurface")) {
            badObject = true;
            continue;
        }
        auto *rawMemory = native_legacy_surface_descriptor(surfaces[i]);
        auto *memory = rawMemory && rawMemory->metaCast("IOMemoryDescriptor") ? rawMemory : nullptr;
        if (memory) {
            addr64_t firstPa = cachedFirstPa(memory);
            if (!firstPa && memory->prepare(kIODirectionOut) == kIOReturnSuccess) {
                IOByteCount segLen = 0;
                firstPa = memory->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
                memory->complete(kIODirectionOut);
            }
            compressedSurface = compressedSurface || (firstPa && tegra_gm20b_surface_compressed(firstPa & ~0xfffULL));
        }
        layers[i] = layerOf(bytes, i, surfaces[i]);
    }
    const uint32_t width = static_cast<uint32_t>(geometry >> 32), height = static_cast<uint32_t>(geometry);
    const unsigned reason = badObject || compressedSurface
                                ? native_planes::kSurface
                                : switch_external::checkSwap(updated, background, hasSurface, layers, width, height);
    if (reason == native_planes::kAccepted) {
        uint32_t id = 0;
        memcpy(&id, bytes + swapId, sizeof(id));
        cacheValidatedLayers(request, id, updated, hasSurface, surfaces, layers);
        return true;
    }
#if DEVELOPMENT || DEBUG
    if (__atomic_fetch_add(&diagValidateRejects, 1u, __ATOMIC_RELAXED) < 8u) {
        char line[512];
        int at = snprintf(line, sizeof(line), "reason %u%s updated %#x bg %#x mode %ux%u", reason,
                          badObject           ? " (non-IOSurface)"
                          : compressedSurface ? " (compressed)"
                                              : "",
                          updated, background, width, height);
        for (unsigned i = 0; i < kLayers && at > 0 && static_cast<size_t>(at) < sizeof(line); ++i) {
            if (!hasSurface[i])
                continue;
            const auto &l = layers[i];
            at +=
                snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                         "; layer %u fmt %#x planes %u xform %u %ux%u strides %u/%u src (%d,%d %dx%d) dst "
                         "(%d,%d %dx%d) check %u",
                         i, l.surface.format, l.surface.planes, l.transform, l.surface.width, l.surface.height,
                         l.surface.stride[0], l.surface.stride[1], l.src.x, l.src.y, l.src.w, l.src.h, l.dst.x, l.dst.y,
                         l.dst.w, l.dst.h, native_planes::checkDetached(l, width, height, switch_external::kScan));
        }
        IOLog("SwitchDisplayPort: validateSwap rejected %s\n", line);
    }
#endif
    return false;
}

IOReturn SwitchExternalScreen::collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t)
{
    if (count)
        *count = 0;
    return kIOReturnUnsupported;
}

IOReturn SwitchExternalScreen::supportedFrameInfo(uint32_t, uint32_t *count, char *, uint32_t *)
{
    if (count)
        *count = 0;
    return kIOReturnUnsupported;
}

void SwitchExternalScreen::retryWhenReady()
{
    if (!ready && switch_gpu_ready_observe(readyQuery, [](void *, int) {}, this, &ready))
        dropFrame(pendingSwapRequest());
}

int SwitchExternalScreen::readyQuery(void *owner)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    return self->loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *screen = static_cast<SwitchExternalScreen *>(owner);
            const auto *request = screen->pendingSwapRequest();
            if (screen->dcb && request) {
                screen->kickSwapQueue();
                if (!screen->pending && screen->pendingSwapRequest() == request)
                    return 0;
            }
            // Disarm on the display loop. A delayed GPU callback could otherwise cancel a new attachment's watch.
            switch_gpu_ready_cancel(&screen->ready);
            return 1;
        }, self);
}

void SwitchExternalScreen::readyFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    self->kickSwapQueue();
}
