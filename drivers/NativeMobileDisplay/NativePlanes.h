// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware planes (r18-planes): a layer Core Animation detaches is scanned out by DC A's window B, under the composited UI on
// window A, so a detached video costs no GPU work per frame and the UI is recomposited only when it changes. Plain logic, no
// IOKit: NativeLegacyDisplay validates, maps and latches swaps with it; tests/display_planes_harness.cpp runs it on the host.
// [V] = read in the 21H461 binaries or the cited source, [I] = inferred, [T] = to be proven by the next boot.
//
// Core Animation (QuartzCore 21H461; out/agent-scratch/r18-planes/re/qc/REPORT.txt):
//  - [V] detaches a layer only while backboardd runs without CA_DISABLE_DETACHED (CA::OGL::detach_contents 0x189566404) and
//    only within the display limits it read at display creation (AppleDisplay::update_display_limits 0x18959e518):
//    IOMFBScalingLimits, IOMFBMaxSrcPixels, IOMFBNumLayers, DisplayPipeStrideRequirements, DisplayPipePlaneBaseAlignment
//    (IOSurface::check_alignment 0x189557ff4: every plane's base and bytes per row). 8-bit 4:2:0 ('420v'/'420f') and 10-bit
//    biplanar are enabled by the ChipID (0x8011), not by a property.
//  - [V] has no graceful answer to a refused swap: finish_update (0x189502028) logs and sends the same swap again every frame
//    until the layer tree changes (the r6h freezes of 28 s and 100 s). So the published limits are the only refusal channel,
//    and validateSwap accepts every swap Core Animation can build from them (checkDetached below mirrors them; anything the
//    DC could not show is kept out by a limit, never refused here).
//  - [V] the detached layer goes under the UI: a YUV one in layer 2, an RGB one in layer 0, the composited UI then in layer 1
//    with alpha (detached_layer_under_destination_p 0x189567f1c, update_surface_has_alpha 0x1896d9cd4); with nothing
//    detached the UI stays the only layer. Background is opaque black. On an internal display a layer that needs a quarter
//    turn, a scale outside the limits or a conversion is first copied by the IOSurfaceAccelerator (TegraVIC) into a
//    display-ready surface: no rotated plane ever reaches the kernel.
// The swap request (IOMobileGraphicsFamily's IOMFBSwapIORequest; its offsets per IPSW kind are kits/SwitchABI's
// IOMobileFramebufferLegacy constants):
//  - [V] per layer i: an IOSurface * (updated layers only, at validateSwap), source and destination rectangles as four
//    int32 x, y, w, h, and a transform (& 7); the updated mask (bits 0-3 layers, bit 31 the background), the ready mask (bit
//    i once layer i's IOSurface fence signalled; null layers and bit 31 at once) and the background colour. After
//    validateSwap the family's enqueue fills the effective surfaces, a layer not updated inheriting the one the queue head,
//    the current swap or the screen holds: a video-only swap (bit 2 alone) is normal. An updated layer with no surface turns
//    that layer off.
//  - [V] A10X (AppleCLCD/UnifiedPipeline): two pipes, at most two layers ("cannot blend more than 2 layers"); the blender takes
//    layer 2, then 0, then 1 (layer_crossbar 0xfffffff006bd38c4), i.e. bottom to top [I: input 0 is the bottom].
// DC A (Tegra X1 T210; L4T drivers/video/tegra/dc, mainline drivers/gpu/drm/tegra/dc.c, hekate bdk/display/di.h, all GPL-2.0,
// used as references only; out/agent-scratch/r18-planes/re/tegra-dc-factsheet.md):
//  - [V] windows A, B and C each have a 6-tap H and a 2-tap V filter, pitch and block-linear surfaces, YCbCr 4:2:0 semi-planar,
//    a CSC and the gen2 blender. DDA increments are 4.12 fixed point (L4T window.c: in / out; mainline: (in - 1) / (out - 1));
//    the H increment may reach 4.0 at 4 bytes per pixel and 8.0 at 1-2 (4:2:0 counts as 2), the V one 15.0. PRESCALED_SIZE
//    is bytes x lines, SIZE and POSITION pixels. LINE_STRIDE holds the luma stride in bits 15:0 and the chroma stride in 31:16.
//  - [V] lower depth is in front; premultiplied "over" is BLEND_LAYER 0x00ffff00 | depth with MATCH/NOMATCH 0x262 (L4T
//    window.c), opaque is bypass (bit 24) | depth.
//  - [V] window, winbuf and display registers are triple-buffered (assembly, then ARM on *_UPDATE, then active on *_ACT_REQ at
//    the frame boundary: mainline dc.c tegra_dc_commit's comment, L4T "assemby -> arm -> active"): one UPDATE write and one
//    ACT_REQ write for several windows latch them at one boundary, never half-written. The CSC and filter registers (0x500-
//    0x638) are not shadowed (hekate di.h): written only while the window is off.
//  - [V] YCbCr 4:2:0 semi-planar with Cb first (NV12, IOSurface '420v'/'420f') is COLOR_DEPTH 43 (mainline plane.c and
//    hekate di.h; L4T's tegra_dc_ext.h agrees once its names are read from the word's top, SwitchWindowCaps.h; HW Test
//    display.x1.window_formats shows it on the panel); 4:2:2 semi-planar with Cb first (NV16, '422v'/'422f',
//    what Core Animation's IOSurfaceAccelerator copies of a detached video are on iPadOS 27) is 45 by the same table, and
//    mainline's T210 overlay windows list NV16 (dc.c: tegra210_dc_soc_info takes tegra114_overlay_formats).
//  - [T] a block-linear surface (WINBUF_SURFACE_KIND BLOCK with its block height; LINE_STRIDE its bytes across) is fetched
//    from START with the source origin in ADDR_H_OFFSET (bytes) and ADDR_V_OFFSET (lines), as mainline dc.c programs one
//    whenever supports_block_linear (T210 sets it); window A already scans one so (kBlockSurfaceKind).
#pragma once
#include <stdint.h>
#include <string.h>

