// SPDX-License-Identifier: MIT
// MAXWELL_B state sequences: render targets and the render area, viewports and scissors, clears, blending.
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gm20b_target {
    uint64_t address;
    // Pitch: the row's bytes. Block-linear: the row stride in texels
    uint32_t width, height;
    uint8_t format;  // NVB197_SET_COLOR_TARGET_FORMAT_V_*; DISABLED for an unused slot below the last target
    bool pitch;
    uint8_t block_height_log2;
    uint32_t layers;
    uint32_t array_pitch;  // block-linear: the layer size / 4
    uint32_t layer;
    // A 3D image's level (NVK: THIRD_DIMENSION_DEFINES_DEPTH_SIZE, its blocks' depth); layers then counts its planes
    bool depth;
    uint8_t block_depth_log2;
};

#define SWITCH_GM20B_COLOR_TARGET_WORDS 12u

void switch_gm20b_color_target(struct nv_push *p, uint32_t index, const struct switch_gm20b_target *target,
                               bool compressed);

// Targets 0..count-1, each its own slot, and the area drawn in from (0, 0)
void switch_gm20b_render_area(struct nv_push *p, uint32_t count, uint32_t width, uint32_t height);

struct switch_gm20b_viewport_state {
    float scale[3], offset[3];
    uint16_t clip_x, clip_width, clip_y, clip_height;
};

#define SWITCH_GM20B_VIEWPORT_WORDS 10u

void switch_gm20b_viewport(struct nv_push *p, uint32_t index, const struct switch_gm20b_viewport_state *v);

// Viewport `index`'s clip-space swizzle (NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE: X in bits 2:0, Y 6:4, Z 10:8, W 14:12)
void switch_gm20b_viewport_swizzle(struct nv_push *p, uint32_t index, uint32_t swizzle);

// Inclusive minimum, exclusive maximum
void switch_gm20b_scissor(struct nv_push *p, uint32_t index, uint16_t xmin, uint16_t xmax, uint16_t ymin,
                          uint16_t ymax);

void switch_gm20b_blend_constant(struct nv_push *p, const float rgba[4]);

struct switch_gm20b_clear_state {
    uint32_t value[4];  // the target format's clear words (a float's bits, or packed integers)
    uint16_t xmin, xmax, ymin, ymax;
    uint32_t target;
    bool depth, stencil;
    // A layered pass's: its layers from `layer` on, each cleared by the channel's layer loop macro at slot `layer_macro`
    // (NVK's nvk_mme_clear) where there are more than one
    uint32_t layer, layers, layer_macro;
};

#define SWITCH_GM20B_CLEAR_WORDS 13u

// Clears one colour target's R, G, B and A (and depth, stencil) over the rectangle in its layers, whatever the render enable
// says
void switch_gm20b_clear(struct nv_push *p, const struct switch_gm20b_clear_state *c);

struct switch_gm20b_blend_state {
    // NVB197_SET_BLEND_PER_TARGET_COLOR_OP_V_OGL_* and _COLOR_SOURCE_COEFF_V_OGL_* values
    uint32_t color_op, color_source, color_dest, alpha_op, alpha_source, alpha_dest;
};

#define SWITCH_GM20B_BLEND_WORDS 11u

// Target `index`'s blend: enabled with `b`, or off (NULL)
void switch_gm20b_blend(struct nv_push *p, uint32_t index, const struct switch_gm20b_blend_state *b);

// RGBA write enables, red in bit 0
void switch_gm20b_ct_write(struct nv_push *p, uint32_t index, uint32_t rgba);

#ifdef __cplusplus
}
#endif
