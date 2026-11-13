// SPDX-License-Identifier: MIT
// The kit compiled by the kernel toolchain: never linked, built so the kernel's flags and headers keep accepting it.
#include "SwitchGM20BMacros.h"
#include "nv_push.h"
#include "nv_push_clb0b5.h"
#include "nv_push_clb197.h"

uint32_t switch_gm20b_emit_kernel_check(uint32_t *words, size_t count);

uint32_t switch_gm20b_emit_kernel_check(uint32_t *words, size_t count)
{
    struct nv_push push;
    nv_push_init(&push, words, count, SUBC_MASK_ALL);
    P_IMMD(&push, NVB197, WAIT_FOR_IDLE, 0);
    P_MTHD(&push, NVB0B5, OFFSET_IN_UPPER);
    P_NVB0B5_OFFSET_IN_UPPER(&push, 0);
    return (uint32_t)nv_push_dw_count(&push) + switch_gm20b_macro_draw[0];
}
