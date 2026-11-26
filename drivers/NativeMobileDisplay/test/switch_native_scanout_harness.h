// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window B's slot code (SwitchPlaneSlots.h, SwitchNativePlaneDisplay.h) in a display that has only what it reads: an SMMU
// domain that records the pages mapped at each IOVA, the plane IOVA space, a busy flag per slot, and producers whose
// next scanout the test sets.
#pragma once
#include <gtest/gtest.h>
#include <array>
#include <cstdarg>
#include <functional>
#include <map>
#include <vector>
#define IONewZero(type, count) new (std::nothrow) type[count]()
#define SWITCH_FRAME_TIMELINE  1

using IOByteCount = uint64_t;
using addr64_t = uint64_t;
constexpr int kIODirectionOut = 2, kIOReturnSuccess = 0, kIOMemoryMapperNone = 0, kIOMapAnywhere = 1,
              kIOMapReadOnly = 2;

inline unsigned ioLogLines;

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *, ...)
{
    ++ioLogLines;
}

class OSObject {
public:
    unsigned refs = 1;

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

class IOMemoryMap : public OSObject {
public:
    static uint64_t getVirtualAddress()
    {
        return 0;
    }

    static uint64_t getLength()
    {
        return 0;
    }
};

// A surface's pages: contiguous from pa, or a hole (no physical address) at the page `hole`.
class IOMemoryDescriptor : public OSObject {
public:
    unsigned completes = 0, prepares = 0;
    uint64_t pa = 0x90000000ULL, length = 0;
    int64_t hole = -1;

    const void *metaCast(const char *) const
    {
        return this;
    }

    uint64_t getLength() const
    {
        return length;
    }

    int prepare(int)
    {
        ++prepares;
        return kIOReturnSuccess;
    }

    void complete(int)
    {
        ++completes;
    }

    addr64_t getPhysicalSegment(uint64_t offset, IOByteCount *bytes, int) const
    {
        EXPECT_GT(prepares, completes) << "IOGMD: getPhysicalSegment requires a prepared descriptor";
        *bytes = 4096;
        return int64_t(offset / 4096) == hole ? 0 : pa + offset;
    }

    static IOMemoryMap *map(int)
    {
        return nullptr;
    }
};

#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object)               \
            (object)->release();  \
        (object) = nullptr;       \
    } while (0)
inline unsigned unholds;
inline void *nextSurfaceHoldPtr;
inline int nextSurfaceHoldTiled;
inline bool surfaceCompressedFlag;
inline std::function<void(uint64_t)> onSurfaceCompressedCheck;
inline std::map<uint64_t, struct switch_gpu_backing *> findPaBackings;
inline std::map<struct switch_gpu_backing *, int> backingRefs;
inline std::map<struct switch_gpu_backing *, int> backingScanoutHolds;

extern "C" void tegra_gm20b_surface_unhold(void *)
{
    ++unholds;
}

extern "C" void *tegra_gm20b_surface_hold(uint64_t, int *held)
{
    *held = nextSurfaceHoldTiled;
    return nextSurfaceHoldPtr;
}

extern "C" int tegra_gm20b_surface_compressed(uint64_t pa)
{
    if (onSurfaceCompressedCheck)
        onSurfaceCompressedCheck(pa);
    return surfaceCompressedFlag ? 1 : 0;
}

extern "C" struct switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t pa)
{
    const auto found = findPaBackings.find(pa);
    if (found == findPaBackings.end() || !found->second)
        return nullptr;
    ++backingRefs[found->second];
    return found->second;
}

extern "C" void switch_gpu_backing_retain(struct switch_gpu_backing *backing)
{
    if (backing)
        ++backingRefs[backing];
}

extern "C" void switch_gpu_backing_release(struct switch_gpu_backing *backing)
{
    if (!backing)
        return;
    EXPECT_GT(backingRefs[backing], 0);
    --backingRefs[backing];
}

extern "C" void switch_gpu_backing_scanout(struct switch_gpu_backing *backing, int add)
{
    if (!backing)
        return;
    if (add) {
        ++backingScanoutHolds[backing];
    } else {
        EXPECT_GT(backingScanoutHolds[backing], 0);
        --backingScanoutHolds[backing];
    }
}

inline void switch_dcache_clean_invalidate(const void *, uint64_t)
{}

#include "../SwitchPlaneSlot.h"
#include "../SwitchNativePlane.h"
#include "../SwitchDisplayComposition.h"
#include "../SwitchWindowCaps.h"
#include "../../TegraSMMU/TegraSMMUSpace.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../../TegraDisplayFlip/TegraScanoutCopy.h"

