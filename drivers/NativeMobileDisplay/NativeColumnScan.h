// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A column-scanning a 4K NV12 frame (debug kernels only, NativeColumnScanDisplay.h runs it): the experiment that
// has to pass before a deferred IOSA job can show a landscape video by scanning NVDEC's frame in place. Core Animation
// sends every quarter-turned layer on an internal display through the IOSurfaceAccelerator (QuartzCore 21H461:
// AppleInternalDisplay is display type 0 and never reads SupportsRotation), so today TegraVIC copies each frame to
// rotate it. Plain logic, no IOKit: test/display_column_scan_test.cpp runs it against a model of window A.
//
// Window A's rules (L4T drivers/video/tegra/dc window.c tegra_dc_update_scaling and tegra_dc_update_windows, dc_priv.h
// win_use_h/v_filter, dc_config.c; GPL-2.0, used as references only). With SCAN_COLUMN each output line is a source
// column and each output pixel a source row:
//  - PRESCALED_SIZE: V = the source width in pixels, H = the source height in bytes (one per NV12 luma pixel);
//  - DDA_INCREMENT: H = source height / output width, V = source width / output height, 4.12, at most 8.0 (H, 4:2:0
//    counts as 2 bytes a pixel) and 15.0 (V); the H and V initial DDAs swap (H takes the y fraction);
//  - H_DIRECTION (decrement) starts at ADDR_H_OFFSET = (x + w) * Bpp - 1, the line's last byte;
//  - the H filter is on when the source height differs from the output width, the V filter when the width differs
//    from the output height;
//  - T210 window A, alone among DC A's windows, rotates planar formats: PLANAR_ROTATION_MAXIMUM_SIZE {4096, 1, 2304, 1}.
// SCAN_COLUMN | H_DIRECTION is the rotation the landscape UI used on hardware before the portrait-native layout
// (NativeScanout.h): panel line j shows landscape column width - 1 - j, its pixel i landscape row i.
//
// The run: window A shows a reference frame first, the image this file says the column scan must produce, rendered in
// software as BGRA in the UI's own layout; then the 3840x2160 surface column-scanned at 1/3 for 30 frames; then
// exactly what it had before. The DC's CRC of each is the only read-back of what it scanned. The column scan's CRC
// equal to the reference's proves the scan pixel-exact: the luma is a grey image with every chroma sample 128 and the
// CSC is the full-range identity on luma (KYRGB 1.0, chroma terms 0), the DDAs are whole (3.0) so every sample sits at
// filter phase 0, whose taps are the identity, and the image is constant over each landscape pixel's 3x3 source cell,
// so where in its cell the scan samples does not change it.
#pragma once
#include <stdint.h>

#include "NativePlanes.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

