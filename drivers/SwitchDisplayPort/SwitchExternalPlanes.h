// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware planes on the external display (display controller B): what its IOMobileFramebuffer publishes, which swaps it
// accepts, and which of DC B's windows A, B and C shows each layer. Plain C++, no IOKit: SwitchExternalScreen uses it and
// test/external_planes_test.cpp runs it on the host. The swap request, window formats, images and blend words are
// NativePlanes.h's.
//
// The external display is always the panel's clone, and Core Animation builds its swaps in clone_update (QuartzCore
// 24B119 IOMFBDisplay::clone_update 0x1846a6d24, 21H461 0x1896da164):
//  - layer 2, a detached video: a VIC copy of the panel's detached source in the source's format, as large as the clone's
//    destination rectangle, with the whole surface as source and no transform;
//  - the panel's UI layer index (0 or 1): a BGRA VIC copy of the panel's framebuffer with its alpha, aspect-fit (so
//    pillarboxed), over the video; the other of 0 and 1 off;
//  - layer 3 only with a dedicated cursor plane, which is never published.
// A display with a clone detaches only while the clone publishes IOMFBNumLayers' MaxNumLayers above 1 (24B119
// AppleDisplay::update_display_limits clears bit 0x2000 otherwise and set_clone_master copies it into the clone; 21H461
// the same with bit 0x400). The clone path reads no scaling limits (IOMFBDisplay::scale_cloned_layer_p returns 1), so a
// swap is held to what DC B's registers express, not to the published scaling: Core Animation sends a refused swap again
// every frame, and the published limits are the only refusal channel.
#pragma once
#include <stdint.h>

#include "SwitchDPModes.h"
#include "SwitchExternalWindows.h"
#include "../NativeMobileDisplay/NativePlanes.h"
#include "../SwitchPower/SwitchDisplayBandwidth.h"
#include "../TegraVIC/TegraVICFormats.h"

namespace switch_external {

using native_planes::Image;
using native_planes::kLayers;
using native_planes::Layer;

// Scaling as published: the largest fraction at which the UI and two more windows, all full screen, stay inside the
// EMC's isochronous ceiling at SWITCH_DP_MAX_PIXEL_KHZ (external_planes_test WorstCaseMemoryFits); 1/2 is over it
// already at 4K60. The clone path never reads these, so they hold back no clone swap, and a swap is accepted down to
// kScan's DDA limits, past the TRM's line-buffer figures (24.1.3), because a refusal freezes the display.
constexpr uint32_t kMaxLayers = 3, kMinScaleFraction = 1, kMaxScale = 4;
constexpr uint32_t kStrideAlignment = 64;
constexpr uint64_t kBaseAlignment = 4096;

#if !defined(SWITCH_IPSW_KIND_24B119) && !defined(SWITCH_IPSW_KIND_21H461)
#error "SwitchExternalPlanes.h: the published limits are the IPSW kind's; compile with -include switch_ipsw_volumes.h"
#endif

#if SWITCH_IPSW_KIND_24B119
constexpr native_planes::Limits kPanelLimits = native_planes::kLimits24B119;
#else
constexpr native_planes::Limits kPanelLimits;
#endif
// IOMFBYUVPackedUnSupported, as the panel publishes it (NativePlanes.h kLimits24B119).
constexpr bool kPackedYuvUnsupported = kPanelLimits.packedYuvUnsupported;

struct PublishedNumber {
    const char *dictionary, *key;
    uint64_t value;
    unsigned bits;
};

// Entries of one dictionary are adjacent.
constexpr PublishedNumber kPublished[] = {
    {"DisplayPipeStrideRequirements", "StrideLinearHorizontal", kStrideAlignment, 32},
    {"DisplayPipePlaneBaseAlignment", "PlaneBaseAlignmentLinear", kBaseAlignment, 64},
    {"IOMFBNumLayers", "MaxNumLayers", kMaxLayers, 32},
    {"IOMFBScalingLimits", "RGBLayer_MinScaleFraction", kMinScaleFraction, 32},
    {"IOMFBScalingLimits", "RGBLayer_MaxScale", kMaxScale, 32},
    {"IOMFBScalingLimits", "YUVLayer_MinScaleFraction", kMinScaleFraction, 32},
    {"IOMFBScalingLimits", "YUVLayer_MaxScale", kMaxScale, 32},
};

// What the windows' registers express, which a swap is held to: sources and buffers to 4096 x 4096 (TRM Table 120), rows
// on 64 bytes, plane bases on the DC's pitch alignment, and every scale imageOf's DDA increments take (H 4.0 at 4 bytes
// a pixel and 8.0 at 1-2, V 15.0) up to 4096x.
constexpr native_planes::Limits kScan = {
    .minScaleFraction = 16,
    .maxScale = 4096,
    .yuvMinScaleFraction = 16,
    .maxSrcRectWidth = SWITCH_DP_MAX_WIDTH,
    .maxSrcRectTotal = uint64_t{SWITCH_DP_MAX_WIDTH} * SWITCH_DP_MAX_HEIGHT,
    .maxSrcBuffer = SWITCH_DP_MAX_WIDTH,
    .maxSrcBufferHeight = SWITCH_DP_MAX_HEIGHT,
    .planeBaseAlignment = 64,
    .strideAlignment = kStrideAlignment,
    .maxLayers = kMaxLayers,
};

// validateSwap's decision on the updated layers: the background black or none, each layer with a surface one a window
// shows, and at most kMaxLayers of them.
inline unsigned checkSwap(uint32_t updated, uint32_t background, const bool hasSurface[kLayers],
                          const Layer layers[kLayers], uint32_t width, uint32_t height)
{
    using namespace native_planes;
    if ((updated & ~(kLayerBits | kBackgroundBit)) || (background && background != kOpaqueBlack) ||
        ((updated & kBackgroundBit) && background != kOpaqueBlack))
        return kLayerState;
    unsigned surfaces = 0;
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!(updated & (1u << i)) || !hasSurface[i])
            continue;
        ++surfaces;
        if (const unsigned why = checkDetached(layers[i], width, height, kScan))
            return why;
    }
    return surfaces > kMaxLayers ? kTwoUi : kAccepted;
}