#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraVIC/SwitchSurfaceColor.h"
#include "SwitchWindowCaps.h"

namespace native_planes {

// --- The swap request ---------------------------------------------------------------------------------------------------
constexpr unsigned kLayers = 4;
constexpr uint32_t kBackgroundBit = 1u << 31, kLayerBits = 0xfu, kOpaqueBlack = 0xff000000u;

struct Rect {
    int32_t x, y, w, h;
};

inline Rect rectAt(const unsigned char *request, unsigned base, unsigned layer)
{
    Rect r;
    memcpy(&r, request + base + static_cast<size_t>(16) * layer, sizeof(r));
    return r;
}

inline uint32_t wordAt(const unsigned char *request, unsigned offset)
{
    uint32_t value = 0;
    memcpy(&value, request + offset, sizeof(value));
    return value;
}

// Bottom to top: layer 2, layer 0, layer 1 (A10X's blender order), then 3.
inline unsigned stackPosition(unsigned layer)
{
    static const unsigned position[kLayers] = {1, 2, 0, 3};
    return position[layer & 3];
}

// --- Surfaces -----------------------------------------------------------------------------------------------------------
constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24 |
           static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(d));
}

// An IOSurface as the kernel's IOSurface describes it (plane fields for planar formats only).
struct Surface {
    uint32_t format = 0, planes = 0, width = 0, height = 0;
    uint32_t colorMatrix = 0, transfer = 0, primaries = 0, chromaTop = 0, chromaBottom = 0;
    uint64_t allocation = 0, descriptorBytes = 0;
    uint64_t offset[2] = {};
    uint32_t stride[2] = {}, planeHeight[2] = {};
};

enum Kind {
    kKindNone,
    kKindBgra,
    kKindRgba,
    kKindNv12Video,
    kKindNv12Full,
    kKindNv16Video,
    kKindNv16Full,
    kKindExtraYuv  // SwitchWindowCaps.h
};

// The formats a window scans: 32-bit BGRA (also its lossless-compressed spelling '&BGA', which the GPU here writes as plain
// rows), RGBA, and 8-bit 4:2:0 and 4:2:2 two-plane video or full range. Anything else (10-bit 'x420'/'xf20', 'w30r',
// compressed YUV) has no window format on T210.
inline Kind kindOf(uint32_t format, uint32_t planes)
{
    if ((format == fourcc('B', 'G', 'R', 'A') && !planes) || (format == fourcc('&', 'B', 'G', 'A') && planes <= 1))
        return kKindBgra;
    if (format == fourcc('R', 'G', 'B', 'A') && !planes)
        return kKindRgba;
    if (format == fourcc('4', '2', '0', 'v') && planes == 2)
        return kKindNv12Video;
    if (format == fourcc('4', '2', '0', 'f') && planes == 2)
        return kKindNv12Full;
    if (format == fourcc('4', '2', '2', 'v') && planes == 2)
        return kKindNv16Video;
    if (format == fourcc('4', '2', '2', 'f') && planes == 2)
        return kKindNv16Full;
    if (switch_window_caps::find(format, planes))
        return kKindExtraYuv;
    return kKindNone;
}

inline bool isNv12(Kind k)
{
    return k == kKindNv12Video || k == kKindNv12Full;
}

inline bool isNv16(Kind k)
{
    return k == kKindNv16Video || k == kKindNv16Full;
}

inline bool isYuv(Kind k)
{
    return isNv12(k) || isNv16(k) || k == kKindExtraYuv;
}

