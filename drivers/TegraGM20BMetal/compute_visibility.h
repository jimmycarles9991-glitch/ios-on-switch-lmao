// SPDX-License-Identifier: MIT
#pragma once
#include "push.h"

namespace gm20b {

inline void computeRelease(Push &push)
{
    constexpr uint32_t kComputeShaderCaches = 0x021c;
    constexpr uint32_t kFlushData = 1u << 2, kInvalidateData = 1u << 4;
    // Implicit WFI plus L1->L2 shader data flush on SUBC_COMPUTE without bouncing to SUBC_3D.
    push.m(kComputeShaderCaches, kFlushData | kInvalidateData, SUBC_COMPUTE);
}

inline void computeToGraphics(Push &push, bool sampledImages)
{
    constexpr uint32_t kGraphicsTextureCache = 0x1288, kGraphicsShaderCaches = 0x0da4;
    constexpr uint32_t kInvalidateData = 1u << 4, kInvalidateConstant = 1u << 12;
    computeRelease(push);
    if (sampledImages)
        push.m(kGraphicsTextureCache, 0, SUBC_3D);
    push.m(kGraphicsShaderCaches, kInvalidateData | kInvalidateConstant, SUBC_3D);
}

}  // namespace gm20b
