// SPDX-License-Identifier: MIT
// See gpu.h. The sampler header fill follows Mesa NVK's nvk_sampler_get_header (MIT).
#include "lock.h"
#include "gpu.h"
#include "sampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// Metal's level clamp bounds as m2v hands them to Vulkan (metal2vulkan metal/sampler.rs vk_sampler: Metal's measured grid of
// sixty-fourths, a nearest mip filter's bounds the levels it reads at them, NotMipmapped as [0, 0.25])
namespace {
float levelOnGrid(float c)
{
    c = std::max(c, 0.0f);
    return c >= 64.0f ? c : std::floor(c * 64.0f) / 64.0f;
}

float nearestLevel(float c)
{
    c = levelOnGrid(c);
    if (c >= 15.0f)
        return 15.0f;
    const float whole = std::floor(c);
    return std::min(15.0f, whole + (c - whole > 0.5f ? 1.0f : 0.0f));
}

// Mesa's __set_ufixed for the 4.8 LOD fields: clamped to what 12 bits hold, truncated
uint32_t lodField(float v)
{
    return uint32_t(std::clamp(v, 0.0f, 4095.0f / 256.0f) * 256.0f) & 0xfff;
}
}  // namespace

// Sampler header (Mesa NVK nvk_sampler_get_header) of the Vulkan sampler m2v makes of the Metal descriptor (vk_sampler): the
// address modes, anisotropy (rounded up to a power of two: MAX_ANISOTROPY, TRILIN_OPT, MIN_ANISO for a linear minification), and
// the LOD clamps on Metal's grid; clampToZero is BORDER with the transparent black border.
void samplerHeader(const SamplerDesc &d, uint32_t w[8])
{
    memset(w, 0, 32);
    auto address = [](uint8_t m) -> uint32_t {  // MTLSamplerAddressMode -> NV9097_TEXSAMP0_ADDRESS_U_*
        switch (m) {
        case 0:
            return 2;  // clampToEdge -> CLAMP_TO_EDGE
        case 1:
            return 5;  // mirrorClampToEdge -> MIRROR_ONCE_CLAMP_TO_EDGE
        case 2:
            return 0;  // repeat -> WRAP
        case 3:
            return 1;  // mirrorRepeat -> MIRROR
        default:
            return 3;  // clampToZero -> BORDER (transparent black)
        }
    };
    // m2v hands anisotropy to a device that samples Metal's way (its profile's device_samples, NVK's) whatever the filters
    uint32_t aniso = 0;  // 0: off
    if (d.maxAnisotropy > 1) {
        aniso = 1;
        while (aniso < d.maxAnisotropy)
            aniso <<= 1;
        aniso = std::min(aniso, 16u);
    }
    setField(w, 2, 0, address(d.sAddress));
    setField(w, 5, 3, address(d.tAddress));
    setField(w, 8, 6, address(d.rAddress));
    setField(w, 13, 13, 1);  // S_R_G_B_CONVERSION
    setField(w, 16, 14, 1);  // FONT_FILTER_WIDTH SIZE_2
    setField(w, 19, 17, 1);  // FONT_FILTER_HEIGHT SIZE_2
    if (aniso)
        setField(w, 22, 20, aniso >= 16 ? 7 : aniso >= 12 ? 6 : aniso >> 1);  // MAX_ANISOTROPY
    setField(w + 1, 2, 0, d.magFilter ? 2 : 1);                               // MAG_LINEAR / MAG_POINT
    setField(w + 1, 5, 4, d.minFilter ? (aniso ? 3 : 2) : 1);                 // MIN_ANISO / MIN_LINEAR / MIN_POINT
    setField(w + 1, 7, 6, d.mipFilter == 2 ? 3 : 2);                          // MIP_LINEAR / MIP_POINT
    setField(w + 1, 9, 9, 1);  // CUBEMAP_INTERFACE_FILTERING AUTO_SPAN_SEAM
    w[1] |= samplerCoordinateBits(d);
    // TRILIN_OPT from the descriptor's maxAnisotropy as Vulkan's (vk_to_9097_trilin_opt: 1 when anisotropy is off)
    const float va = aniso ? float(aniso) : 1.0f;
    setField(w + 1, 30, 26, va >= 12 ? 0 : va >= 4 ? 6 : va >= 2 ? 4 : 0);
    const float lo = d.lodMin, hi = d.lodMax;
    const bool grid = d.mipFilter != 0 && d.minFilter == d.magFilter && d.maxAnisotropy <= 1;
    auto onGrid = [&](float bound, float level) {
        return (!grid && bound > 0.0f && level <= 0.0f) ? bound : level;
    };
    float minLod, maxLod;
    if (d.mipFilter == 0) {
        if (hi > 0.0f) {
            minLod = std::clamp(lo, 0.0f, 0.25f);
            maxLod = 0.25f;
        } else {
            minLod = maxLod = 0.0f;
        }
    } else if (d.mipFilter == 1) {
        minLod = onGrid(lo, nearestLevel(lo));
        maxLod = onGrid(hi, nearestLevel(hi));
    } else {
        minLod = onGrid(lo, levelOnGrid(lo));
        maxLod = onGrid(hi, levelOnGrid(hi));
    }
    maxLod = std::max(maxLod, minLod);
    setField(w + 2, 11, 0, lodField(minLod));
    setField(w + 2, 23, 12, lodField(maxLod));
    if (d.border)
        borderHeader(w, ownBorderTexel(d.border));
}