// --- What the display publishes (and so what Core Animation can send) ------------------------------------------------------
// A10X's own scaling where the DC can do as much (IOMFBScalingLimits 4/4/4/4 [V]), YUV down to SwitchWindowCaps.h's
// fraction; sources and buffers up to its 4096 x 2304; plane bases 64-byte aligned (the DC's pitch alignment, mainline
// pitch_align); rows 128-byte aligned (what the GPU renders in place, already published; video pool rows are 256).
// Three windows: the UI and two independently fenced detached layers.
struct Limits {
    uint32_t minScaleFraction = 4, maxScale = 4;  // 1/4 .. 4x on each axis
    uint32_t yuvMinScaleFraction = switch_window_caps::kYuvMinScaleFraction;
    uint32_t maxSrcRectWidth = switch_window_caps::kSourceWidth;
    uint64_t maxSrcRectTotal = switch_window_caps::kSourcePixels;
    uint32_t maxSrcBuffer = switch_window_caps::kSourceWidth, maxSrcBufferHeight = switch_window_caps::kSourceHeight;
    uint32_t planeBaseAlignment = 64, strideAlignment = 128;
    uint32_t maxLayers = 3;
    bool packedYuvUnsupported = false;    // published as IOMFBYUVPackedUnSupported (24B119 only, see kLimits24B119)
    bool compressionUnsupported = false;  // published as IOMFBMaxSrcPixels' IOMFBCompressionSupport 0 (24B119 only)
};

// QuartzCore 24B119 [V] reads every key above the same way (update_display_limits 0x1844e82f0; MaxSrcRectWidthForPipe and
// MaxSrcRectHeightForPipe, absent, become MaxSrcRectWidth and MaxSrcRectTotal / MaxSrcRectWidth). Two kinds of surface it
// would give window B are new with J181's chip id (0x8030), where 21H461 went by J120's (0x8011) to rule them out:
//  - Apple's packed 10-bit YUV ('p420', 'pf20'...): AppleDisplay::detachable_layer_impl (0x18443d2a0) takes them, and
//    IOMFBDisplay::detach_layer makes the IOSurfaceAccelerator copies for the display in them, unless the display says
//    IOMFBYUVPackedUnSupported (supports_packed_yuv);
//  - lossless-compressed surfaces: CADeviceUseCompression is on for 0x8030, and AppleDisplay::compressed_pixel_format then
//    gives the surfaces Core Animation makes for the display ('&8v0', '&BGA') compressed formats unless IOMFBMaxSrcPixels'
//    IOMFBCompressionSupport is 0.
// Window B scans neither, so 24B119 publishes both. 10-bit biplanar ('x420') and 'RGhA' have no such key: 24B119 gates
// the first by IOMFBYUVSupportKey alone (all YUV) and the second by chip id and the surface's transfer function.
// IOMFBYUVPackedUnSupported also makes every scaler copy of a 4:2:0 detached layer 4:2:2 ('422v', '422f'): detach_layer
// (0x18443c44c) converts the copy's format when detachable_layer_impl asks for a copy (mode 2) and supports_packed_yuv
// (display+0x75d6, update_display_limits 0x1844e9844) is false; backboardd logs it as "CADisplay 1 Packed YUV Support
// Enabled: NO". Window B and VIC take 4:2:2 for that. The display+0x6bf4 chroma mode that detach_layer also reads
// (0x18443ba04) comes from the chip id alone (update_display_limits 0x1844e9828: 2, 4:4:4, on 0x7000; otherwise 0).
// detach_layer's other two 4:2:2 triggers never fire on this display: needs_420_transform (0x18443c3c4) takes the copy's
// width (0x18443b9b8) and wants more than display+0x7700, 2160 for chip 0x8030 (0x1844e9640), and nothing in 24B119
// sets the flag bit 29 that 0x18443c378 tests (render_contents_background builds the flags at 0x18437cc6c-0x18437ce98,
// OGLDisplay::detach_layer adds bit 22, detach_layer keeps bits 31-27, 7 and 3-0 of the layer's earlier flags and
// detachable_layer_impl adds bits 7 and 23-27).
constexpr Limits kLimits24B119 = {.packedYuvUnsupported = true, .compressionUnsupported = true};

// The row and plane-base alignment published for every surface iPadOS sizes for this display (Core Animation's
// framebuffers through update_display_limits, VideoToolbox's and MediaToolbox's outputs through
// FigGetAlignmentForIOSurfaceOutput): VIC's 256-byte pitch, so any of them goes to VIC or window B as allocated. Multiples
// of Limits' strideAlignment and planeBaseAlignment, so everything validateSwap takes stays taken, and of the 128 the GPU
// renders linear rows in place at. Fixed from start: Core Animation and VideoToolbox read them once.
constexpr uint32_t kPublishedRowAlignment = 256, kPublishedBaseAlignment = 256;

