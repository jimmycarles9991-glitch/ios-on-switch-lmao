// SPDX-License-Identifier: MIT
// The copy engine's rectangle copy (mesa/nouveau_copy_rect.c, NVK's), for the kernel and user space alike.
#pragma once

#include "../SwitchGM20B/SwitchGM20BCopy.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gm20b_copy_surface {
    uint64_t base_addr;
    bool is_3d;

    struct {
        uint32_t x, y, z, a;
    } offset_el;

    struct {
        uint32_t width, height, depth, array_len;
    } extent_el;

    uint32_t bpp, row_stride, array_stride;
    // Block-linear with 8-row GOBs, one GOB wide, 2^y_log2 GOBs tall and 2^z_log2 deep; else pitch-linear
    bool tiled;
    uint8_t y_log2, z_log2;
};

struct switch_gm20b_copy {
    struct switch_gm20b_copy_surface src, dst;

    // comp_size 0: no remap, lengths in bytes; else each texel as bpp / comp_size components of comp_size bytes,
    // destination component i from dst[i] (NVB0B5_SET_REMAP_COMPONENTS_DST_X_*)
    struct {
        uint8_t comp_size;
        uint8_t dst[4];
    } remap;

    struct {
        uint32_t width, height, depth, array_len;
    } extent_el;
};

#define SWITCH_GM20B_COPY_RECT_WORDS_PER_LAYER 25u
#define SWITCH_GM20B_COPY_RECT_WORDS(layers)   (2u + SWITCH_GM20B_COPY_RECT_WORDS_PER_LAYER * (layers))

// data_transfer_type: NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED, or _NON_PIPELINED to start after the copy before
void switch_gm20b_copy_rect(struct nv_push *p, const struct switch_gm20b_copy *copy, uint8_t data_transfer_type);

#ifdef __cplusplus
}
#endif