// lock_ held: a free entry of the sampler header pool
bool Device::takeTsc(uint32_t &tsc)
{
    if (!freeTsc_.empty()) {
        tsc = freeTsc_.back();
        freeTsc_.pop_back();
        return true;
    }
    if (nextTsc_ >= kTscCount)
        return false;
    tsc = nextTsc_++;
    return true;
}

bool Device::takeHeader(const uint32_t words[8], uint32_t &slot)
{
    std::array<uint32_t, 8> header;
    memcpy(header.data(), words, sizeof header);
    if (const auto shared = sharedTsc_.find(header); shared != sharedTsc_.end()) {
        shared->second.users++;
        slot = shared->second.slot;
        return true;
    }
    if (!takeTsc(slot))
        return false;
    memcpy(tsc_.cpu + 32 * slot, words, 32);
    sharedTsc_[header] = {slot, 1};
    tscHeaders_[slot] = header;
    return true;
}

void Device::dropHeader(uint32_t slot)
{
    const auto header = tscHeaders_.find(slot);
    if (header == tscHeaders_.end())
        return;
    const auto shared = sharedTsc_.find(header->second);
    if (shared != sharedTsc_.end() && --shared->second.users)
        return;
    if (shared != sharedTsc_.end())
        sharedTsc_.erase(shared);
    tscHeaders_.erase(header);
    freeTsc_.push_back(slot);
}

bool Device::createSampler(const SamplerDesc &d, Sampler &s)
{
    uint32_t w[8], compared[8];
    samplerHeader(d, w);
    memcpy(compared, w, sizeof w);
    compareHeader(compared, d.compare);
    s.desc = d;
    std::lock_guard<gm20b::Mutex> l(lock_);
    // the sampler's own slot is its name in the argument heap (heapSampler), so never shared; its compare form is
    if (!takeTsc(s.tsc))
        return false;
    if (!takeHeader(compared, s.compareTsc)) {
        freeTsc_.push_back(s.tsc);
        s.tsc = 0;
        return false;
    }
    memcpy(tsc_.cpu + 32 * s.tsc, w, 32);
    return true;
}

void Device::destroySampler(Sampler &s)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (s.tsc)
        freeTsc_.push_back(s.tsc);
    if (s.compareTsc)
        dropHeader(s.compareTsc);
    for (uint32_t m : s.mapped)
        if (m && m != s.tsc)
            dropHeader(m);
    for (uint32_t m : s.compareMapped)
        if (m && m != s.compareTsc)
            dropHeader(m);
    for (uint32_t b : s.bordered)
        if (b)
            dropHeader(b);
    s = {};
}

