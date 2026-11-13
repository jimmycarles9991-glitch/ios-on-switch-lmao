// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display's window probes (debug kernels; DisplayWindowProbeDisplay.h runs them for HW Test). Each shows test
// pictures on the panel for about 0.7 s, one route production does not use yet against a known-good route, and
// compares DC A's CRC of the two frames (the CRC unit is the only read-back of what DC A scanned, DisplaySmmuProbe.h):
//  V5a  NV12 in 2-GOB (NVDEC's) and 16-GOB blocks on window B against the same picture pitch linear, which window B
//       shows for every video today. L4T window.c sets BL_16B2 for any format; nothing had run it on an overlay.
//  V3   window A column-scanning landscape NV12 in NVDEC's blocks at 1:1, both quarter turns a landscape video takes
//       (SCAN_COLUMN | H_DIRECTION, IOSurfaceAccelerator transform 7; SCAN_COLUMN | V_DIRECTION, transform 4:
//       SwitchLandscape.h), against the picture turned in memory and scanned row by row, on window B (the known-good
//       route) and on window A; the UI's own 16-GOB BGRA on window B against window A, the other half of a landscape
//       video under the UI.
//  V5c  a 2-GOB NV12 picture cropped through ADDR_H_OFFSET (bytes) and ADDR_V_OFFSET (lines) with START on the plane
//       bases, as L4T window.c programs a buffer's source rectangle, against the crop copied out in memory; H and V
//       alone too.
// Each also scans a control that must give another CRC, so equal CRCs are not a blind unit's. Pictures are 1:1 with
// the filters off: every sample is a source sample, so equal pictures give equal CRCs. Chroma comes in cells, offset so
// that no crop edge sits on a cell boundary: whatever way the window samples chroma, a cropped edge's outside neighbour
// is the same colour as the edge. Plain logic, no IOKit: test/display_window_probe_test.cpp holds each plan to a model of
// the windows' fetch.
#pragma once
#include <stdint.h>

#include "DisplayWindowProbeABI.h"
#include "DisplayWindowProbeExtras.h"
#include "NativeColumnScan.h"

