// SPDX-License-Identifier: MIT
// Copyright © 2024 Collabora, Ltd.
// Word 0 of the block-linear texture header of a view (component sizes, data types and sources), as NIL's
// nvb097_set_th_bl_0 (nil/descriptor.rs) writes it: the format's own sources (SwitchGM20BVkFormats.h) under the view's
// component mapping. No address, extent or view type reaches it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../../third_party/mesa/src/nouveau/headers/drf.h"
#include "SwitchGM20B3D.h"
#include "SwitchGM20BVkFormats.h"

#ifdef __cplusplus
extern "C" {
#endif

// VkComponentSwizzle
enum {
    SWITCH_GM20B_SWIZZLE_IDENTITY = 0,
    SWITCH_GM20B_SWIZZLE_ZERO = 1,
    SWITCH_GM20B_SWIZZLE_ONE = 2,
    SWITCH_GM20B_SWIZZLE_R = 3,
    SWITCH_GM20B_SWIZZLE_A = 6,
};

static inline const struct switch_gm20b_vk_format *switch_gm20b_vk_format(uint32_t vk)
{
    uint32_t lo = 0, hi = SWITCH_GM20B_VK_FORMATS;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (switch_gm20b_vk_formats[mid].vk < vk)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < SWITCH_GM20B_VK_FORMATS && switch_gm20b_vk_formats[lo].vk == vk ? &switch_gm20b_vk_formats[lo] : 0;
}

#define SWITCH_GM20B_TIC0_SOURCE(i) (DRF_LO(DRF_MW(NVB197_TEXHEAD_BL_X_SOURCE)) + 3u * (i))

// `components`: the view's VkComponentSwizzle of r, g, b, a. False for a format NIL has no row for, or a storage view
// of a format NIL does not store.
static inline bool switch_gm20b_view_word0(uint32_t vk_format, const uint32_t components[4], bool storage,
                                           uint32_t *word0)
{
    const struct switch_gm20b_vk_format *f = switch_gm20b_vk_format(vk_format);
    if (!f || (storage && !(f->flags & SWITCH_GM20B_VK_FORMAT_STORAGE)))
        return false;
    uint32_t own[4], word = f->tic0;
    for (uint32_t i = 0; i < 4; i++) {
        own[i] = f->tic0 >> SWITCH_GM20B_TIC0_SOURCE(i) & 7;
        word &= ~(7u << SWITCH_GM20B_TIC0_SOURCE(i));
    }
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t source;
        if (components[i] > SWITCH_GM20B_SWIZZLE_A)
            return false;
        if (components[i] == SWITCH_GM20B_SWIZZLE_IDENTITY)
            source = own[i];
        else if (components[i] == SWITCH_GM20B_SWIZZLE_ZERO)
            source = NVB197_TEXHEAD_BL_X_SOURCE_IN_ZERO;
        else if (components[i] == SWITCH_GM20B_SWIZZLE_ONE)
            source = f->flags & SWITCH_GM20B_VK_FORMAT_INTEGER ? NVB197_TEXHEAD_BL_X_SOURCE_IN_ONE_INT
                                                               : NVB197_TEXHEAD_BL_X_SOURCE_IN_ONE_FLOAT;
        else
            source = own[components[i] - SWITCH_GM20B_SWIZZLE_R];
        word |= source << SWITCH_GM20B_TIC0_SOURCE(i);
    }
    *word0 = word;
    return true;
}

#ifdef __cplusplus
}
#endif