// m2v's address map of a planar texture (metal2vulkan texture/ycbcr_image.rs `ycbcr_modes`, Metal's measured reads): a 4:2:0
// texture samples the wrapping modes (mirror-clamp-to-edge 1, repeat 2, mirrored repeat 3) as clamp to the edge; one read as
// codes clamp to zero (4) and clamp to the border color (5) too, which a converted one reads as the border (alpha 1: its header's
// W_SOURCE); a 4:4:4 texture read as codes clamp to zero and to the border as clamp to the edge, the others as they are. (r16 sent
// every mode of a converted 4:2:0 texture to the edge: its clamps to zero read the edge texels, not the border m2v reads.)
bool Device::samplerVariant(Sampler &s, uint8_t map)
{
    if (!map || map > kLevelZeroMap)
        return false;
    if (s.mapped[map])
        return true;
    uint32_t own[8], w[8], compared[8];
    samplerHeader(s.desc, own);
    memcpy(w, own, sizeof w);
    mapSamplerHeader(w, map);
    if (!memcmp(w, own, sizeof w)) {
        s.mapped[map] = s.tsc;
        s.compareMapped[map] = s.compareTsc;
        return true;
    }
    memcpy(compared, w, sizeof compared);
    compareHeader(compared, s.desc.compare);
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    uint32_t tsc = 0, compareTsc = 0;
    if (!takeHeader(w, tsc))
        return false;
    if (!takeHeader(compared, compareTsc)) {
        dropHeader(tsc);
        return false;
    }
    s.mapped[map] = tsc;
    s.compareMapped[map] = compareTsc;
    return true;
}

uint8_t samplerBorderTexel(const Sampler &s, const Texture &t)
{
    constexpr uint32_t kA8Unorm = 1;
    const FormatInfo *f = formatInfo(t.format);
    return f ? borderTexel(f->tic0, t.swizzle, t.format == kA8Unorm, s.desc.border) : ownBorderTexel(s.desc.border);
}

bool Device::borderVariant(Sampler &s, const Texture &t)
{
    const uint8_t texel = samplerBorderTexel(s, t);
    if (!s.desc.border || texel == ownBorderTexel(s.desc.border) || s.bordered[texel])
        return true;
    uint32_t w[8];
    samplerHeader(s.desc, w);
    borderHeader(w, texel);
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    uint32_t tsc = 0;
    if (!takeHeader(w, tsc))
        return false;
    s.bordered[texel] = tsc;
    return true;
}

bool Device::constexprMapsPlaced()
{
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    for (; constexprMappedCount_ < constexprCount_; constexprMappedCount_++) {
        const auto placed = std::find_if(completedSamplers_.begin(), completedSamplers_.end(), [&](const auto &s) {
            return s.second == constexprMappedCount_;
        });
        if (placed == completedSamplers_.end() || !placeConstexprMaps(constexprMappedCount_, placed->first.data()))
            return false;
    }
    return true;
}

bool Device::constexprLevelZeroPlaced()
{
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    for (; constexprLevelZeroCount_ < constexprCount_; constexprLevelZeroCount_++) {
        const uint32_t index = constexprLevelZeroCount_, own = constexprTsc_[index];
        uint32_t words[8], w[8];
        memcpy(words, tsc_.cpu + 32 * own, sizeof words);
        memcpy(w, words, sizeof w);
        levelZeroHeader(w);
        uint32_t tsc = own;
        if (memcmp(w, words, sizeof w) && !takeHeader(w, tsc))
            return false;
        constexprMapped_[index][kLevelZeroMap] = tsc;
    }
    return true;
}

bool Device::placeConstexprMaps(uint32_t index, const uint32_t words[8])
{
    const uint32_t own = constexprTsc_[index];
    uint32_t taken[4] = {};
    for (uint8_t map = 1; map < 4; map++) {
        uint32_t w[8];
        memcpy(w, words, sizeof w);
        mapSamplerAddresses(w, map);
        if (!memcmp(w, words, sizeof w)) {
            constexprMapped_[index][map] = own;
            continue;
        }
        if (!takeTsc(taken[map])) {
            for (uint32_t tsc : taken)
                if (tsc)
                    freeTsc_.push_back(tsc);
            return false;
        }
        memcpy(tsc_.cpu + 32 * taken[map], w, sizeof w);
        constexprMapped_[index][map] = taken[map];
    }
    constexprMapped_[index][0] = own;
    return true;
}

}  // namespace gm20b