constexpr uint32_t kPaletteEnable = 1u << 16;  // WIN_OPTIONS CP_ENABLE

// Whether VIC reads l's format, so it can convert l (TegraVICFormats.h has no 'yuvf').
inline bool vicConverts(const Layer &l)
{
    return tegra_vic_format(l.surface.format) != nullptr;
}

// The image a window scans of l, in rows or (block, the GPU's layout: TegraGM20BLayout.h) block-linear, or false when no
// window reads it so. Every byte it reads is in the surface's allocation, which is what a slot maps. Chroma that VIC
// cannot resample is scanned co-sited, as SwitchSurfaceColor.h scans a siting VIC has no conversion for.
inline bool imageFor(const Layer &l, const tgm_layout *block, uint32_t width, uint32_t height, Image &image)
{
    const bool ok = native_planes::imageOf(l, width, height, image, block);
    // imageOf adds DC A's palette (the panel's gamma table); DC B's palettes hold nothing.
    image.options &= ~kPaletteEnable;
    image.convert = image.convert && vicConverts(l);
    return ok && (!image.visible || image.readHigh <= ((l.surface.allocation + 4095) & ~4095ULL));
}

struct Placement {
    bool on = false;
    bool csc = false;  // the window's CSC is written with this frame
    int layer = -1;
    unsigned depth = 0;
};

struct Plan {
    Placement window[kWindows];
    // Layers with an image below the third (more than the display publishes), or a YUV layer VIC cannot convert that
    // finds no window. A clone frame never has either: its one YUV layer and one RGB leave a window that the frame
    // before had off.
    uint32_t unshown = 0;
    // YUV layers VIC converts to BGRA for a window: chroma sited where no window samples it (Image::convert, as the
    // panel's windows), or no window free that holds the layer's matrix and range or may have its CSC written now.
    uint32_t convert = 0;
};

