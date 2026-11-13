// SPDX-License-Identifier: MIT
// Copyright © 2022 Collabora Ltd. and Red Hat Inc.
// The texture sampler header (TSC) of a sampler, packed as NVK's nvk_sampler_get_header (nvk_sampler.c) packs it on
// MAXWELL_B, from the state vk_sampler_state_init makes of a VkSamplerCreateInfo with a non-custom border colour and no
// sRGB border mapping (what metal2vulkan creates).
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "SwitchGM20B3D.h"
#include "SwitchGM20BTexture.h"

#ifdef __cplusplus
extern "C" {
#endif

// The Vulkan values of the state's enumerations
enum {
    SWITCH_GM20B_FILTER_NEAREST = 0,
    SWITCH_GM20B_FILTER_LINEAR = 1,
    SWITCH_GM20B_MIPMAP_NEAREST = 0,
    SWITCH_GM20B_MIPMAP_LINEAR = 1,
    SWITCH_GM20B_ADDRESS_MIRROR_CLAMP_TO_EDGE = 4,
    SWITCH_GM20B_COMPARE_ALWAYS = 7,
    SWITCH_GM20B_BORDER_INT_OPAQUE_WHITE = 5,
    SWITCH_GM20B_REDUCTION_MAX = 2,
};

struct switch_gm20b_sampler_state {
    uint32_t mag_filter, min_filter, mipmap_mode;  // VkFilter, VkFilter, VkSamplerMipmapMode
    uint32_t address[3];                           // VkSamplerAddressMode of u, v and w
    float mip_lod_bias, min_lod, max_lod;
    bool anisotropy;
    float max_anisotropy;
    bool compare;
    uint32_t compare_op;            // VkCompareOp
    uint32_t border_color;          // VkBorderColor
    uint32_t reduction;             // VkSamplerReductionMode
    uint32_t border_components[4];  // VkComponentSwizzle of r, g, b, a (identity: no border mapping)
    bool unnormalized;
};

static inline void switch_gm20b_tsc_put(uint32_t *word, uint32_t value, unsigned lo, unsigned hi)
{
    *word |= (value & (uint32_t)(~0ull >> (63 - (hi - lo)))) << lo;
}

#define SWITCH_GM20B_TSC(w, i, FIELD, v) \
    switch_gm20b_tsc_put(&(w)[i], (v), DRF_LO(NVB197_TEXSAMP##i##_##FIELD), DRF_HI(NVB197_TEXSAMP##i##_##FIELD))

static inline uint32_t switch_gm20b_tsc_fixed(float v, unsigned lo, unsigned hi, bool sign)
{
    const float factor = 256.0f;
    const unsigned bits = hi - lo + 1;
    const float max = (float)((1ll << (bits - (sign ? 1 : 0))) - 1) / factor;
    const float min = sign ? -(float)(1ll << (bits - 1)) / factor : 0.0f;
    const float clamped = v < min ? min : v > max ? max : v;
    return (uint32_t)llroundf(clamped * factor);
}

static inline uint32_t switch_gm20b_tsc_max_anisotropy(float max)
{
    if (max >= 16)
        return NVB197_TEXSAMP0_MAX_ANISOTROPY_ANISO_16_TO_1;
    if (max >= 12)
        return NVB197_TEXSAMP0_MAX_ANISOTROPY_ANISO_12_TO_1;
    return (uint32_t)(max > 0.0f ? max : 0.0f) >> 1;
}

static inline uint32_t switch_gm20b_tsc_trilinear(float max_anisotropy)
{
    return max_anisotropy >= 12 ? 0 : max_anisotropy >= 4 ? 6 : max_anisotropy >= 2 ? 4 : 0;
}

static inline uint32_t switch_gm20b_border_component(const uint32_t color[4], uint32_t swizzle, uint32_t own,
                                                     bool integer)
{
    if (swizzle == SWITCH_GM20B_SWIZZLE_IDENTITY)
        return color[own];
    if (swizzle == SWITCH_GM20B_SWIZZLE_ZERO)
        return 0;
    if (swizzle == SWITCH_GM20B_SWIZZLE_ONE)
        return integer ? 1 : 0x3f800000;
    return color[swizzle - SWITCH_GM20B_SWIZZLE_R];
}

// Mesa's util_format_linear_float_to_srgb_8unorm over the only values a non-custom border colour holds: 0 and 1.0, and
// an integer 1, whose bits read as a float lie below sRGB's first step.
static inline uint32_t switch_gm20b_border_srgb8(uint32_t bits)
{
    return bits == 0x3f800000 ? 255 : 0;
}

// False for a state outside the Vulkan values NVK packs
static inline bool switch_gm20b_sampler_header(const struct switch_gm20b_sampler_state *s, uint32_t w[8])
{
    for (int i = 0; i < 8; i++)
        w[i] = 0;
    for (int i = 0; i < 3; i++)
        if (s->address[i] > SWITCH_GM20B_ADDRESS_MIRROR_CLAMP_TO_EDGE)
            return false;
    if (s->mag_filter > SWITCH_GM20B_FILTER_LINEAR || s->min_filter > SWITCH_GM20B_FILTER_LINEAR ||
        s->mipmap_mode > SWITCH_GM20B_MIPMAP_LINEAR || s->compare_op > SWITCH_GM20B_COMPARE_ALWAYS ||
        s->border_color > SWITCH_GM20B_BORDER_INT_OPAQUE_WHITE || s->reduction > SWITCH_GM20B_REDUCTION_MAX)
        return false;
    for (int i = 0; i < 4; i++)
        if (s->border_components[i] > SWITCH_GM20B_SWIZZLE_A)
            return false;
    static const uint8_t address[] = {
        NVB197_TEXSAMP0_ADDRESS_U_WRAP,
        NVB197_TEXSAMP0_ADDRESS_U_MIRROR,
        NVB197_TEXSAMP0_ADDRESS_U_CLAMP_TO_EDGE,
        NVB197_TEXSAMP0_ADDRESS_U_BORDER,
        NVB197_TEXSAMP0_ADDRESS_U_MIRROR_ONCE_CLAMP_TO_EDGE,
    };
    SWITCH_GM20B_TSC(w, 0, ADDRESS_U, address[s->address[0]]);
    SWITCH_GM20B_TSC(w, 0, ADDRESS_V, address[s->address[1]]);
    SWITCH_GM20B_TSC(w, 0, ADDRESS_P, address[s->address[2]]);
    if (s->compare) {
        SWITCH_GM20B_TSC(w, 0, DEPTH_COMPARE, 1);
        SWITCH_GM20B_TSC(w, 0, DEPTH_COMPARE_FUNC, s->compare_op);
    }
    SWITCH_GM20B_TSC(w, 0, S_R_G_B_CONVERSION, 1);
    // Fermi's font filter width and height (cl9097tex.h FONT_FILTER_*_SIZE_2), which NVK still writes
    SWITCH_GM20B_TSC(w, 0, RESERVED0A, 1);
    SWITCH_GM20B_TSC(w, 0, RESERVED0B, 1);
    const float max_anisotropy = s->anisotropy ? s->max_anisotropy : 1.0f;
    if (s->anisotropy)
        SWITCH_GM20B_TSC(w, 0, MAX_ANISOTROPY, switch_gm20b_tsc_max_anisotropy(max_anisotropy));
    SWITCH_GM20B_TSC(w, 1, MAG_FILTER,
                     s->mag_filter ? NVB197_TEXSAMP1_MAG_FILTER_MAG_LINEAR : NVB197_TEXSAMP1_MAG_FILTER_MAG_POINT);
    SWITCH_GM20B_TSC(w, 1, MIN_FILTER,
                     !s->min_filter  ? NVB197_TEXSAMP1_MIN_FILTER_MIN_POINT
                     : s->anisotropy ? NVB197_TEXSAMP1_MIN_FILTER_MIN_ANISO
                                     : NVB197_TEXSAMP1_MIN_FILTER_MIN_LINEAR);
    SWITCH_GM20B_TSC(w, 1, MIP_FILTER,
                     s->mipmap_mode ? NVB197_TEXSAMP1_MIP_FILTER_MIP_LINEAR : NVB197_TEXSAMP1_MIP_FILTER_MIP_POINT);
    SWITCH_GM20B_TSC(w, 1, CUBEMAP_INTERFACE_FILTERING, NVB197_TEXSAMP1_CUBEMAP_INTERFACE_FILTERING_AUTO_SPAN_SEAM);
    SWITCH_GM20B_TSC(w, 1, REDUCTION_FILTER, s->reduction);
    SWITCH_GM20B_TSC(w, 1, MIP_LOD_BIAS,
                     switch_gm20b_tsc_fixed(s->mip_lod_bias, DRF_LO(NVB197_TEXSAMP1_MIP_LOD_BIAS),
                                            DRF_HI(NVB197_TEXSAMP1_MIP_LOD_BIAS), true));
    SWITCH_GM20B_TSC(w, 1, FLOAT_COORD_NORMALIZATION,
                     s->unnormalized ? NVB197_TEXSAMP1_FLOAT_COORD_NORMALIZATION_FORCE_UNNORMALIZED_COORDS
                                     : NVB197_TEXSAMP1_FLOAT_COORD_NORMALIZATION_USE_HEADER_SETTING);
    SWITCH_GM20B_TSC(w, 1, TRILIN_OPT, switch_gm20b_tsc_trilinear(max_anisotropy));
    SWITCH_GM20B_TSC(w, 2, MIN_LOD_CLAMP,
                     switch_gm20b_tsc_fixed(s->min_lod, DRF_LO(NVB197_TEXSAMP2_MIN_LOD_CLAMP),
                                            DRF_HI(NVB197_TEXSAMP2_MIN_LOD_CLAMP), false));
    SWITCH_GM20B_TSC(w, 2, MAX_LOD_CLAMP,
                     switch_gm20b_tsc_fixed(s->max_lod, DRF_LO(NVB197_TEXSAMP2_MAX_LOD_CLAMP),
                                            DRF_HI(NVB197_TEXSAMP2_MAX_LOD_CLAMP), false));
    const bool integer = s->border_color & 1;
    const uint32_t one = integer ? 1 : 0x3f800000;
    const uint32_t black = s->border_color >= 2 ? one : 0, white = s->border_color >= 4 ? one : 0;
    const uint32_t color[4] = {white, white, white, black};
    uint32_t border[4];
    for (uint32_t i = 0; i < 4; i++)
        border[i] = switch_gm20b_border_component(color, s->border_components[i], i, integer);
    SWITCH_GM20B_TSC(w, 2, S_R_G_B_BORDER_COLOR_R, switch_gm20b_border_srgb8(border[0]));
    SWITCH_GM20B_TSC(w, 3, S_R_G_B_BORDER_COLOR_G, switch_gm20b_border_srgb8(border[1]));
    SWITCH_GM20B_TSC(w, 3, S_R_G_B_BORDER_COLOR_B, switch_gm20b_border_srgb8(border[2]));
    SWITCH_GM20B_TSC(w, 4, BORDER_COLOR_R, border[0]);
    SWITCH_GM20B_TSC(w, 5, BORDER_COLOR_G, border[1]);
    SWITCH_GM20B_TSC(w, 6, BORDER_COLOR_B, border[2]);
    SWITCH_GM20B_TSC(w, 7, BORDER_COLOR_A, border[3]);
    return true;
}

#ifdef __cplusplus
}
#endif
