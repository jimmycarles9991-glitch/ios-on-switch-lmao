// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <pexpert/pexpert.h>
#include "SwitchPortrait.h"

// Hekate's OLED surface is portrait storage rotated counter-clockwise. Publish
// landscape dimensions to the original compositor and rotate only the copy.
// PE_current_console has already decoded boot Video.v_depth=0x320 into depth
// 32 and rotation 3. v_display=0 is stock text mode, not a missing display.
// Portrait-native (SwitchPortrait.h): the compositor gets the panel's own 720x1280 and rotation 3 stays the console's.
static inline bool native_legacy_video_mode(const PE_Video &v)
{
    if (!v.v_baseAddr || !v.v_width || !v.v_height || v.v_depth != 32 || v.v_width > 16384 || v.v_height > 16384 ||
        v.v_rowBytes < v.v_width * 4 || v.v_rowBytes > UINT64_MAX / v.v_height)
        return false;
    return v.v_rotate == 0 || (v.v_rotate == 3 && v.v_width == 720 && v.v_height == 1280 && v.v_rowBytes == 2880);
}

static inline uint32_t native_legacy_video_width(const PE_Video &v)
{
    return static_cast<uint32_t>(v.v_rotate == 3 && !SWITCH_PORTRAIT_NATIVE ? v.v_height : v.v_width);
}

static inline uint32_t native_legacy_video_height(const PE_Video &v)
{
    return static_cast<uint32_t>(v.v_rotate == 3 && !SWITCH_PORTRAIT_NATIVE ? v.v_width : v.v_height);
}

#ifndef NATIVE_SCANOUT_MODE_ONLY
#include "NativeLegacyRequest.h"

static inline bool native_legacy_scanout_layout(const volatile void *destination, uint64_t destinationBytes,
                                                const PE_Video &v, const void *source, uint64_t sourceBytes,
                                                uint64_t sourceStride, uint32_t width, uint32_t height)
{
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    return native_legacy_video_mode(v) && width == native_legacy_video_width(v) &&
           height == native_legacy_video_height(v) && (dst != 0u) && (src != 0u) && ((dst & 3) == 0u) &&
           ((v.v_rowBytes & 3) == 0u) &&
           (native_legacy_surface_bounds(static_cast<uint32_t>(v.v_width), static_cast<uint32_t>(v.v_height),
                                         v.v_rowBytes, destinationBytes, destinationBytes) != 0) &&
           (native_legacy_surface_bounds(width, height, sourceStride, sourceBytes, sourceBytes) != 0) &&
           destinationBytes <= UINTPTR_MAX && sourceBytes <= UINTPTR_MAX && dst <= UINTPTR_MAX - destinationBytes &&
           src <= UINTPTR_MAX - sourceBytes;
}

static inline bool native_legacy_scanout_copy(volatile void *destination, uint64_t destinationBytes, const PE_Video &v,
                                              const void *source, uint64_t sourceBytes, uint64_t sourceStride,
                                              uint32_t width, uint32_t height)
{
    if (!native_legacy_scanout_layout(destination, destinationBytes, v, source, sourceBytes, sourceStride, width,
                                      height))
        return false;
    if (!v.v_rotate || SWITCH_PORTRAIT_NATIVE) {  // portrait-native frames are the panel's own rows
        return native_legacy_copy_pixels(destination, destinationBytes, v.v_rowBytes, source, sourceBytes, sourceStride,
                                         width, height) != 0;
    }
    // Destination rows are contiguous device memory. Read one source column
    // per row and use aligned 32-bit stores, matching the proven OLED console.
    for (uint32_t x = 0; x < width; ++x) {
        volatile uint32_t *out = reinterpret_cast<volatile uint32_t *>(
            static_cast<volatile unsigned char *>(destination) + static_cast<uint64_t>(width - 1 - x) * v.v_rowBytes);
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t pixel = 0;
            __builtin_memcpy(&pixel,
                             static_cast<const unsigned char *>(source) + static_cast<uint64_t>(y) * sourceStride +
                                 static_cast<uint64_t>(x) * 4,
                             4);
            out[y] = pixel;
        }
    }
    return true;
}

// Hardware rotation (NativeLegacyDisplay): the landscape frame is stored
// unrotated, block-linear, and display controller A's window A turns it with SCAN_COLUMN|H_DIRECTION.
// This is hekate's _di_winA_block (bdk/display/di.inl) and Horizon's own scanout: column scan is used
// on block-linear surfaces only (FreeBSD sys/arm/nvidia/drm2/tegra_dc.c refuses 90/270 on pitch ones),
// so the copy swizzles instead of rotating. Panel line j still shows landscape column width-1-j and
// pixel i of it landscape row i, exactly the mapping of the rotate branch above (L4T
// drivers/video/tegra/dc/window.c: SCAN_COLUMN walks the source x per output line, H_DIRECTION from the
// last byte down), so touch and the console's notion of up are unchanged.
//
// Layout (hekate nyx/nyx_gui/gfx/gfx.c gfx_set_rect_land_block, the Maxwell GOB): GOBs of 64 bytes x 8
// lines (512 bytes), BLOCK_HEIGHT(4) = 16 GOBs (128 lines) stacked per block, blocks row-major with
// 1280 * 4 / 64 = 80 blocks per block row. Inside a GOB, byte x of line y lands at
// ((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15). 720 lines fill five
// block rows and 10 GOBs of the sixth; the surface is allocated to the block (768 lines, 0x3C0000
// bytes), as Nyx's is, which stays inside the 4 MiB framebuffer carve-out the loader keeps from XNU
// (scripts/build_native_loader.py NATIVE_FULL_RAM_EXCLUSIONS 'framebuffer' 0xF5A00000 + 0x400000).
//
// Portrait-native (SwitchPortrait.h): the surface is the compositor's own 720x1280 portrait frame in the same GOB and
// block layout, and window A scans it row by row, unrotated (no SCAN_COLUMN, no H_DIRECTION, H_OFFSET 0): 2880-byte
// lines are 45 GOBs, 1280 lines exactly 10 block rows of 45 x 8 KiB, 0x384000 bytes (hekate's pitch frame's size).
// Only SURFACE_KIND then differs from hekate's pitch window. The copies below and the GPU's (TegraScanoutCopy.h) take
// the geometry from these constants.
static constexpr uint32_t kNativeBlockWidth = SWITCH_PORTRAIT_NATIVE ? 720 : 1280,
                          kNativeBlockHeight = SWITCH_PORTRAIT_NATIVE ? 1280 : 720;
