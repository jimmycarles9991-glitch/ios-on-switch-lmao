// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../SwitchPower/SwitchDisplayMemory.h"

namespace switch_display {
enum class TimingBank {
    Active,
    Programmed
};

template <class Registers>
bool memoryTiming(Registers &dc, uint32_t pixelClock, switch_display_memory_head &out,
                  TimingBank bank = TimingBank::Active)
{
    const uint32_t access = dc[0x40];
    dc[0x40] = bank == TimingBank::Active ? access | 1u : access & ~1u;
    const uint32_t active = dc[0x409], sync = dc[0x407], back = dc[0x408], front = dc[0x40a];
    dc[0x40] = access;
    switch_display_memory_head result = {};
    result.pixel_clock_hz = pixelClock;
    result.active_width = active & 0xffff;
    result.active_height = active >> 16;
    result.total_width = result.active_width + (sync & 0xffff) + (back & 0xffff) + (front & 0xffff);
    result.total_height = result.active_height + (sync >> 16) + (back >> 16) + (front >> 16);
    if (!pixelClock || !result.active_width || !result.active_height || result.total_width > 16384 ||
        result.total_height > 16384 || result.total_width <= result.active_width ||
        result.total_height <= result.active_height)
        return false;
    out = result;
    return true;
}
}