// --- Validation ---------------------------------------------------------------------------------------------------------
// The reasons a swap is refused (the stats' "rejected ... (reason N)"; 2-4 as before r18).
enum Reason : unsigned {
    kAccepted = 0,
    kNotReady = 1,     // the display is not presenting
    kLayerState = 2,   // unknown mask bits, a non-black background, a layer turned on with no surface
    kGeometry = 3,     // a rectangle outside its surface, empty, or (the UI) not the whole display
    kSurface = 4,      // the IOSurface or its memory is not what its own fields say
    kBlank = 5,        // accepted: everything off over black
    kTwoUi = 6,        // more layers with surfaces than published (IOMFBNumLayers)
    kTransformed = 7,  // a transform (the display publishes no flips or rotation: Core Animation copies those)
    kScale = 8,        // a scale outside 1/4 .. 4x, or a source wider than published
    kFormat = 9,       // a format no window reads
    kAlignment = 10,   // a plane base or row not aligned as published
};

// A layer as a window would show it.
struct Layer {
    Kind kind = kKindNone;
    Surface surface;
    Rect src = {}, dst = {};
    uint32_t transform = 0;
};

// The composited UI on window A: one full-frame BGRA surface, untransformed (validateSwap's rule since r6).
inline bool isUi(const Layer &l, uint32_t width, uint32_t height)
{
    return l.kind == kKindBgra && (l.transform == 0u) && l.src.x == 0 && l.src.y == 0 &&
           static_cast<uint32_t>(l.src.w) == width && static_cast<uint32_t>(l.src.h) == height && l.dst.x == 0 &&
           l.dst.y == 0 && static_cast<uint32_t>(l.dst.w) == width && static_cast<uint32_t>(l.dst.h) == height &&
           l.surface.width >= width && l.surface.height >= height;
}

inline bool aligned(uint64_t value, uint64_t to)
{
    return (to != 0u) && ((value % to) == 0u);
}

// --- Window B's registers -------------------------------------------------------------------------------------------------
// Register words within a window's own blocks (DC_WIN 0x700 + i, DC_WINBUF 0x800 + i, WINC 0x500 + i); the driver adds the
// window's direct-aperture base (mainline dc.c tegra_plane_offset: B at 0xd80 / 0xdc0 / 0xc00).
enum : uint32_t {
    kOptions = 0x00,
    kByteSwap = 0x01,
    kColorDepth = 0x03,
    kPosition = 0x04,
    kSize = 0x05,
    kPrescaled = 0x06,
    kHInitialDda = 0x07,
    kVInitialDda = 0x08,
    kDdaIncrement = 0x09,
    kLineStride = 0x0a,
    kBufStride = 0x0b,
    kUvBufStride = 0x0c,
    kBlendLayer = 0x16,
    kBlendMatch = 0x17,
    kBlendNomatch = 0x18,  // DC_WIN
    kStartAddr = 0x00,
    kStartAddrU = 0x02,
    kHOffset = 0x06,
    kVOffset = 0x08,
    kSurfaceKind = 0x0b,
    kStartAddrHi = 0x0d,
    kStartAddrHiU = 0x0f,
    kCdeControl = 0x2f,  // DC_WINBUF
    kHFilter0 = 0x101,
    kCscYof = 0x111,
    kVFilter0 = 0x119,  // WINC
};

constexpr uint32_t kWinEnable = 1u << 30, kCscEnable = 1u << 18, kHFilterOn = 1u << 8, kVFilterOn = 1u << 10;
// WINBUF_SURFACE_KIND: BLOCK, with the block height (GOBs, log2) in bits 6:4 (mainline dc.c DC_WINBUF_SURFACE_KIND_*).
constexpr uint32_t kSurfaceKindBlock = 2, kSurfaceKindBlockHeightShift = 4;
constexpr uint32_t kDepthB8G8R8A8 = 12, kDepthR8G8B8A8 = 13, kDepthYCbCr420SP = 43, kDepthYCbCr422SP = 45;
constexpr uint32_t kBlendBypass = 1u << 24, kBlendPremultiplied = 0x00ffff00u, kBlendOver = 0x262;
// The TRM's default filter taps (mainline dc.c tegra_dc_setup_window): 6-tap H, 2-tap V (128 - 8i).
constexpr uint32_t kHFilterTaps[16] = {0x00008000, 0x3e087ce1, 0x3b117ac1, 0x591b73aa, 0x57256d9a, 0x552f668b,
                                       0x73385e8b, 0x72435583, 0x714c4c8b, 0x70554393, 0x715e389b, 0x71662faa,
                                       0x536d25ba, 0x55731bca, 0x387a11d9, 0x3c7c08f1};

inline uint32_t vFilterTap(unsigned i)
{
    return 128 - 8 * i;
}