// Which window shows which layer. Depth follows the swap's stack order (NativePlanes.h stackPosition), 0 in front. The
// topmost RGB layer (the UI) takes window A and the others B and C first. A YUV layer takes a window whose CSC holds its
// matrix and range already, else one that is not scanning, whose CSC is written with the frame; with neither it is
// converted (planSwap) when it is one of the `convertible` layers.
inline Plan planWindows(const bool present[kLayers], const Image images[kLayers], const WindowState state[kWindows],
                        uint32_t convertible = (1u << kLayers) - 1)
{
    Plan plan;
    unsigned order[kLayers] = {};
    unsigned count = 0;
    for (unsigned position = kLayers; position-- > 0;)
        for (unsigned layer = 0; layer < kLayers; ++layer)
            if (native_planes::stackPosition(layer) == position && present[layer] && images[layer].visible)
                order[count++] = layer;
    for (unsigned k = kWindows; k < count; ++k)
        plan.unshown |= 1u << order[k];
    if (count > kWindows)
        count = kWindows;
    constexpr Window kPreference[] = {kB, kC, kA};
    bool placed[kWindows] = {};
    const auto place = [&](Window w, unsigned k, bool csc) {
        plan.window[w] = {true, csc, int(order[k]), k};
        placed[k] = true;
    };
    for (unsigned k = 0; k < count; ++k)
        if (!images[order[k]].yuv) {
            place(kA, k, false);
            break;
        }
    for (unsigned k = 0; k < count; ++k) {
        const Image &m = images[order[k]];
        if (!m.yuv)
            continue;
        if (m.convert) {
            plan.convert |= 1u << order[k];
            continue;
        }
        int chosen = -1;
        for (const Window w : kPreference)
            if (chosen < 0 && !plan.window[w].on && state[w].matrix == int(m.matrix) &&
                state[w].fullRange == int(m.fullRange))
                chosen = int(w);
        const bool csc = chosen < 0;
        for (const Window w : kPreference)
            if (chosen < 0 && !plan.window[w].on && !state[w].live)
                chosen = int(w);
        if (chosen >= 0)
            place(Window(chosen), k, csc);
        else if (convertible & (1u << order[k]))
            plan.convert |= 1u << order[k];
        else
            plan.unshown |= 1u << order[k];
    }
    for (unsigned k = 0; k < count; ++k) {
        if (placed[k] || images[order[k]].yuv)
            continue;
        for (const Window w : kPreference)
            if (!placed[k] && !plan.window[w].on)
                place(w, k, false);
    }
    return plan;
}

// The BGRA rows VIC converts a YUV layer's whole surface into: as wide as the surface, or a quarter of it when the layer
// is narrowed more than 4x (a YUV window's H DDA takes 8.0, an RGB one's 4.0), at VIC's 256-byte pitch.
struct Conversion {
    uint32_t width, height, pitch, divisor;
    uint64_t bytes;
};

inline Conversion conversionOf(const Layer &l)
{
    const uint32_t divisor = uint64_t(l.src.w) > uint64_t(l.dst.w) * 4 ? 4 : 1;
    const uint32_t width = (l.surface.width + divisor - 1) / divisor;
    const uint32_t pitch = (width * 4 + 255) & ~255u;
    return {width, l.surface.height, pitch, divisor, uint64_t(pitch) * l.surface.height};
}

// The image a window scans of l once VIC has converted it: the same rectangles on the BGRA, the source across divided
// as the conversion divided the surface (rounded out). Opaque, as VIC writes alpha 1 from a source without alpha.
inline bool convertedImage(const Layer &l, uint32_t width, uint32_t height, Image &image)
{
    const Conversion c = conversionOf(l);
    Layer rgb = l;
    rgb.kind = native_planes::kKindBgra;
    rgb.surface = {};
    rgb.surface.format = native_planes::fourcc('B', 'G', 'R', 'A');
    rgb.surface.width = c.width;
    rgb.surface.height = c.height;
    rgb.surface.stride[0] = c.pitch;
    rgb.surface.planeHeight[0] = c.height;
    rgb.surface.allocation = rgb.surface.descriptorBytes = c.bytes;
    const int32_t d = int32_t(c.divisor);
    rgb.src.x = l.src.x / d;
    rgb.src.w = (l.src.x + l.src.w + d - 1) / d - rgb.src.x;
    return imageFor(rgb, nullptr, width, height, image);
}

