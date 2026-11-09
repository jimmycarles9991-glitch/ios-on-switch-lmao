// SPDX-License-Identifier: MIT
// A pass's sample layout as NVK leaves it for a state of that many samples: SET_ANTI_ALIAS (nvk_cmd_set_sample_layout at begin
// rendering), the SET_ANTI_ALIAS_SAMPLE_POSITIONS words and the root table's draw.sample_locations (nvk_flush_ms_state), and
// draw.sample_masks (NVK_MME_SET_ANTI_ALIAS for one shading pass a pixel), which NAK's sample position and sample mask
// lowering read. Metal's default positions are Vulkan's standard ones.
#pragma once
#include <cstdint>
#include "raw_gpu.h"

namespace raw {

constexpr uint32_t kRootSampleInfo = 0x1c;  // offsetof(nvk_root_descriptor_table, draw.sample_locations)
constexpr uint32_t kRootSampleInfoWords = 6;

struct SampleLayout {
    uint32_t antiAlias = 0;  // NV9097_SET_ANTI_ALIAS_SAMPLES_MODE
    uint32_t positions[4] = {};
    uint32_t root[kRootSampleInfoWords] = {};  // eight u4 x | y << 4 locations, then eight 16-bit masks
};

inline bool sampleLayout(uint32_t samples, SampleLayout &out)
{
    // x | y << 4 in sixteenths of a pixel (vk_standard_sample_locations.c)
    static const uint8_t k1[] = {0x88};
    static const uint8_t k2[] = {0xcc, 0x44};
    static const uint8_t k4[] = {0x26, 0x6e, 0xa2, 0xea};
    static const uint8_t k8[] = {0x59, 0xb7, 0x9d, 0x35, 0xd3, 0x71, 0xfb, 0x1f};
    out = SampleLayout();
    const uint8_t *loc;
    switch (samples) {
    case 1:
        loc = k1;  // 1X1
        break;
    case 2:
        loc = k2, out.antiAlias = 5;  // 2X1_D3D
        break;
    case 4:
        loc = k4, out.antiAlias = 2;  // 2X2
        break;
    case 8:
        loc = k8, out.antiAlias = 4;  // 4X2_D3D
        break;
    default:
        return false;
    }
    // the hardware's sixteen positions: the pattern repeated (the standard grid is one pixel)
    for (uint32_t n = 0; n < 16; n++)
        out.positions[n / 4] |= uint32_t(loc[n % samples]) << (8 * (n % 4));
    uint8_t bytes[4 * kRootSampleInfoWords] = {};
    for (uint32_t s = 0; s < samples; s++)
        bytes[s] = loc[s];
    // one sample a pass where a pixel has one, else every sample in the one pass
    for (uint32_t s = 0; s < 8; s++) {
        const uint16_t mask = samples == 1 ? uint16_t(1u << s) : 0xffff;
        bytes[8 + 2 * s] = uint8_t(mask);
        bytes[9 + 2 * s] = uint8_t(mask >> 8);
    }
    for (uint32_t w = 0; w < kRootSampleInfoWords; w++)
        out.root[w] = uint32_t(bytes[4 * w]) | uint32_t(bytes[4 * w + 1]) << 8 | uint32_t(bytes[4 * w + 2]) << 16 |
                      uint32_t(bytes[4 * w + 3]) << 24;
    return true;
}

// The pack's resolve state into a target of MTLPixelFormat `format` (pack/resolve.metal), keyed "raw.resolve.<fragment
// function>"; null for a format it has none for. An 8-bit sRGB target's state writes codes, into its UNORM twin
// (resolveTargetFormat).
inline const char *resolveState(uint32_t format)
{
    switch (format) {
    case 70:
        return "raw.resolve.resolve_rgba8_frag";
    case 71:
        return "raw.resolve.resolve_rgba8_srgb_frag";
    case 80:
        return "raw.resolve.resolve_bgra8_frag";
    case 81:
        return "raw.resolve.resolve_bgra8_srgb_frag";
    case 92:
        return "raw.resolve.resolve_rg11b10f_frag";
    case 115:
        return "raw.resolve.resolve_rgba16f_frag";
    default:
        return nullptr;
    }
}

// The rows of a multisampled target's sample grid (NIL's px_extent_sa: two down for 4 and 8 samples), as NVK's HEIGHT counts
inline uint32_t sampleRows(const Texture &t)
{
    return t.samples >= 4 ? t.height * 2 : t.height;
}

inline uint32_t resolveTargetFormat(uint32_t format)
{
    return format == 71 ? 70 : format == 81 ? 80 : format;
}

}  // namespace raw