namespace NativeColumnScan {

constexpr uint32_t kHDirection = 1u << 0, kVDirection = 1u << 2, kScanColumn = 1u << 4;
constexpr uint32_t kBlockKind = 2;  // SURFACE_KIND BLOCK; BLOCK_HEIGHT (log2 GOBs) in bits 6:4
constexpr uint32_t kPlanarRotationMaxWidth = 4096, kPlanarRotationMaxHeight = 2304;

// The test surface: a 4K frame as NVDEC writes one (luma rows to 2176, the next 128-line block; chroma after them),
// laid out in GOBs of 2^yLog2 (4: the UI's 16-GOB blocks; 1: NVDEC's own, SwitchVideoDecoders/AVD/BlockLinear.h).
constexpr uint32_t kSourceWidth = 3840, kSourceHeight = 2160, kRowBytes = 3840;
constexpr uint32_t kLumaRows = 2176, kChromaRows = 1152;
constexpr uint64_t kChromaOffset = (uint64_t)kRowBytes * kLumaRows;
constexpr uint64_t kSurfaceBytes = kChromaOffset + (uint64_t)kRowBytes * kChromaRows;
constexpr uint8_t kMaxYLog2 = 4;

// The panel (portrait 720x1280) showing the 1280x720 landscape image, and the reference frame in the UI's layout.
constexpr uint32_t kPanelWidth = 720, kPanelHeight = 1280, kScale = 3;
constexpr uint32_t kReferenceRowBytes = kPanelWidth * 4;
constexpr uint8_t kReferenceYLog2 = 4;
constexpr uint64_t kReferenceBytes = (uint64_t)kReferenceRowBytes * kPanelHeight;
static_assert(kSourceWidth == kPanelHeight * kScale && kSourceHeight == kPanelWidth * kScale, "a 1/3 scan of 4K");
static_assert(kLumaRows % (8u << kMaxYLog2) == 0 && kChromaRows % (8u << kMaxYLog2) == 0 &&
                  kChromaRows * 2 >= kSourceHeight && kPanelHeight % (8u << kReferenceYLog2) == 0,
              "planes in whole blocks of any height up to 16 GOBs");

// Window A's registers for one frame, by value.
struct Window {
    uint32_t options, depth, position, size, prescaled, hInitial, vInitial, dda, lineStride, kind;
    uint32_t start, startU, hOffset, vOffset;
};

inline bool sameWindow(const Window &a, const Window &b)
{
    return a.options == b.options && a.depth == b.depth && a.position == b.position && a.size == b.size &&
           a.prescaled == b.prescaled && a.hInitial == b.hInitial && a.vInitial == b.vInitial && a.dda == b.dda &&
           a.lineStride == b.lineStride && a.kind == b.kind && a.start == b.start && a.startU == b.startU &&
           a.hOffset == b.hOffset && a.vOffset == b.vOffset;
}

// The test surface column-scanned onto the whole panel, the landscape image upright (the luma and chroma planes at
// `luma`, `luma` + kChromaOffset in window A's address space). False when window A cannot scan it so.
inline bool scanWindow(uint32_t luma, uint8_t yLog2, Window &w)
{
    const uint32_t hInc = NativePlanes::ddaIncrement(kSourceHeight, kPanelWidth, 8),
                   vInc = NativePlanes::ddaIncrement(kSourceWidth, kPanelHeight, 15);
    if (!hInc || !vInc || yLog2 > kMaxYLog2 || kSourceWidth > kPlanarRotationMaxWidth ||
        kSourceHeight > kPlanarRotationMaxHeight || (uint64_t)luma + kSurfaceBytes > 0x100000000ull)
        return false;
    w = {};
    w.options = NativePlanes::kWinEnable | kScanColumn | kHDirection | NativePlanes::kCscEnable |
                (kSourceHeight != kPanelWidth ? NativePlanes::kHFilterOn : 0) |
                (kSourceWidth != kPanelHeight ? NativePlanes::kVFilterOn : 0);
    w.depth = NativePlanes::kDepthYCbCr420SP;
    w.size = kPanelHeight << 16 | kPanelWidth;
    w.prescaled = kSourceWidth << 16 | kSourceHeight;
    w.dda = vInc << 16 | hInc;
    w.lineStride = kRowBytes << 16 | kRowBytes;
    w.kind = kBlockKind | (uint32_t)yLog2 << 4;
    w.start = luma;
    w.startU = luma + (uint32_t)kChromaOffset;
    w.hOffset = kSourceWidth - 1;
    return true;
}

// The reference frame (at `start`) as the UI's portrait windows scan theirs: 16-GOB blocks, rows top to bottom, 1:1.
// `depth` is the UI's B8G8R8A8 or R8G8B8A8 (the frame is grey: either reads it alike).
inline Window referenceWindow(uint32_t start, uint32_t depth)
{
    Window w = {};
    w.options = NativePlanes::kWinEnable;
    w.depth = depth;
    w.size = kPanelHeight << 16 | kPanelWidth;
    w.prescaled = kPanelHeight << 16 | kReferenceRowBytes;
    w.dda = 0x10001000;
    w.lineStride = kReferenceRowBytes;
    w.kind = kBlockKind | (uint32_t)kReferenceYLog2 << 4;
    w.start = start;
    return w;
}

// --- The test image --------------------------------------------------------------------------------------------------
// Grey, so that the CSC passes luma through: a white border 48 source pixels (16 panel pixels) wide, a black square in
// the landscape top left (it tells every mirror and transpose apart), a luma ramp along x and bands every 240 rows.
// Every edge and ramp step falls on a multiple of 3.
constexpr uint32_t kBorder = 48, kMarkerFrom = 96, kMarkerTo = 480;
constexpr uint8_t kChroma = 128;

inline uint8_t lumaAt(uint32_t x, uint32_t y)
{
    if (x < kBorder || y < kBorder || x >= kSourceWidth - kBorder || y >= kSourceHeight - kBorder)
        return 235;
    if (x >= kMarkerFrom && x < kMarkerTo && y >= kMarkerFrom && y < kMarkerTo)
        return 16;
    return (uint8_t)(16 + x * 160 / kSourceWidth + ((y / 240) & 1) * 40);
}

// What panel line `line`, pixel `pixel` must show: landscape pixel (1279 - line, pixel), i.e. its 3x3 source cell.
inline uint8_t panelLuma(uint32_t line, uint32_t pixel)
{
    return lumaAt(kScale * (kPanelHeight - 1 - line), kScale * pixel);
}

inline uint32_t greyBgra(uint8_t y)
{
    return 0xff000000u | (uint32_t)y << 16 | (uint32_t)y << 8 | y;
}

// Block-linear fills, a 16-byte GOB run at a time (TegraGM20BLayout.h's layout, which DC A scans).
template <typename ByteAt>
void fillBlockLinear(uint8_t *plane, uint32_t rowBytes, uint32_t rows, uint8_t yLog2, ByteAt byteAt)
{
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t xB = 0; xB < rowBytes; xB += 16) {
            uint8_t *run = plane + tgm_layout_offset_of(rowBytes, yLog2, xB, y);
            for (uint32_t i = 0; i < 16; ++i)
                run[i] = byteAt(xB + i, y);
        }
}

