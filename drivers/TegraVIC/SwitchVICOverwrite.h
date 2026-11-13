// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICRequest.h"

static inline bool switch_vic_overwrites(const TegraVICPlan &plan, const TegraVICSurfaceDesc &destination)
{
    for (uint32_t i = 0; i < plan.count; ++i) {
        const auto &op = plan.op[i];
        if (!op.dstX && !op.dstY && op.dstW == destination.width && op.dstH == destination.height)
            return true;
    }
    return false;
}
