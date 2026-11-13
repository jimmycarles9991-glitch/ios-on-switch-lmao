// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmitBarrier.h"

#include "nv_push_clb197.h"

static bool has(uint32_t caches, uint32_t cache)
{
    return (caches & cache) != 0;
}

void switch_gm20b_wait_for_idle(struct nv_push *p)
{
    P_IMMD(p, NVB197, WAIT_FOR_IDLE, 0);
}

void switch_gm20b_invalidate_shader_caches(struct nv_push *p, uint32_t caches)
{
    P_IMMD(p, NVB197, INVALIDATE_SHADER_CACHES,
           {
               .instruction = has(caches, SWITCH_GM20B_SHADER_INSTRUCTIONS),
               .data = has(caches, SWITCH_GM20B_SHADER_DATA),
               .constant = has(caches, SWITCH_GM20B_SHADER_CONSTANTS),
               .locks = 0,
               .flush_data = has(caches, SWITCH_GM20B_SHADER_FLUSH_DATA),
           });
}

void switch_gm20b_invalidate_shader_caches_no_wfi(struct nv_push *p, uint32_t caches)
{
    assert(!has(caches, SWITCH_GM20B_SHADER_FLUSH_DATA));
    P_IMMD(p, NVB197, INVALIDATE_SHADER_CACHES_NO_WFI,
           {
               .instruction = has(caches, SWITCH_GM20B_SHADER_INSTRUCTIONS),
               .global_data = has(caches, SWITCH_GM20B_SHADER_DATA),
               .constant = has(caches, SWITCH_GM20B_SHADER_CONSTANTS),
           });
}

void switch_gm20b_invalidate_texture_data(struct nv_push *p)
{
    P_IMMD(p, NVB197, INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, {.lines = LINES_ALL, .tag = 0});
}

void switch_gm20b_invalidate_texture_headers(struct nv_push *p)
{
    P_IMMD(p, NVB197, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {.lines = LINES_ALL, .tag = 0});
}

void switch_gm20b_invalidate_samplers(struct nv_push *p)
{
    P_IMMD(p, NVB197, INVALIDATE_SAMPLER_CACHE_NO_WFI, {.lines = LINES_ALL, .tag = 0});
}

void switch_gm20b_tiled_cache_flush(struct nv_push *p)
{
    __push_immd(p, SUBC_NVB197, SWITCH_NVB197_TILED_CACHE_FLUSH, 0);
}

void switch_gm20b_pixel_shader_barrier(struct nv_push *p)
{
    P_IMMD(p, NVB197, PIXEL_SHADER_BARRIER, SYSMEMBAR_ENABLE_FALSE);
    __push_immd(p, SUBC_NVB197, SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED, 0);
}

void switch_gm20b_pass_barrier(struct nv_push *p, bool tiled_cache)
{
    if (tiled_cache)
        switch_gm20b_tiled_cache_flush(p);
    switch_gm20b_wait_for_idle(p);
    switch_gm20b_invalidate_shader_caches(p, SWITCH_GM20B_SHADER_FLUSH_DATA | SWITCH_GM20B_SHADER_DATA |
                                                 SWITCH_GM20B_SHADER_CONSTANTS);
    switch_gm20b_invalidate_texture_data(p);
}