// planWindows, again with each layer it sends to VIC as its converted image, until none more is sent. False when a
// converted image has no window (convertedImage fails), which no accepted layer reaches; plan.convert is then that layer.
inline bool planSwap(const bool present[kLayers], const Layer layers[kLayers], Image images[kLayers],
                     const WindowState state[kWindows], uint32_t width, uint32_t height, Plan &plan)
{
    uint32_t converted = 0, convertible = 0;
    for (unsigned i = 0; i < kLayers; ++i)
        convertible |= uint32_t(present[i] && vicConverts(layers[i])) << i;
    for (;;) {
        plan = planWindows(present, images, state, convertible);
        if (!plan.convert) {
            plan.convert = converted;
            return true;
        }
        for (unsigned i = 0; i < kLayers; ++i)
            if ((plan.convert & (1u << i)) && !convertedImage(layers[i], width, height, images[i])) {
                plan.convert = 1u << i;
                return false;
            }
        converted |= plan.convert;
    }
}

// A window's registers for image m at `depth`, its surface mapped from `iova`. RGB is premultiplied over what lies
// behind; YUV is opaque through its CSC.
inline WindowRegisters registersOf(const Image &m, unsigned depth, bool csc, uint32_t iova)
{
    using namespace native_planes;
    WindowRegisters r;
    r.on = true;
    r.options = m.options;
    r.colorDepth = m.colorDepth;
    r.byteSwap = m.byteSwap;
    r.position = m.position;
    r.size = m.size;
    r.prescaled = m.prescaled;
    r.hInitial = m.hInitial;
    r.vInitial = m.vInitial;
    r.dda = m.dda;
    r.lineStride = m.lineStride;
    r.blend = (m.yuv ? kBlendBypass : kBlendPremultiplied) | depth;
    r.blendSelect = m.yuv ? 0 : kBlendOver;
    r.start = iova + static_cast<uint32_t>(m.start[0]);
    r.startU = m.yuv ? iova + static_cast<uint32_t>(m.start[1]) : 0;
    r.hOffset = m.hOffset;
    r.vOffset = m.vOffset;
    r.kind = m.surfaceKind;
    if (m.yuv) {
        r.matrix = int(m.matrix);
        r.fullRange = m.fullRange ? 1 : 0;
        r.csc = csc;
        for (unsigned i = 0; i < 8; ++i)
            r.cscWords[i] = kCsc[m.matrix][m.fullRange ? 1 : 0][i];
    }
    return r;
}

// What a window scanning m fetches at the head's pixel clock: its bytes a pixel, more for a source larger than its
// destination on either axis.
inline switch_display::Demand demandOf(const Image &m, uint32_t pixelClockHz)
{
    switch_display::Demand d = {m.position & 0xffff, m.position >> 16, m.size & 0xffff, m.size >> 16,
                                uint64_t(pixelClockHz) * native_planes::fetchBytesPerPixel(m)};
    const uint32_t sourceWidth = (m.prescaled & 0xffff) / native_planes::prescaledBytesPerPixel(m);
    d.bytesPerSecond = switch_display::scaled(d.bytesPerSecond, sourceWidth, d.width);
    d.bytesPerSecond = switch_display::scaled(d.bytesPerSecond, m.prescaled >> 16, d.height);
    return d;
}

// The frame for a plan: each window's registers (its layer's surface mapped at iova[layer]) and the memory the windows
// fetch, their peak the most any overlap of them fetches at once.
inline Frame frameOf(const Plan &plan, const Image images[kLayers], const uint32_t iova[kLayers], uint32_t pixelClockHz)
{
    Frame frame;
    switch_display::Demand demands[kWindows] = {};
    unsigned count = 0;
    for (unsigned w = 0; w < kWindows; ++w) {
        const Placement &p = plan.window[w];
        if (!p.on)
            continue;
        const Image &m = images[p.layer];
        frame.window[w] = registersOf(m, p.depth, p.csc, iova[p.layer]);
        const switch_display::Demand d = demandOf(m, pixelClockHz);
        const uint32_t sourceWidth = (m.prescaled & 0xffff) / native_planes::prescaledBytesPerPixel(m);
        (void)switch_display::memoryWindow(frame.memory[w], d.bytesPerSecond, sourceWidth, m.prescaled >> 16, d.width,
                                           d.height, m.yuv, !m.surfaceKind, false);
        demands[count++] = d;
    }
    frame.peakBytes = switch_display::peak(demands, count);
    return frame;
}

}  // namespace switch_external
