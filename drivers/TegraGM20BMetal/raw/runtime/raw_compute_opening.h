// SPDX-License-Identifier: MIT
// A compute encoder's opening (ComputeEncoder in every build; the debug build's probeComputeOpening wraps it): what the
// render and copy work before wrote, out of the SMs' data, constant and texture caches; then the scheduler's cached QMDs
// (the upload ring reuses their addresses) and the compute class's header caches, as NVK opens a command buffer's
// compute work (nvk_cmd_buffer_begin_compute). INVALIDATE_SHADER_CACHES is the waiting one of its pair (clb1c0.h 0x021c,
// beside INVALIDATE_SHADER_CACHES_NO_WFI 0x1698; NVK nvk_cmd_buffer.c's flush of shader data: "This is also implicitly a
// WFI"), so no WAIT_FOR_IDLE goes before it.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"

namespace raw {

// the header caches alone: the opening's tail, which the debug build's other forms of the flush keep
inline void computeOpeningHeaders(Push &p)
{
    p.m(NVB1C0_INVALIDATE_SKED_CACHES, 0, SUBC_COMPUTE);
    p.m(NVB1C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    p.m(NVB1C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
}

inline void computeOpening(Push &p)
{
    constexpr uint32_t data = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE),
                       flush = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
                       constant = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
    p.m(NVB1C0_INVALIDATE_SHADER_CACHES, data | flush | constant, SUBC_COMPUTE);
    p.m(NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    computeOpeningHeaders(p);
}

}  // namespace raw