// Y'CbCr to RGB (WIN_CSC_YOF..KVB, 0x611-0x618): YOF s.7.0, KYRGB 2.8, KUR/KVR/KUB/KVB s.2.8, KUG/KVG s.1.8 (tegra_dc_ext.h).
// Rounded from the standards' Kr/Kb with video range scaled by 255/219 (luma) and 255/224 (chroma); tests check them against
// the float conversion, and BT.601 video range within one step of the TRM default every source programs (f0 12a 0 198 39b 32f
// 204 0). Untagged input uses the same conservative BT.601 policy as VIC.
enum Matrix {
    kBt601,
    kBt709,
    kBt2020
};

constexpr uint32_t kCsc[3][2][8] = {
    {{0x0f0, 0x12a, 0x000, 0x199, 0x39c, 0x330, 0x204, 0x000},
     {0x000, 0x100, 0x000, 0x167, 0x3a8, 0x349, 0x1c6, 0x000}},
    {{0x0f0, 0x12a, 0x000, 0x1cb, 0x3c9, 0x378, 0x21d, 0x000},
     {0x000, 0x100, 0x000, 0x193, 0x3d0, 0x388, 0x1db, 0x000}},
    {{0x0f0, 0x12a, 0x000, 0x1ae, 0x3d0, 0x359, 0x224, 0x000},
     {0x000, 0x100, 0x000, 0x179, 0x3d6, 0x36e, 0x1e2, 0x000}},
};

inline Matrix matrixOf(const Surface &s)
{
    return s.colorMatrix == 2 ? kBt709 : s.colorMatrix == 3 ? kBt2020 : kBt601;
}

// DDA increment, 4.12, as L4T computes it (in / out, window.c compute_dda_inc), so a 4x layer is exactly 4.0 (mainline's
// (in - 1) / (out - 1) goes past the H limit for small 4x layers); 0 when it is over the limit.
inline uint32_t ddaIncrement(uint32_t in, uint32_t out, uint32_t maxWhole)
{
    const uint64_t inc = out ? (static_cast<uint64_t>(in) << 12) / out : 0;
    return inc > static_cast<uint64_t>(maxWhole) << 12 ? 0 : static_cast<uint32_t>(inc);
}

// Everything the driver writes for a detached layer, and the part of its surface the window reads.
struct Image {
    bool visible = false;     // false: the layer lies off the screen (window off)
    uint8_t surfaceKind = 0;  // WINBUF_SURFACE_KIND (pitch: 0)
    uint16_t vOffset = 0;     // ADDR_V_OFFSET (pitch: 0)
    uint32_t colorDepth = 0, position = 0, size = 0, prescaled = 0, dda = 0, lineStride = 0, hOffset = 0, options = 0;
    uint32_t hInitial = 0, vInitial = 0;  // H/V_INITIAL_DDA, 4.12: the first output pixel's source phase
    uint64_t start[2] = {};               // surface byte offsets of the first source row each plane is read from
    uint64_t readLow = 0, readHigh = 0;   // surface bytes the window reads, rounded out to 4 KiB pages
    bool yuv = false;
    Matrix matrix = kBt601;
    bool fullRange = false;
    bool convert = false;  // chroma sited where the window does not sample it: VIC resamples it into BGRA first
    uint8_t byteSwap = 0, lumaBytes = 0, fetchBytes = 0;  // SwitchWindowCaps.h's formats; 0 for the other kinds
};

// One axis of a window: the destination [d, d + dn) on a screen [0, screen), the source [s, s + sn) of a surface `limit`
// long. Output pixel i (from the destination's own first) samples source s + (i + 1/2) * sn / dn - 1/2, Core Animation's own
// mapping of pixel centres: a phase of (inc - 1) / 2 at the first pixel, plus whatever the screen's edge clipped off. The
// window starts on source pixel `first` (even when `even`: a 4:2:0 chroma pair) with phase `initial` and reads `count`.
struct Axis {
    bool visible;
    uint32_t pos, size, first, count, inc, initial;
};

inline bool axisOf(int32_t s, int32_t sn, int32_t d, int32_t dn, uint32_t screen, uint32_t limit, uint32_t maxWhole,
                   bool even, Axis &a)
{
    a = {};
    const int64_t d0 = d < 0 ? 0 : d;
    const int64_t d1 = static_cast<int64_t>(d) + dn > static_cast<int64_t>(screen) ? static_cast<int64_t>(screen)
                                                                                   : static_cast<int64_t>(d) + dn;
    if (d1 <= d0)
        return true;  // nothing on the screen
    a.inc = ddaIncrement(static_cast<uint32_t>(sn), static_cast<uint32_t>(dn), maxWhole);
    if (!a.inc)
        return false;
    a.visible = true;
    a.pos = static_cast<uint32_t>(d0);
    a.size = static_cast<uint32_t>(d1 - d0);
    // The source position of the first visible output pixel, in 4.12 from the source rectangle's start.
    const uint64_t phase = a.inc > 4096 ? (a.inc - 4096) / 2 : 0;
    const uint64_t at = static_cast<uint64_t>(s) * 4096 + phase + static_cast<uint64_t>(d0 - d) * a.inc;
    uint64_t first = at >> 12;
    if (even && (first & 1))
        --first;
    a.first = static_cast<uint32_t>(first);
    a.initial = static_cast<uint32_t>(at - first * 4096);
    // The last output pixel's source pixel, and the one after it the filter may take, within the surface.
    const uint64_t last = (a.initial + static_cast<uint64_t>(a.size - 1) * a.inc) / 4096 + 1;
    const uint64_t end = first + last + 1 > limit ? limit : first + last + 1;
    a.count = static_cast<uint32_t>(end > first ? end - first : 1);
    return a.initial <= 0xffff;
}