namespace {
SwitchNativeScanout nextNative, nextPitch;
unsigned leases, acquisitions, pitchAcquisitions, deliveries;
int nativeError, pitchError;
uint64_t completedThrough = 0;  // tokens with a value up to this have completed

// The display domain as window B reads it: the physical page at each mapped IOVA page.
struct Mapping {
    unsigned maps = 0, unmaps = 0, count = 0;
    uint64_t first = 0, iova = 0;
    bool fail = false;
    std::map<uint64_t, uint64_t> pages;

    bool map(uint64_t at, const uint64_t *pagesPa, uint32_t n, bool write)
    {
        EXPECT_FALSE(write);
        ++maps;
        count = n;
        first = pagesPa[0];
        iova = at;
        if (fail)
            return false;
        for (uint32_t i = 0; i < n; ++i)
            EXPECT_TRUE(pages.emplace(at + uint64_t(i) * 4096, pagesPa[i]).second) << "IOVA mapped twice";
        return true;
    }

    void unmap(uint64_t at, uint32_t n)
    {
        ++unmaps;
        for (uint32_t i = 0; i < n; ++i)
            pages.erase(at + uint64_t(i) * 4096);
    }
};

template <unsigned Slots = switch_window_caps::kPlaneSlots, uint64_t Space = switch_window_caps::kPlaneSpace>
struct Display {
    static constexpr unsigned kPlaneSlots = Slots;
    static constexpr unsigned kPlaneOverlayDependencies = 3, kPlaneDependencies = 6;
    static constexpr uint64_t kPlaneSlotSpan = switch_window_caps::kPlaneSlotSpan, kPlaneSlot0 = 0x10000000ULL;
    static constexpr uint32_t kSpaceWords = TegraSMMUSpace::bitmapWords(Space);
    using PlaneSlot = switch_display::PlaneSlot;
    PlaneSlot planeSlots[kPlaneSlots] = {};
    uint32_t spaceBits[kSpaceWords] = {};
    TegraSMMUSpace planeSpace;
    using PlaneSet = switch_display::Composition;
    PlaneSet planeNext;
    bool planeNextSet = false, stats = true, windowIdle = false;
    uint32_t planeSlotUses = 0, stPlaneMaps = 0, stPlaneRemaps = 0, stPlaneNative = 0, flushes = 0;
    bool busy[kPlaneSlots] = {};
    Mapping probeDomain;

    Display()
    {
        planeSpace.init(kPlaneSlot0, Space, spaceBits, kSpaceWords);
        planeDropped = {};
        planeForgets = {};
        planeUnplacedLogged = false;
    }

    bool planeSlotBusy(int slot) const
    {
        return busy[slot];
    }

    bool planeWindowIdle() const
    {
        return windowIdle;
    }

    static const tgm_layout *surfaceLayout(OSObject *, tgm_layout &)
    {
        return nullptr;
    }

    void cleanDomain()
    {
        ++flushes;
    }

    int planeUseNoted(int slot, OSObject *)
    {
        return slot;
    }

    static bool planeUseNotes(unsigned)
    {
        return false;
    }

    void planeUseRelease(unsigned)
    {}

    void planeUseHoldScanned()
    {}

    ~Display()
    {
        for (unsigned i = 0; i < kPlaneSlots; ++i) {
            planeDropSlot(i);
            delete[] planeSlots[i].native;
        }
    }

    // Every placed slot's IOVA range lies in the space, apart from every other's, and is what the domain maps; the
    // space has exactly their granules taken.
    void expectSpaceConsistent() const
    {
        uint64_t taken = 0, mappedPages = 0;
        for (unsigned k = 0; k < kPlaneSlots; ++k) {
            const PlaneSlot &a = planeSlots[k];
            if (!a.iova)
                continue;
            const uint64_t bytes = a.high - a.low;
            EXPECT_GE(a.iova, kPlaneSlot0) << k;
            EXPECT_LE(a.iova + bytes, kPlaneSlot0 + Space) << k;
            taken += TegraSMMUSpace::round(bytes);
            mappedPages += bytes / 4096;
            for (uint64_t at = a.iova; at < a.iova + bytes; at += 4096)
                EXPECT_EQ(probeDomain.pages.count(at), 1u) << "slot " << k << " page " << at;
            for (unsigned j = k + 1; j < kPlaneSlots; ++j) {
                const PlaneSlot &b = planeSlots[j];
                EXPECT_TRUE(!b.iova || b.iova >= a.iova + bytes || a.iova >= b.iova + (b.high - b.low))
                    << k << " " << j;
            }
        }
        EXPECT_EQ(probeDomain.pages.size(), mappedPages);
        EXPECT_EQ(planeSpace.bytesTotal() - planeSpace.bytesFree(), taken);
    }

#include "../SwitchPlaneSlots.h"
#include "../SwitchNativePlaneDisplay.h"
};
}  // namespace

