// SPDX-License-Identifier: MIT
// Copyright © 2024 Collabora, Ltd.
// A dispatch's QMD (version 01_07, MAXWELL_COMPUTE_B), filled as NAK's nak_fill_qmd (qmd.rs, Qmd1_7) fills it: the
// program at `program`, its constant buffers at `cbuf_address`, no dependent QMD.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "SwitchGM20BCompute.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_QMD_CBUFS 8u

struct switch_gm20b_qmd_info {
    // What NAK's nak_shader_info says of the program
    uint8_t num_gprs, num_barriers;
    uint16_t local_size[3];
    uint32_t slm_size, crs_size;
    // The dispatch's: shared memory, grid, program offset and the constant buffers by slot (size 0: none)
    uint32_t smem_size, grid[3];
    uint32_t program;
    uint32_t cbuf_count, cbuf_size[SWITCH_GM20B_QMD_CBUFS];
    uint64_t cbuf_address[SWITCH_GM20B_QMD_CBUFS];
};

static inline uint32_t switch_gm20b_qmd_align(uint32_t v, uint32_t to)
{
    return (v + to - 1) / to * to;
}

// False where the shared memory is past GM20B's 48 KiB or the constant buffers past its eight
static inline bool switch_gm20b_qmd_fill(const struct switch_gm20b_qmd_info *in, uint32_t q[64])
{
    const uint32_t smem = switch_gm20b_qmd_align(in->smem_size, 0x100);
    if (smem > (48u << 10) || in->cbuf_count > SWITCH_GM20B_QMD_CBUFS)
        return false;
    for (int i = 0; i < 64; i++)
        q[i] = 0;
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, QMD_MAJOR_VERSION, 1);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, QMD_VERSION, 7);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, API_VISIBLE_CALL_LIMIT, NVB1C0_QMDV01_07_API_VISIBLE_CALL_LIMIT_NO_CHECK);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SAMPLER_INDEX, NVB1C0_QMDV01_07_SAMPLER_INDEX_INDEPENDENTLY);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SASS_VERSION, 0x30);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, BARRIER_COUNT, in->num_barriers);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_RASTER_WIDTH, in->grid[0]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_RASTER_HEIGHT, in->grid[1]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_RASTER_DEPTH, in->grid[2]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_THREAD_DIMENSION0, in->local_size[0]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_THREAD_DIMENSION1, in->local_size[1]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CTA_THREAD_DIMENSION2, in->local_size[2]);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, PROGRAM_OFFSET, in->program);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, REGISTER_COUNT, in->num_gprs);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SHADER_LOCAL_MEMORY_CRS_SIZE, switch_gm20b_qmd_align(in->crs_size, 0x200));
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SHADER_LOCAL_MEMORY_HIGH_SIZE, 0);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SHADER_LOCAL_MEMORY_LOW_SIZE, switch_gm20b_qmd_align(in->slm_size, 0x10));
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, SHARED_MEMORY_SIZE, smem);
    NVVAL_MW_SET(q, NVB1C0, QMDV01_07, L1_CONFIGURATION,
                 smem <= (16u << 10)   ? NVB1C0_QMDV01_07_L1_CONFIGURATION_DIRECTLY_ADDRESSABLE_MEMORY_SIZE_16KB
                 : smem <= (32u << 10) ? NVB1C0_QMDV01_07_L1_CONFIGURATION_DIRECTLY_ADDRESSABLE_MEMORY_SIZE_32KB
                                       : NVB1C0_QMDV01_07_L1_CONFIGURATION_DIRECTLY_ADDRESSABLE_MEMORY_SIZE_48KB);
    for (uint32_t c = 0; c < in->cbuf_count; c++) {
        if (!in->cbuf_size[c])
            continue;
        NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CONSTANT_BUFFER_ADDR_LOWER, c, (uint32_t)in->cbuf_address[c]);
        NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CONSTANT_BUFFER_ADDR_UPPER, c, (uint32_t)(in->cbuf_address[c] >> 32));
        NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CONSTANT_BUFFER_SIZE, c, in->cbuf_size[c]);
        NVVAL_MW_SET(q, NVB1C0, QMDV01_07, CONSTANT_BUFFER_VALID, c, NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID_TRUE);
    }
    return true;
}

#ifdef __cplusplus
}
#endif