namespace display_window_probe {

enum Probe : uint32_t {
    kNoProbe = 0,
    kOverlayBlockYuv = 1,
    kColumnScanYuv = 2,
    kOffsetCrop = 3,
};

struct Plan;
inline const char *extraProbeName(Probe p);
inline bool extraPlanOf(Probe probe, Plan &plan);

inline const char *probeName(Probe p)
{
    switch (p) {
    case kOverlayBlockYuv:
        return "V5a";
    case kColumnScanYuv:
        return "V3";
    case kOffsetCrop:
        return "V5c";
    default:
        return extraProbeName(p);
    }
}

constexpr uint32_t kPanelWidth = 720, kPanelHeight = 1280;
constexpr uint32_t kLandscapeWidth = 1280, kLandscapeHeight = 720;
constexpr uint32_t kCropX = 132, kCropY = 42;  // inside a GOB's 64 bytes and a 2-GOB block's 16 lines
constexpr unsigned kMaxSurfaces = 8, kMaxSteps = SWITCH_DISPLAY_WINDOW_PROBE_STEPS;
constexpr uint64_t kSurfaceAlign = 64 * 1024;
constexpr uint8_t kPitch = 0xff;  // the yLog2 of a pitch-linear surface

inline uint64_t roundUp(uint64_t value, uint64_t to)
{
    return (value + to - 1) / to * to;
}

// --- The pictures ----------------------------------------------------------------------------------------------------
// One base picture over every coordinate: luma, BGRA and chroma from a hash, so any mirror, turn or shift differs.
inline uint32_t mix(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = (x * 0x9e3779b1u) ^ ((y + seed) * 0x85ebca77u);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

inline uint8_t lumaAt(uint32_t x, uint32_t y)
{
    return static_cast<uint8_t>(mix(x, y, 1) >> 24);
}

inline uint32_t bgraAt(uint32_t x, uint32_t y)
{
    return 0xff000000u | mix(x, y, 3) >> 8;
}

// Chroma sample (cx, cy) as Cb | Cr << 8: one colour everywhere (cell 0), or one per cell of `cell` samples a side,
// the cells one sample off the origin.
inline uint16_t chromaAt(uint32_t cx, uint32_t cy, uint8_t cell)
{
    if (!cell)
        return 90 | 170 << 8;
    return static_cast<uint16_t>(mix((cx + 1) / cell, (cy + 1) / cell, 2) >> 16);
}

enum class Content : uint8_t {
    Image,    // the base picture from its origin
    Turned,   // the landscape picture turned by transform 7 (SCAN_COLUMN | H_DIRECTION) onto the portrait panel
    TurnedV,  // the landscape picture turned by transform 4 (SCAN_COLUMN | V_DIRECTION)
    Cropped,  // the base picture from (cropX, cropY), both even
};

struct Surface {
    bool nv12 = true;
    uint8_t yLog2 = kPitch;
    uint32_t width = 0, height = 0, rowBytes = 0, lumaRows = 0, chromaRows = 0;
    Content content = Content::Image;
    uint32_t cropX = 0, cropY = 0;
    uint8_t chromaCell = 1;
    uint64_t offset = 0, chromaOffset = 0;                          // in the probe's buffer
    void (*fillWith)(uint8_t *buffer, const Surface &s) = nullptr;  // DisplayWindowFormatProbes.h's pictures, by tag
    uint32_t tag = 0;

    bool block() const
    {
        return yLog2 != kPitch;
    }

    uint64_t lumaBytes() const
    {
        return static_cast<uint64_t>(rowBytes) * lumaRows;
    }

    uint64_t chromaBytes() const
    {
        return nv12 ? static_cast<uint64_t>(rowBytes) * chromaRows : 0;
    }
};

inline uint32_t blockRows(uint8_t yLog2)
{
    return yLog2 == kPitch ? 1 : 8u << yLog2;
}

inline Surface nv12(uint32_t width, uint32_t height, uint8_t yLog2, uint8_t chromaCell)
{
    Surface s;
    s.width = width;
    s.height = height;
    s.yLog2 = yLog2;
    s.chromaCell = chromaCell;
    s.rowBytes = static_cast<uint32_t>(roundUp(width, 64));
    s.lumaRows = static_cast<uint32_t>(roundUp(height, blockRows(yLog2)));
    s.chromaRows = static_cast<uint32_t>(roundUp((height + 1) / 2, blockRows(yLog2)));
    return s;
}

inline Surface bgra(uint32_t width, uint32_t height, uint8_t yLog2)
{
    Surface s;
    s.nv12 = false;
    s.width = width;
    s.height = height;
    s.yLog2 = yLog2;
    s.rowBytes = static_cast<uint32_t>(roundUp(static_cast<uint64_t>(width) * 4, 64));
    s.lumaRows = static_cast<uint32_t>(roundUp(height, blockRows(yLog2)));
    return s;
}

// The base picture's luma pixel (bx, by) that surface pixel (x, y) shows. A turned surface's pixel (x, y) is panel line
// y, pixel x: landscape column width - 1 - y, row x (H_DIRECTION), or column y, row height - 1 - x (V_DIRECTION).
inline void sourceOf(const Surface &s, uint32_t x, uint32_t y, uint32_t &bx, uint32_t &by)
{
    bx = x;
    by = y;
    if (s.content == Content::Turned) {
        bx = kLandscapeWidth - 1 - y;
        by = x;
    } else if (s.content == Content::TurnedV) {
        bx = y;
        by = kLandscapeHeight - 1 - x;
    } else if (s.content == Content::Cropped) {
        bx = s.cropX + x;
        by = s.cropY + y;
    }
}

// The same for a 4:2:0 chroma sample: a turn keeps 2x2 cells whole because the landscape sides are even.
inline void chromaSourceOf(const Surface &s, uint32_t cx, uint32_t cy, uint32_t &bx, uint32_t &by)
{
    bx = cx;
    by = cy;
    if (s.content == Content::Turned) {
        bx = kLandscapeWidth / 2 - 1 - cy;
        by = cx;
    } else if (s.content == Content::TurnedV) {
        bx = cy;
        by = kLandscapeHeight / 2 - 1 - cx;
    } else if (s.content == Content::Cropped) {
        bx = s.cropX / 2 + cx;
        by = s.cropY / 2 + cy;
    }
}

template <typename ByteAt> void fillPlane(uint8_t *plane, const Surface &s, uint32_t rows, ByteAt byteAt)
{
    if (s.block()) {
        native_column_scan::fillBlockLinear(plane, s.rowBytes, rows, s.yLog2, byteAt);
        return;
    }
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t xB = 0; xB < s.rowBytes; ++xB)
            plane[static_cast<uint64_t>(y) * s.rowBytes + xB] = byteAt(xB, y);
}

// Surface `s` into the probe's buffer; bytes past the picture are 0 (chroma 128).
inline void fill(uint8_t *buffer, const Surface &s)
{
    if (s.fillWith)
        return s.fillWith(buffer, s);
    if (!s.nv12) {
        fillPlane(buffer + s.offset, s, s.lumaRows, [&](uint32_t xB, uint32_t y) -> uint8_t {
            if (xB / 4 >= s.width || y >= s.height)
                return 0;
            uint32_t bx = 0;
            uint32_t by = 0;
            sourceOf(s, xB / 4, y, bx, by);
            return static_cast<uint8_t>(bgraAt(bx, by) >> (8 * (xB % 4)));
        });
        return;
    }
    fillPlane(buffer + s.offset, s, s.lumaRows, [&](uint32_t x, uint32_t y) -> uint8_t {
        if (x >= s.width || y >= s.height)
            return 0;
        uint32_t bx = 0;
        uint32_t by = 0;
        sourceOf(s, x, y, bx, by);
        return lumaAt(bx, by);
    });
    fillPlane(buffer + s.chromaOffset, s, s.chromaRows, [&](uint32_t xB, uint32_t cy) -> uint8_t {
        const uint32_t cx = xB / 2;
        if (cx >= (s.width + 1) / 2 || cy >= (s.height + 1) / 2)
            return 128;
        uint32_t bx = 0;
        uint32_t by = 0;
        chromaSourceOf(s, cx, cy, bx, by);
        return static_cast<uint8_t>(chromaAt(bx, by, s.chromaCell) >> (8 * (xB % 2)));
    });
}

// --- The windows -----------------------------------------------------------------------------------------------------
// One window's registers for a step, by value (NativePlanes.h's words); START and START_U as offsets in the buffer.
struct Window {
    uint32_t options = 0, depth = 0, position = 0, size = 0, prescaled = 0, hInitial = 0, vInitial = 0;
    uint32_t dda = 0, lineStride = 0, kind = 0, hOffset = 0, vOffset = 0;
    uint64_t start = 0, startU = 0;

