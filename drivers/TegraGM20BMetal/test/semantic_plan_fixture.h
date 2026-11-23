// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include "../TegraGM20BMetalPlanApply.h"

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

// a command's extra words, alive to the end of the record call
template <size_t N> const uint64_t *words(const uint64_t (&extra)[N])
{
    return extra;
}

Cmd binding(Op op, uint8_t stage, uint16_t slot, uintptr_t value, uint64_t offset = 0)
{
    Cmd c = cmd(op, stage, slot);
    c.obj = object(value);
    c.c = offset;
    return c;
}

void draw(SwitchMetalCommandPlan &plan, uint32_t first = 0)
{
    Cmd c = cmd(OpDraw, 3);
    c.a = first;
    c.b = 6;
    c.c = uint64_t(1) << 32;
    plan.record(c, {});
}

}  // namespace
