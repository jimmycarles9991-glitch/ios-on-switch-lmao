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

#define SWITCH_GM20B_PASS_BARRIER_WORDS 3u
#define SWITCH_GM20B_PASS_DRAIN_WORDS   3u
#define SWITCH_GM20B_KIND_SWITCH_WORDS  12u

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

// Same barrier with SYSMEMBAR_ENABLE_TRUE so VOL=1 targets that bypass L2 drain ROP writes to DRAM before TEX reads
void switch_gm20b_pixel_shader_barrier_sysmem(struct nv_push *p);

// In-pass memory barrier: INVALIDATE_SHADER_CACHES with FLUSH_DATA waits for idle on Maxwell before flushing SM L1
void switch_gm20b_pass_barrier(struct nv_push *p, bool tiled_cache);

// Pass-to-pass drain when the prior pass already flushed its tiled cache and SM stores: wait for idle, then
// non-waiting cache invalidates (preserving constants unless a prior shader/engine wrote a uniform buffer)
void switch_gm20b_pass_drain(struct nv_push *p, bool invalidate_constants);

// Between two accesses of the same pages through PTE kinds that place a GOB's bytes otherwise (kind 0 and 0xfe, or
// either and a zeta kind), after the first has completed: the L2's lines of them written back, then dropped. A line
// keeps the placement of the access that brought it in, so the second kind's reads and writes would otherwise take
// the first kind's GOB order (on the device: TegraColor texels read back in FermiColor order).
void switch_gm20b_kind_switch(struct nv_push *p);

#ifdef __cplusplus
}
#endif