// A pitch surface's planes: each one's first source row, and the bytes the window reads from them. halfRows: the chroma
// plane has a row for every two of luma (4:2:0).
inline void pitchReads(const Surface &f, const Axis &x, const Axis &y, uint32_t bpp, bool halfRows, Image &image)
{
    uint64_t low = UINT64_MAX;
    uint64_t high = 0;
    for (unsigned p = 0; p < (image.yuv ? 2u : 1u); ++p) {
        const uint64_t base = image.yuv ? f.offset[p] : 0;
        const uint64_t stride = f.stride[p];
        const bool half = p && halfRows;
        const uint64_t row0 = half ? static_cast<uint64_t>(y.first) / 2 : static_cast<uint64_t>(y.first);
        const uint64_t rows = half ? (static_cast<uint64_t>(y.count) + 1) / 2 : static_cast<uint64_t>(y.count);
        image.start[p] = base + row0 * stride;
        const uint64_t a = image.start[p] + image.hOffset;
        const uint64_t b =
            image.start[p] + (rows - 1) * stride + image.hOffset + static_cast<uint64_t>(x.count) * (p ? 1 : bpp);
        if (a < low)
            low = a;
        if (b > high)
            high = b;
    }
    image.lineStride = image.yuv ? f.stride[1] << 16 | f.stride[0] : f.stride[0];
    image.readLow = low & ~4095ULL;
    image.readHigh = (high + 4095) & ~4095ULL;
}

// A surface the GPU renders block-linear (TegraGM20BLayout.h), 32-bit RGB only: START on the first block row the window
// reads, the rest of the source origin in H_OFFSET (bytes) and V_OFFSET (lines), as mainline dc.c programs a block-linear
// plane from its buffer's base. A block row is one run of bytes across the whole surface, so the reads are whole block rows.
inline bool blockReads(const tgm_layout &b, const Surface &f, const Axis &y, Image &image)
{
    if (b.bpp != 4 || (b.row_bytes % 64) != 0 || b.width < f.width || b.height < f.height || b.y_log2 > 5)
        return false;
    const uint64_t blockRows = 8ULL << b.y_log2;
    const uint64_t blockRowBytes = static_cast<uint64_t>(b.row_bytes) * blockRows;
    const uint64_t first = y.first / blockRows;
    const uint64_t end = (static_cast<uint64_t>(y.first) + y.count + blockRows - 1) / blockRows;
    image.start[0] = first * blockRowBytes;
    image.vOffset = static_cast<uint16_t>(y.first - first * blockRows);
    image.lineStride = b.row_bytes;
    image.surfaceKind =
        static_cast<uint8_t>(kSurfaceKindBlock | static_cast<uint32_t>(b.y_log2) << kSurfaceKindBlockHeightShift);
    image.readLow = image.start[0] & ~4095ULL;
    image.readHigh = (end * blockRowBytes + 4095) & ~4095ULL;
    return image.readHigh <= ((f.descriptorBytes + 4095) & ~4095ULL);
}

#include "SwitchWindowFormatImage.h"