int switch_native_scanout_acquire(OSObject *, SwitchNativeScanout *out, bool)
{
    ++acquisitions;
    *out = nativeError ? SwitchNativeScanout{} : nextNative;
    if (out->lease)
        ++leases;
    return nativeError;
}

int switch_native_scanout_pitch(OSObject *, SwitchNativeScanout *out)
{
    ++pitchAcquisitions;
    *out = pitchError ? SwitchNativeScanout{} : nextPitch;
    if (out->lease)
        ++leases;
    return pitchError;
}

int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *token)
{
    return token->value <= completedThrough ? 1 : 0;
}

std::vector<switch_gpu_backing_token> promised;  // the overlay surface's pending writer promises

int switch_gpu_backing_promise_tokens(switch_gpu_backing *, uint32_t, uint32_t, switch_gpu_backing_token *tokens,
                                      uint32_t capacity)
{
    if (promised.size() > capacity)
        return -28;
    for (size_t i = 0; i < promised.size(); ++i)
        tokens[i] = promised[i];
    return int(promised.size());
}

// A debug kernel's traits with no HW Test forcing any: the shipped ones (TegraVIC's SwitchVICTurn.cpp).
extern "C" SwitchDisplayTraits switch_display_traits(void)
{
    return kSwitchDisplayTraits;
}

std::vector<std::pair<switch_gpu_backing *, uint32_t>> noted;  // the backings an engine noted it maps

extern "C" void switch_gpu_backing_note_mapped(switch_gpu_backing *backing, uint32_t engine)
{
    noted.emplace_back(backing, engine);
}

void switch_native_scanout_deliver(SwitchNativeScanout *value)
{
    if (value->lease)
        ++deliveries;
}

void switch_native_scanout_release(SwitchNativeScanout *value)
{
    if (value->lease) {
        EXPECT_GT(leases, 0u);
        --leases;
    }
    *value = {};
}

namespace {
// A 1280x720 NV12 picture NVDEC decoded into a 1536x736 block-linear allocation, shown at the panel's width; the
// pitch representation VIC made of it is allocation 2.
class NativeScanoutFixture : public testing::Test {
protected:
    native_planes::Layer layer;
    native_planes::Image image;
    std::array<uint64_t, 1024> pages;
    bool resolved;

    void SetUp() override
    {
        leases = acquisitions = pitchAcquisitions = deliveries = 0;
        unholds = 0;
        nextSurfaceHoldPtr = nullptr;
        nextSurfaceHoldTiled = 0;
        surfaceCompressedFlag = false;
        onSurfaceCompressedCheck = {};
        findPaBackings.clear();
        backingRefs.clear();
        backingScanoutHolds.clear();
        completedThrough = 0;
        noted.clear();
        nativeError = pitchError = 0;
        for (uint32_t i = 0; i < pages.size(); ++i)
            pages[i] = 0x80000000ULL + uint64_t(i) * 4096;
        layer = {};
        layer.kind = native_planes::kKindNv12Video;
        layer.surface.format = native_planes::fourcc('4', '2', '0', 'v');
        layer.surface.width = 1280;
        layer.surface.height = 720;
        layer.surface.planes = 2;
        layer.surface.stride[0] = layer.surface.stride[1] = 1280;
        layer.surface.planeHeight[0] = 720;
        layer.surface.planeHeight[1] = 360;
        layer.surface.offset[1] = 1280 * 720;
        layer.src = {0, 0, 1280, 720};
        layer.dst = {0, 437, 720, 405};
        nextNative = {};
        nextNative.lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
        nextNative.view.native = reinterpret_cast<switch_gpu_backing *>(1);
        nextNative.view.producer = {73, 1, SWITCH_GPU_ENGINE_NVDEC, 163};
        auto &layout = nextNative.view.layout;
        layout.format = layer.surface.format;
        layout.plane_count = 2;
        layout.color_matrix = 2;
        layout.color_range = 1;
        layout.planes[0] = {0, 1536 * 736, 1536, 736, 1536, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1536 * 736, 1536 * 368, 768, 368, 1536, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        nextNative.allocation.allocation_id = nextNative.allocation.allocation_generation =
            nextNative.allocation.content_generation = 1;
        nextNative.allocation.pages = pages.data();
        nextNative.allocation.page_count = uint32_t(pages.size());
        nextPitch = nextNative;
        nextPitch.allocation.allocation_id = 2;
        nextPitch.view.producer = {92, 1, SWITCH_GPU_ENGINE_VIC, 164};
        resolved = false;
    }

    template <class D> int admit(D &display)
    {
        return display.planeNativeSlotFor(nullptr, layer, 720, 1280, image, resolved);
    }
};
}  // namespace