static constexpr uint64_t kNativeBlockStride = static_cast<uint64_t>(kNativeBlockWidth) * 4;  // 5120 (2880) bytes
static constexpr uint64_t kNativeBlockGob = 512, kNativeBlockGobs = 16;                       // BLOCK_HEIGHT(4)
static constexpr uint64_t kNativeBlockColumn = kNativeBlockGob * kNativeBlockGobs;            // 8 KiB per block
static constexpr uint64_t kNativeBlockRow = kNativeBlockColumn * (kNativeBlockStride / 64);   // 655360 (368640)
static constexpr uint64_t kNativeBlockBytes = kNativeBlockRow * ((kNativeBlockHeight + 127) / 128);  // 0x3C0000
static_assert(kNativeBlockBytes == (SWITCH_PORTRAIT_NATIVE ? 0x384000 : 0x3C0000) && kNativeBlockBytes <= 0x400000 &&
                  kNativeBlockStride % 64 == 0 && kNativeBlockWidth % 16 == 0 && kNativeBlockHeight % 8 == 0,
              "block-linear surface outgrew the carve-out or is not whole GOBs");

static inline bool native_legacy_block_layout(const volatile void *destination, uint64_t destinationBytes,
                                              const void *source, uint64_t sourceBytes, uint64_t sourceStride,
                                              uint32_t width, uint32_t height)
{
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    return width == kNativeBlockWidth && height == kNativeBlockHeight && (dst != 0u) && (src != 0u) &&
           ((dst & (kNativeBlockGob - 1)) == 0u) && destinationBytes >= kNativeBlockBytes &&
           (native_legacy_surface_bounds(width, height, sourceStride, sourceBytes, sourceBytes) != 0) &&
           destinationBytes <= UINTPTR_MAX && sourceBytes <= UINTPTR_MAX && dst <= UINTPTR_MAX - destinationBytes &&
           src <= UINTPTR_MAX - sourceBytes;
}

// One straight pass, no per-pixel work: every GOB is written front to back in 16-byte pieces (each one
// 16 source bytes of one line), so the write-combined destination sees 512 contiguous bytes per GOB, and
// the reads walk eight source lines forward together.
static inline bool native_legacy_block_copy(void *destination, uint64_t destinationBytes, const void *source,
                                            uint64_t sourceBytes, uint64_t sourceStride, uint32_t width,
                                            uint32_t height)
{
    if (!native_legacy_block_layout(destination, destinationBytes, source, sourceBytes, sourceStride, width, height))
        return false;
    for (uint32_t gobLine = 0; gobLine < height / 8; ++gobLine) {
        const unsigned char *lines =
            static_cast<const unsigned char *>(source) + static_cast<uint64_t>(gobLine) * 8 * sourceStride;
        // Plain stores: the scanout is mapped write-combined (Normal non-cacheable), so the compiler may pair them into 16-byte
        // stp; nothing reads the destination back, and the caller's dsb orders them before the controller can fetch.
        uint64_t *gob = reinterpret_cast<uint64_t *>(static_cast<unsigned char *>(destination) +
                                                     (gobLine / kNativeBlockGobs) * kNativeBlockRow +
                                                     (gobLine % kNativeBlockGobs) * kNativeBlockGob);
        for (uint32_t column = 0; column < width / 16; ++column, gob += kNativeBlockColumn / 8) {
            const unsigned char *in = lines + static_cast<uint64_t>(column) * 64;
            for (unsigned at = 0; at < kNativeBlockGob; at += 16) {
                // Offset bit 8 is x bit 5, bits 7:6 are y bits 2:1, bit 5 is x bit 4, bit 4 is y bit 0.
                const unsigned x = ((at >> 3) & 32) | ((at >> 1) & 16);
                const unsigned y = ((at >> 5) & 6) | ((at >> 4) & 1);
                uint64_t lo = 0;
                uint64_t hi = 0;
                __builtin_memcpy(&lo, in + static_cast<uint64_t>(y) * sourceStride + x, 8);
                __builtin_memcpy(&hi, in + static_cast<uint64_t>(y) * sourceStride + x + 8, 8);
                gob[at / 8] = lo;
                gob[at / 8 + 1] = hi;
            }
        }
    }
    return true;
}

// A solid color is the same in any layout: fill the whole block-linear extent.
static inline bool native_legacy_block_fill(volatile void *destination, uint64_t destinationBytes, uint32_t value)
{
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    if (!dst || (dst & 7) || destinationBytes < kNativeBlockBytes)
        return false;
    volatile uint64_t *out = static_cast<volatile uint64_t *>(destination);
    const uint64_t pair = static_cast<uint64_t>(value) | (static_cast<uint64_t>(value) << 32);
    for (uint64_t i = 0; i < kNativeBlockBytes / 8; ++i)
        out[i] = pair;
    return true;
}

#endif