// The NV12 test surface (kSurfaceBytes) in blocks 2^yLog2 GOBs tall; rows past the image are border.
inline void fillSurface(uint8_t *surface, uint8_t yLog2)
{
    fillBlockLinear(surface, kRowBytes, kLumaRows, yLog2, [](uint32_t x, uint32_t y) {
        return y < kSourceHeight ? lumaAt(x, y) : (uint8_t)235;
    });
    fillBlockLinear(surface + kChromaOffset, kRowBytes, kChromaRows, yLog2, [](uint32_t, uint32_t) {
        return kChroma;
    });
}

// The reference frame (kReferenceBytes): panelLuma as grey BGRA.
inline void fillReference(uint8_t *frame)
{
    fillBlockLinear(frame, kReferenceRowBytes, kPanelHeight, kReferenceYLog2, [](uint32_t xB, uint32_t y) {
        const uint32_t pixel = greyBgra(panelLuma(y, xB / 4));
        return (uint8_t)(pixel >> (8 * (xB % 4)));
    });
}

// --- The run -----------------------------------------------------------------------------------------------------------
// One frame end at a time, the driver's I/O kept out (Action says what to write). The DC's CRC is read as the SMMU
// probe reads it (DisplaySmmuProbe.h): 0 is never a sample, a sample is two frame ends in a row reading the same value,
// and the first two frame ends after a latch can still carry the previous frame's CRC.
struct Run {
    enum Stage {
        kIdle,
        kPreparing,  // the trigger is filling the surfaces
        kWaiting,    // for nothing in flight on window A or B (swaps are held from here)
        kReferenceLatch,
        kReference,
        kScanLatch,
        kScan,
        kRestoreLatch,
    };

    enum Action {
        kNothing,
        kStart,     // save window A, the CRC unit on, the reference frame
        kShowScan,  // the CSC and filter taps (window A is RGB and unfiltered), then the column scan
        kRestore,   // window A's saved registers and the CRC unit as they were
        kFinish,    // the line; window A's CSC and taps back once it scans RGB again; the swaps go
    };

    static constexpr unsigned kScanFrames = 30, kWaitFrames = 180, kLatchFrames = 4, kTransitionFrames = 2;
    static constexpr unsigned kZeroReads = 8, kSettleReads = 4;

    Stage stage = kIdle, failedAt = kIdle;
    const char *why = nullptr;  // the first failure
    unsigned frames = 0, latchFrames = 0, wait = 0, zeros = 0, reads = 0;
    uint32_t last = 0, reference = 0, scanCrc = 0;
    unsigned scanFrames = 0, crcChanges = 0, underflows = 0, referenceUnderflows = 0, mismatches = 0;
    bool restored = false, restoreWritten = false;

    bool busy() const
    {
        return stage >= kWaiting;
    }

    bool windowChanged() const
    {
        return stage >= kReferenceLatch;
    }

