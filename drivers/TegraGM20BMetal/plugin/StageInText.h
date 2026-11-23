// SPDX-License-Identifier: MIT
// A compute state's stage input as the driver keys its kernels (TegraGM20BMetalComputeState.mm: its constants text) and the
// journal's D line names it (pack/kernel_shapes.h reads it back): " | in a<i>=<format>:<offset>:<buffer>:<stride>:<step
// function>:<step rate>... i=<index type>:<index buffer>", a dynamic stride as UINT32_MAX as TegraGM20BMetalPipelineKey.h writes a
// vertex layout's; "" for none, so a kernel without one keys as before.
#pragma once
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <string>

inline std::string stageInText(MTLStageInputOutputDescriptor *in)
{
    std::string text;
    char b[96];
    for (NSUInteger i = 0; in && i < 31; i++) {
        MTLAttributeDescriptor *a = in.attributes[i];
        if (a.format == MTLAttributeFormatInvalid)
            continue;
        MTLBufferLayoutDescriptor *l = in.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
        const unsigned long stride = l.stride == MTLBufferLayoutStrideDynamic ? UINT32_MAX : l.stride;
        snprintf(b, sizeof b, " a%lu=%lu:%lu:%lu:%lu:%lu:%lu", (unsigned long)i, (unsigned long)a.format,
                 (unsigned long)a.offset, (unsigned long)a.bufferIndex, stride, (unsigned long)l.stepFunction,
                 (unsigned long)l.stepRate);
        text += b;
    }
    if (text.empty())
        return text;
    snprintf(b, sizeof b, " i=%lu:%lu", (unsigned long)in.indexType, (unsigned long)in.indexBufferIndex);
    return " | in" + text + b;
}