    bool on() const
    {
        return (options & native_planes::kWinEnable) != 0u;
    }
};

inline uint32_t surfaceKind(const Surface &s)
{
    return s.block() ? native_planes::kSurfaceKindBlock | static_cast<uint32_t>(s.yLog2)
                                                              << native_planes::kSurfaceKindBlockHeightShift
                     : 0;
}

// The panel at 1:1 from surface pixel (x, y), row by row.
inline Window rowWindow(const Surface &s, uint32_t x = 0, uint32_t y = 0)
{
    Window w;
    const uint32_t bpp = s.nv12 ? 1 : 4;
    w.options = native_planes::kWinEnable | (s.nv12 ? native_planes::kCscEnable : 0);
    w.depth = s.nv12 ? native_planes::kDepthYCbCr420SP : native_planes::kDepthB8G8R8A8;
    w.size = kPanelHeight << 16 | kPanelWidth;
    w.prescaled = kPanelHeight << 16 | kPanelWidth * bpp;
    w.dda = 0x10001000;
    w.lineStride = s.nv12 ? s.rowBytes << 16 | s.rowBytes : s.rowBytes;
    w.kind = surfaceKind(s);
    w.start = s.offset;
    w.startU = s.nv12 ? s.chromaOffset : 0;
    w.hOffset = x * bpp;
    w.vOffset = y;
    return w;
}

// Landscape NV12 `s` column-scanned onto the panel at 1:1: PRESCALED V is the source width, H its height in bytes;
// H_DIRECTION starts at the line's last byte, V_DIRECTION at the last line (L4T window.c: (x + w) * Bpp - 1, y + h - 1).
inline Window columnWindow(const Surface &s, bool hDirection, bool vDirection = false)
{
    Window w = rowWindow(s);
    w.options = native_planes::kWinEnable | native_planes::kCscEnable | native_column_scan::kScanColumn |
                (hDirection ? native_column_scan::kHDirection : 0) | (vDirection ? native_column_scan::kVDirection : 0);
    w.prescaled = s.width << 16 | s.height;
    w.hOffset = hDirection ? s.width - 1 : 0;
    w.vOffset = vDirection ? s.height - 1 : 0;
    return w;
}

// --- The plans ---------------------------------------------------------------------------------------------------------
enum Check : uint8_t {
    kShown,   // a reference
    kEqual,   // the claim: this step's CRC is `against`'s
    kDiffer,  // the control: this step's CRC is not `against`'s
    kReport,  // reported, not judged
};

struct Step {
    const char *name = "";
    Window a, b;
    const Extra *extra = nullptr;  // DisplayWindowProbeExtras.h
    unsigned extras = 0;
    Check check = kShown;
    uint8_t against = 0;
};

struct Plan {
    Probe probe = kNoProbe;
    bool mismatchFails = true;  // a stable CRC unlike its reference's is a FAIL (false: INCONCLUSIVE)
    unsigned surfaces = 0, steps = 0;
    Surface surface[kMaxSurfaces];
    Step step[kMaxSteps];
    uint64_t bytes = 0;
};

// The plan's heaviest window B step (32-bit RGB over NV12) as the planes' bandwidth and memory admission take a layer:
// what the run reserves for window B. False when no step shows window B.
inline bool heaviestOverlay(const Step *steps, unsigned count, native_planes::Image &image)
{
    bool found = false;
    for (unsigned i = 0; i < count; ++i) {
        const Window &w = steps[i].b;
        const bool yuv = w.depth == native_planes::kDepthYCbCr420SP;
        if (!w.on() || (found && (yuv || !image.yuv)))
            continue;
        image = native_planes::Image();
        image.visible = true;
        image.yuv = yuv;
        image.colorDepth = w.depth;
        image.position = w.position;
        image.size = w.size;
        image.prescaled = w.prescaled;
        image.dda = w.dda;
        image.surfaceKind = static_cast<uint8_t>(w.kind);
        found = true;
    }
    return found;
}

inline unsigned addSurface(Plan &p, Surface s)
{
    s.offset = p.bytes;
    p.bytes = roundUp(p.bytes + s.lumaBytes(), kSurfaceAlign);
    if (s.nv12) {
        s.chromaOffset = p.bytes;
        p.bytes = roundUp(p.bytes + s.chromaBytes(), kSurfaceAlign);
    }
    p.surface[p.surfaces] = s;
    return p.surfaces++;
}

inline void addStep(Plan &p, const char *name, const Window &a, const Window &b, Check check = kShown,
                    unsigned against = 0)
{
    Step &s = p.step[p.steps++];
    s.name = name;
    s.a = a;
    s.b = b;
    s.check = check;
    s.against = static_cast<uint8_t>(against);
}

inline Plan overlayBlockYuvPlan()
{
    Plan p;
    p.probe = kOverlayBlockYuv;
    const Window off;
    const Surface pitch = p.surface[addSurface(p, nv12(kPanelWidth, kPanelHeight, kPitch, 1))];
    const Surface block2 = p.surface[addSurface(p, nv12(kPanelWidth, kPanelHeight, 1, 1))];
    const Surface block16 = p.surface[addSurface(p, nv12(kPanelWidth, kPanelHeight, 4, 1))];
    Window asPitch = rowWindow(block2);
    asPitch.kind = 0;
    addStep(p, "pitch", off, rowWindow(pitch));
    addStep(p, "block2", off, rowWindow(block2), kEqual, 0);
    addStep(p, "block16", off, rowWindow(block16), kEqual, 0);
    addStep(p, "control", off, asPitch, kDiffer, 1);
    return p;
}

inline Plan columnScanYuvPlan()
{
    Plan p;
    p.probe = kColumnScanYuv;
    p.mismatchFails = false;
    const Window off;
    const Surface land = p.surface[addSurface(p, nv12(kLandscapeWidth, kLandscapeHeight, 1, 0))];
    Surface turned = nv12(kPanelWidth, kPanelHeight, kPitch, 0);
    turned.content = Content::Turned;
    turned = p.surface[addSurface(p, turned)];
    Surface turnedV = nv12(kPanelWidth, kPanelHeight, kPitch, 0);
    turnedV.content = Content::TurnedV;
    turnedV = p.surface[addSurface(p, turnedV)];
    const Surface ui = p.surface[addSurface(p, bgra(kPanelWidth, kPanelHeight, 4))];
    addStep(p, "turned_b", off, rowWindow(turned));
    addStep(p, "turned_a", rowWindow(turned), off, kEqual, 0);
    addStep(p, "scan", columnWindow(land, true), off, kEqual, 1);
    addStep(p, "control", columnWindow(land, false), off, kDiffer, 2);
    addStep(p, "turned_v", rowWindow(turnedV), off);
    addStep(p, "scan_v", columnWindow(land, false, true), off, kEqual, 4);
    addStep(p, "ui_a", rowWindow(ui), off);
    addStep(p, "ui_b", off, rowWindow(ui), kEqual, 6);
    return p;
}

inline Plan offsetCropPlan()
{
    Plan p;
    p.probe = kOffsetCrop;
    const Window off;
    const Surface full = p.surface[addSurface(p, nv12(1024, 1536, 1, 4))];
    Surface crop = nv12(kPanelWidth, kPanelHeight, 1, 4);
    crop.content = Content::Cropped;
    const auto cropped = [&](uint32_t x, uint32_t y) {
        Surface s = crop;
        s.cropX = x;
        s.cropY = y;
        return p.surface[addSurface(p, s)];
    };
    const Surface both = cropped(kCropX, kCropY);
    const Surface across = cropped(kCropX, 0);
    const Surface down = cropped(0, kCropY);
    addStep(p, "ref_hv", off, rowWindow(both));
    addStep(p, "crop_hv", off, rowWindow(full, kCropX, kCropY), kEqual, 0);
    addStep(p, "control", off, rowWindow(full, kCropX, kCropY + 2), kDiffer, 1);
    addStep(p, "ref_h", off, rowWindow(across));
    addStep(p, "crop_h", off, rowWindow(full, kCropX, 0), kEqual, 3);
    addStep(p, "ref_v", off, rowWindow(down));
    addStep(p, "crop_v", off, rowWindow(full, 0, kCropY), kEqual, 5);
    return p;
}

inline bool planOf(Probe probe, Plan &plan)
{
    switch (probe) {
    case kOverlayBlockYuv:
        plan = overlayBlockYuvPlan();
        return true;
    case kColumnScanYuv:
        plan = columnScanYuvPlan();
        return true;
    case kOffsetCrop:
        plan = offsetCropPlan();
        return true;
    default:
        return extraPlanOf(probe, plan);
    }
}

}  // namespace display_window_probe

#include "DisplayWindowFormatProbes.h"