// The image of a layer checkDetached accepted: the destination clipped to the display with the source cut exactly where
// the clipped edge samples it. block: the surface's layout when the GPU renders it block-linear.
inline bool imageOf(const Layer &l, uint32_t width, uint32_t height, Image &image, const tgm_layout *block = nullptr)
{
    if (l.kind == kKindExtraYuv)
        return extraImageOf(l, width, height, image, block);
    image = Image();
    image.yuv = isYuv(l.kind);
    image.convert = image.yuv && !switch_surface_dc_siting(l.surface.chromaTop, l.surface.chromaBottom);
    const uint32_t bpp = image.yuv ? 1 : 4;  // bytes per pixel the offsets and PRESCALED count (luma)
    const uint32_t hMax = image.yuv ? 8 : 4;
    const uint32_t vMax = 15;  // DDA limits (4:2:0 and 4:2:2 count as 2 bytes per pixel)
    Axis x;
    Axis y;
    if (!axisOf(l.src.x, l.src.w, l.dst.x, l.dst.w, width, l.surface.width, hMax, image.yuv, x) ||
        !axisOf(l.src.y, l.src.h, l.dst.y, l.dst.h, height, l.surface.height, vMax, isNv12(l.kind), y))
        return false;
    if (!x.visible || !y.visible)
        return true;
    image.visible = true;
    if (image.yuv) {
        image.colorDepth = isNv12(l.kind) ? kDepthYCbCr420SP : kDepthYCbCr422SP;
    } else {
        image.colorDepth = l.kind == kKindRgba ? kDepthR8G8B8A8 : kDepthB8G8R8A8;
    }
    image.position = y.pos << 16 | x.pos;
    image.size = y.size << 16 | x.size;
    image.prescaled = y.count << 16 | x.count * bpp;
    image.dda = y.inc << 16 | x.inc;
    image.hInitial = x.initial;
    image.vInitial = y.initial;
    image.hOffset = x.first * bpp;
    const bool scaledX = x.inc != 4096 || (x.initial != 0u);
    const bool scaledY = y.inc != 4096 || (y.initial != 0u);
    image.options = kWinEnable | (image.yuv ? kCscEnable : 0) | (scaledX ? kHFilterOn : 0) |
                    (scaledY ? kVFilterOn : 0) | (image.yuv ? 0 : switch_window_palette::options());
    if (block) {
        if (image.yuv || !blockReads(*block, l.surface, y, image))
            return false;
    } else {
        pitchReads(l.surface, x, y, bpp, isNv12(l.kind), image);
    }
    image.matrix = matrixOf(l.surface);
    image.fullRange = l.kind == kKindNv12Full || l.kind == kKindNv16Full;
    return true;
}

// A detached layer: kAccepted when it is one Core Animation may send under the published limits and window B shows exactly.
inline unsigned checkDetached(const Layer &l, uint32_t width, uint32_t height, const Limits &limits = Limits())
{
    if (l.kind == kKindNone)
        return kFormat;
    if (l.transform)
        return kTransformed;
    const Rect s = l.src;
    const Rect d = l.dst;
    if (s.x < 0 || s.y < 0 || s.w <= 0 || s.h <= 0 || d.w <= 0 || d.h <= 0 ||
        static_cast<uint64_t>(s.x) + static_cast<uint64_t>(s.w) > l.surface.width ||
        static_cast<uint64_t>(s.y) + static_cast<uint64_t>(s.h) > l.surface.height ||
        l.surface.width > limits.maxSrcBuffer || l.surface.height > limits.maxSrcBufferHeight)
        return kGeometry;
    // Window coordinates are 13 bits; a layer wholly off the screen is accepted and shown as nothing.
    if (static_cast<int64_t>(d.x) + d.w > 8191 || static_cast<int64_t>(d.y) + d.h > 8191 || d.x < -8191 || d.y < -8191)
        return kGeometry;
    const uint64_t up = limits.maxScale;
    const uint64_t down = isYuv(l.kind) ? limits.yuvMinScaleFraction : limits.minScaleFraction;
    if (static_cast<uint64_t>(d.w) > static_cast<uint64_t>(s.w) * up ||
        static_cast<uint64_t>(d.h) > static_cast<uint64_t>(s.h) * up ||
        static_cast<uint64_t>(s.w) > static_cast<uint64_t>(d.w) * down ||
        static_cast<uint64_t>(s.h) > static_cast<uint64_t>(d.h) * down ||
        static_cast<uint32_t>(s.w) > limits.maxSrcRectWidth ||
        static_cast<uint64_t>(s.w) * static_cast<uint64_t>(s.h) > limits.maxSrcRectTotal)
        return kScale;
    if (l.kind == kKindExtraYuv)
        return extraCheck(l, width, height, limits);
    const unsigned planes = isYuv(l.kind) ? 2 : 1;
    for (unsigned p = 0; p < planes; ++p) {
        const uint64_t base = planes == 1 ? 0 : l.surface.offset[p];
        const uint64_t rows = planes == 1 ? l.surface.height : l.surface.planeHeight[p];
        if (!aligned(base, limits.planeBaseAlignment) || !aligned(l.surface.stride[p], limits.strideAlignment) ||
            l.surface.stride[p] > 0xffff)
            return kAlignment;
        const uint64_t minRow =
            static_cast<uint64_t>(planes == 1 ? l.surface.width * 4 : (l.surface.width + 1) / 2 * 2);
        const uint64_t needRows = p && isNv12(l.kind) ? (l.surface.height + 1) / 2 : l.surface.height;
        if (l.surface.stride[p] < minRow || rows < needRows ||
            base + rows * l.surface.stride[p] > l.surface.allocation ||
            l.surface.allocation > l.surface.descriptorBytes)
            return kSurface;
    }
    Image image;
    return imageOf(l, width, height, image) ? kAccepted : kScale;  // the DDA limits after clipping, exactly as shown
}

