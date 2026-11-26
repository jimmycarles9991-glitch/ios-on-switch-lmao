// SPDX-License-Identifier: BSD-2-Clause-Patent
// One frame for display controller B: each of windows A, B and C as its registers hold it, and the memory the windows
// fetch together. SwitchExternalPlanes.h makes it; TegraDisplayB writes it.
#pragma once
#include <stdint.h>

#include "../SwitchPower/SwitchDisplayMemory.h"

namespace switch_external {

constexpr unsigned kWindows = 3;

enum Window : unsigned {
    kA,
    kB,
    kC
};

struct WindowRegisters {
    bool on = false;
    // cscWords go into the window's CSC with this frame. The CSC is not shadowed, so the window is off until it latches.
    bool csc = false;
    int matrix = -1, fullRange = -1;  // the CSC the window scans YUV with; -1 for RGB
    uint32_t options = 0, colorDepth = 0, byteSwap = 0, position = 0, size = 0, prescaled = 0;
    uint32_t hInitial = 0, vInitial = 0, dda = 0, lineStride = 0, blend = 0, blendSelect = 0;
    uint32_t start = 0, startU = 0, hOffset = 0, vOffset = 0, kind = 0;
    uint32_t cscWords[8] = {};
};

struct Frame {
    WindowRegisters window[kWindows];
    switch_display_memory_window memory[kWindows] = {};
    uint64_t peakBytes = 0;
};

// A window as the next frame finds it: whether it may be scanning (latched on, or turned on by a request still out or one
// whose latch was missed) and the CSC its registers hold (-1: not known).
struct WindowState {
    bool live = false;
    int matrix = -1, fullRange = -1;
};

inline switch_display_memory_head memoryOf(const switch_display_memory_head &timing, const Frame &frame)
{
    switch_display_memory_head head = timing;
    head.peak_bytes_per_second = frame.peakBytes;
    for (unsigned w = 0; w < kWindows; ++w)
        head.windows[w] = frame.memory[w];
    return head;
}

}  // namespace switch_external
