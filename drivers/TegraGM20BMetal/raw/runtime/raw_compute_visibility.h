// SPDX-License-Identifier: MIT
#pragma once
#include "raw_push.h"

namespace raw {

inline void computeToGraphics(Push &push, bool sampledImages)
{
    constexpr uint32_t kComputeShaderCaches = 0x021c, kGraphicsTextureCache = 0x1288, kGraphicsShaderCaches = 0x0da4;
    constexpr uint32_t kFlushData = 1u << 2, kInvalidateData = 1u << 4, kInvalidateConstant = 1u << 12;
    // Shader stores require the producer flush, which also waits for idle; consumer invalidation alone is insufficient.
    push.m(kComputeShaderCaches, kFlushData | kInvalidateData, SUBC_COMPUTE);
    if (sampledImages)
        push.m(kGraphicsTextureCache, 0, SUBC_3D);
    push.m(kGraphicsShaderCaches, kInvalidateData | kInvalidateConstant, SUBC_3D);
}

}  // namespace raw