// --- The swap's layers --------------------------------------------------------------------------------------------------
// The full-frame UI is above up to two detached layers in the family's stack order.
struct Roles {
    int ui = -1, plane = -1, second = -1;
    unsigned extra = 0;
};

inline Roles rolesOf(const bool present[kLayers], const Layer layers[kLayers], uint32_t width, uint32_t height)
{
    Roles r;
    for (unsigned i = 0; i < kLayers; ++i)
        if (present[i] && isUi(layers[i], width, height) &&
            (r.ui < 0 || stackPosition(i) > stackPosition(unsigned(r.ui))))
            r.ui = int(i);
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!present[i] || int(i) == r.ui)
            continue;
        if (r.ui >= 0 && stackPosition(i) > stackPosition(unsigned(r.ui))) {
            ++r.extra;
        } else if (r.plane < 0 || stackPosition(i) > stackPosition(unsigned(r.plane))) {
            if (r.second >= 0)
                ++r.extra;
            r.second = r.plane;
            r.plane = int(i);
        } else if (r.second < 0 || stackPosition(i) > stackPosition(unsigned(r.second))) {
            if (r.second >= 0)
                ++r.extra;
            r.second = int(i);
        } else {
            ++r.extra;
        }
    }
    return r;
}

// validateSwap's decision on the updated layers (the family checked each source rectangle against its surface, and a
// surface ID that did not resolve refused the swap before this): every updated layer with a surface is the UI or a detached
// layer window B can show; at most one UI among them; at most `maxLayers` with surfaces.
inline unsigned checkUpdated(uint32_t updated, uint32_t background, const bool hasSurface[kLayers],
                             const Layer layers[kLayers], uint32_t width, uint32_t height,
                             const Limits &limits = Limits())
{
    // Core Animation always sets an opaque black background (SwapSetBackgroundColor forces it [V]); the field holds it or 0.
    if ((updated & ~(kLayerBits | kBackgroundBit)) || (background && background != kOpaqueBlack) ||
        ((updated & kBackgroundBit) && background != kOpaqueBlack))
        return kLayerState;
    // A full-frame BGRA layer is the UI, or (the lower of two) a full-screen detached RGB layer window B takes as it is.
    unsigned surfaces = 0;
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!(updated & (1u << i)) || !hasSurface[i])
            continue;
        ++surfaces;
        if (isUi(layers[i], width, height))
            continue;
        const unsigned why = checkDetached(layers[i], width, height, limits);
        if (why)
            return why;
    }
    return surfaces > limits.maxLayers ? kTwoUi : kAccepted;
}

// --- Window B's latch -------------------------------------------------------------------------------------------------------
// Window B's register sets as they move through DC A, the way TegraScanoutRing follows window A: `armed` is the last set
// written (its ACT_REQ out), `superseded` one it replaced before a frame end showed it taken (it may still have been taken at
// that boundary), `active` the one scanned. A set is a small integer the driver keeps (an IOVA slot and its registers), with
// kOff for the window off. At a frame end the driver reports whether WIN_B_ACT_REQ is still set and which set window B's active
// START_ADDR belongs to (kUnknown when two sets share an address); latched() then says which set is scanned now.
struct PlaneLatch {
    static constexpr int kNone = -2, kOff = -1, kUnknown = -3;
    int active = kOff, armed = kNone, superseded = kNone;

    bool requested() const
    {
        return armed != kNone;
    }

    // May a new set be written now? Not while one set is armed and another superseded: which of them the boundary took is
    // still open, so the new one waits for the next frame end (the driver holds it, newest wins).
    bool mayWrite() const
    {
        return superseded == kNone;
    }

    void write(int set)
    {
        if (armed != kNone)
            superseded = armed;
        armed = set;
    }

    // At a frame end with a set written: true when the scanned set changed (`active` is it), in which case every set no
    // longer named here is free.
    bool latched(bool actReqOut, int scanned)
    {
        if (armed == kNone)
            return false;
        if (!actReqOut) {  // everything written is in: the last write wins
            active = armed;
            armed = superseded = kNone;
            return true;
        }
        if (superseded != kNone && scanned != kUnknown && scanned == superseded && scanned != armed) {
            active = superseded;  // the replaced set was taken at this boundary; the newer one is still out
            superseded = kNone;
            return true;
        }
        return false;
    }

    bool busy(int set) const
    {
        return set >= 0 && (set == active || set == armed || set == superseded);
    }

    // SC7, a panic or a stop: the last request is what the window has (or gets back).
    void settle()
    {
        if (armed != kNone)
            active = armed;
        armed = superseded = kNone;
    }
};

}  // namespace native_planes
