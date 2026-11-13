// SPDX-License-Identifier: BSD-2-Clause-Patent
// What T210 DC A's windows B and C scan beyond NativePlanes.h's NV12, NV16 and 32-bit RGB, and the largest source they
// take. Plain C++, no dependencies; NativePlanes.h reads both.
//
// COLOR_DEPTH codes. L4T writes its tegra_dc_ext.h format number straight into DC_WIN_COLOR_DEPTH (window.c,
// tegra_dc_fmt) and names it by the components of a little-endian word from the top: 12 A8R8G8B8 is the bytes B, G, R,
// A in memory (mainline's B8G8R8A8). Read that way 43 Y8___V8U8_N420 has Cb in a pair's low byte, so first in memory:
// NV12, which is what mainline (dc.h WIN_COLOR_DEPTH_YCbCr420SP 43, plane.c DRM_FORMAT_NV12) and hekate (di.h 0x2B)
// call it; 42 is NV21. The same reading and the same two sources give 49 for NV24 (mainline YCbCr444SP, plane.c
// DRM_FORMAT_NV24; L4T Y8___V8U8_N444, one of the two 4:4:4 semi-planar codes T210 adds to T124's, dc_config.h
// TEGRA_WIN_FMT_T210_HIGH) and 16 for packed 4:2:2: UYVY ('2vuy': Cb Y0 Cr Y1) as it lies, YUYV ('yuvs', 'yuvf': Y0 Cb
// Y1 Cr) with BYTE_SWAP SWAP2, the bytes of each 16-bit half exchanged (plane.c DRM_FORMAT_UYVY / DRM_FORMAT_YUYV). All
// are in T210 windows A-C's format masks (dc_config.c t210_feature_entries_a) and mainline's tegra210 overlay list
// (dc.c tegra114_overlay_formats).
//
// Source size. L4T gives windows A-C TEGRA_DC_FEATURE_MAXIMUM_SIZE {4096, 1, 4096, 1} with both filters
// (dc_config.c t210_feature_entries_a), and window.c bounds only the DDA increments: H 4.0 at 4 bytes a pixel, 8.0 at
// 1-2 (a 4:2:0 or 4:2:2 window counts as 2), V 15.0 (compute_dda_inc); mainline's T210 max_width is 4096. A YUV layer is
// published down to 1/6 (a 4096-wide picture across the 720-pixel panel is 5.7x), not the DDA's 1/8: one 4:2:0 or 4:2:2
// window at 1/6 on both axes fetches 78 MHz x 2 bytes x 36 = 5.6 GB/s, and two with the UI are the 1600 MHz EMC rate's
// isochronous ceiling (SwitchDisplayBandwidth.h floorKhz: 11.5 GB/s at 45%), where 1/8 would need twice the rate. RGB
// stays at the 4.0 H limit. Sources stop at 2304 rows so that the largest 32-bit one, 4096 x 2304 at 16 KiB a row
// (36 MiB), fits one plane slot.
#pragma once
#include <stdint.h>

namespace switch_window_caps {

constexpr uint32_t kSourceWidth = 4096, kSourceHeight = 2304;
constexpr uint64_t kSourcePixels = static_cast<uint64_t>(kSourceWidth) * kSourceHeight;
constexpr uint32_t kYuvMinScaleFraction = 6;
// The IOVA span of one plane slot: the largest source's rows plus a page either side, in whole 4 MiB SMMU tables.
constexpr uint64_t kPlaneSlotSpan = 40ULL << 20;
// The display domain's tables: 8 plane slots of 10 tables, the direct-scanout and probe slots, and the identity maps.
constexpr unsigned kDomainTables = 128;

constexpr uint32_t kByteSwapNone = 0, kByteSwap2 = 1;

struct Format {
    uint32_t fourcc;
    uint8_t planes;       // memory planes: 2 semi-planar, 1 packed
    uint8_t depth;        // COLOR_DEPTH
    uint8_t byteSwap;     // BYTE_SWAP
    uint8_t lumaBytes;    // bytes a pixel in the first plane: what H_OFFSET and PRESCALED_SIZE count
    uint8_t chromaBytes;  // bytes a pixel in the second plane
    uint8_t fetchBytes;   // bytes a pixel the window fetches, for its memory bandwidth
    bool pairs;           // chroma shared by two pixels across: the first pixel read is even
    bool fullRange;
};

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24 |
           static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(d));
}

constexpr uint32_t kDepthYCbCr422 = 16, kDepthYCbCr444SP = 49;

constexpr Format kFormats[] = {
    {fourcc('4', '4', '4', 'v'), 2, kDepthYCbCr444SP, kByteSwapNone, 1, 2, 3, false, false},
    {fourcc('4', '4', '4', 'f'), 2, kDepthYCbCr444SP, kByteSwapNone, 1, 2, 3, false, true},
    {fourcc('2', 'v', 'u', 'y'), 1, kDepthYCbCr422, kByteSwapNone, 2, 0, 2, true, false},
    {fourcc('y', 'u', 'v', 's'), 1, kDepthYCbCr422, kByteSwap2, 2, 0, 2, true, false},
    {fourcc('y', 'u', 'v', 'f'), 1, kDepthYCbCr422, kByteSwap2, 2, 0, 2, true, true},
};

// The format of an IOSurface with `planes` planes (0 for a packed surface, as IOSurface counts one), or null.
inline const Format *find(uint32_t format, uint32_t planes)
{
    for (const Format &f : kFormats) {
        if (f.fourcc == format && (f.planes == 2 ? planes == 2 : planes <= 1))
            return &f;
    }
    return nullptr;
}

}  // namespace switch_window_caps

// The WIN_OPTIONS bits every window carries for its palette: CP_ENABLE while TegraDisplayColor shows a per-channel gamma
// table through the palettes (TegraDisplayPalette.h), set by the display's frame-end action under the gate and read
// where window options are made.
namespace switch_window_palette {

inline uint32_t gOptions = 0;

inline uint32_t options()
{
    return __atomic_load_n(&gOptions, __ATOMIC_RELAXED);
}

inline void setOptions(uint32_t value)
{
    __atomic_store_n(&gOptions, value, __ATOMIC_RELAXED);
}

}  // namespace switch_window_palette
