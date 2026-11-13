// SPDX-License-Identifier: MIT
// The barrier vocabulary of GM20B's 3D channel: each function is one hazard's method sequence, the same in the kernel
// and user space.
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

enum switch_gm20b_shader_caches {
    SWITCH_GM20B_SHADER_INSTRUCTIONS = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, INSTRUCTION, TRUE),
    SWITCH_GM20B_SHADER_FLUSH_DATA = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
    SWITCH_GM20B_SHADER_DATA = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, DATA, TRUE),
    SWITCH_GM20B_SHADER_CONSTANTS = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE),
};

#define SWITCH_GM20B_PASS_BARRIER_WORDS 4u

void switch_gm20b_wait_for_idle(struct nv_push *p);

// Waits for idle first: the flush covers every earlier shader store
void switch_gm20b_invalidate_shader_caches(struct nv_push *p, uint32_t caches);

// Acts when the front end reads it, ahead of the work before: only for data no work in flight writes
void switch_gm20b_invalidate_shader_caches_no_wfi(struct nv_push *p, uint32_t caches);

// Texels another engine or an earlier pass wrote, sampled after an idle
void switch_gm20b_invalidate_texture_data(struct nv_push *p);

void switch_gm20b_invalidate_texture_headers(struct nv_push *p);
void switch_gm20b_invalidate_samplers(struct nv_push *p);

// The tiled cache's binned writes reach memory; a wait for idle alone is not known to drain it
void switch_gm20b_tiled_cache_flush(struct nv_push *p);

// A fragment reading what earlier fragments of the same pass wrote (texture barrier, framebuffer fetch)
void switch_gm20b_pixel_shader_barrier(struct nv_push *p);

// Between two passes: the earlier pass's ROP and shader writes reach memory, and the next one's shader and texture
// caches drop what they held, constants included (the copy and 2D engines write constant buffers too)
void switch_gm20b_pass_barrier(struct nv_push *p, bool tiled_cache);

#ifdef __cplusplus
}
#endif