    void begin()
    {
        *this = Run();
        stage = kWaiting;
    }

    // Every DC A frame end while busy(): whether nothing is in flight (only read while waiting), whether a window A or
    // general request is still out, the latched CRC, WIN_A_UF for the frame that ended, and whether window A's active
    // registers are the set last written (the column scan's, or the saved ones once restoring). An underflow from the
    // column scan's request to the restore's latch counts against the column scan: the frame that ended at a latch is
    // the new set's when the request beat the boundary, the old one's when it did not.
    Action frameEnd(bool quiet, bool requestOut, uint32_t crc, bool underflow, bool asWritten)
    {
        ++frames;
        switch (stage) {
        case kWaiting:
            if (quiet) {
                stage = kReferenceLatch;
                latchFrames = 0;
                return kStart;
            }
            return frames >= kWaitFrames ? fail("the display never went quiet") : kNothing;
        case kReferenceLatch:
            referenceUnderflows += underflow;
            if (requestOut)
                return ++latchFrames >= kLatchFrames ? fail("window A did not take the reference frame") : kNothing;
            stage = kReference;
            wait = kTransitionFrames;
            return kNothing;
        case kReference:
            referenceUnderflows += underflow;
            if (wait && wait--)
                return kNothing;
            return settleReference(crc);
        case kScanLatch:
            underflows += underflow;
            if (requestOut)
                return ++latchFrames >= kLatchFrames ? fail("window A did not take the column scan") : kNothing;
            stage = kScan;
            return kNothing;
        case kScan:
            return scanFrame(crc, underflow, asWritten);
        case kRestoreLatch:
            underflows += underflow;
            if (requestOut && ++latchFrames < kLatchFrames)
                return kNothing;
            restored = !requestOut && asWritten;
            if (!restored && !why) {
                why = requestOut ? "window A did not take its registers back"
                                 : "window A's registers are not the saved ones";
                failedAt = stage;
            }
            stage = kIdle;
            return kFinish;
        default:
            return kNothing;
        }
    }

    Action settleReference(uint32_t crc)
    {
        if (!crc) {
            last = 0;
            return ++zeros >= kZeroReads ? fail("the CRC unit reads 0") : kNothing;
        }
        if (crc != last) {
            last = crc;
            return ++reads >= kSettleReads ? fail("the reference frame's CRC is not stable") : kNothing;
        }
        reference = crc;
        stage = kScanLatch;
        latchFrames = 0;
        return kShowScan;
    }

    Action scanFrame(uint32_t crc, bool underflow, bool asWritten)
    {
        ++scanFrames;
        underflows += underflow;
        mismatches += !asWritten;
        if (scanFrames > kTransitionFrames) {
            if (scanFrames > kTransitionFrames + 1 && crc != scanCrc)
                ++crcChanges;
            scanCrc = crc;
        }
        return scanFrames >= kScanFrames ? restore() : kNothing;
    }

    Action restore()
    {
        stage = kRestoreLatch;
        latchFrames = 0;
        restoreWritten = true;
        return kRestore;
    }

    Action fail(const char *reason)
    {
        why = reason;
        failedAt = stage;
        if (stage == kWaiting) {
            stage = kIdle;
            return kFinish;
        }
        return restore();
    }

    // The display stops, sleeps or goes off: no frame end will come. True when window A's saved registers must be
    // written now; the driver then finishes without waiting for their latch.
    bool abort(const char *reason)
    {
        if (!busy())
            return false;
        if (!why) {
            why = reason;
            failedAt = stage;
        }
        const bool write = windowChanged() && !restoreWritten;
        restoreWritten = restoreWritten || write;
        stage = kIdle;
        return write;
    }

    // Stable, underflow-free and as written, and window A back as it was.
    bool clean() const
    {
        return !why && scanCrc && !crcChanges && !underflows && !mismatches && restored;
    }

    // Clean, and the very image the reference frame shows.
    bool exact() const
    {
        return clean() && scanCrc == reference;
    }

    static const char *name(Stage s)
    {
        static const char *const names[] = {"idle",      "preparing",  "waiting", "reference latch",
                                            "reference", "scan latch", "scan",    "restore latch"};
        return s <= kRestoreLatch ? names[s] : "?";
    }
};

}  // namespace NativeColumnScan
