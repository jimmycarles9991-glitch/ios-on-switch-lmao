// SPDX-License-Identifier: BSD-2-Clause-Patent
// Display routes a window probe (DisplayWindowProbe.h) has to prove on the device before the display takes them. Each is
// false until its probe's HW Test reports claim_verdict=PASS; the same on every iOS version (the controller is T210's).
#pragma once

struct SwitchDisplayTraits {
    // V5a, display.x1.overlay_block_yuv: windows B and C scan NV12 in a decoder's 2-GOB and 16-GOB blocks as they
    // scan it pitch linear.
    bool overlayBlockYuv;
    // V3, display.x1.column_scan_yuv: window A column-scans NVDEC's landscape NV12, and window B scans the UI's 16-GOB
    // BGRA as window A does.
    bool columnScanYuv;
    // V5c, display.x1.offset_crop: a block-linear NV12 source rectangle through ADDR_H_OFFSET and ADDR_V_OFFSET, START
    // on the plane bases.
    bool offsetCrop;
};

constexpr SwitchDisplayTraits kSwitchDisplayTraits = {false, false, false};

// The traits the kernel runs with: kSwitchDisplayTraits, and on debug kernels those a HW Test run forces for itself
// through TegraVIC's kernel-test selector until its connection closes (SwitchVICTurn.cpp).
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
extern "C" SwitchDisplayTraits switch_display_traits(void);
#else
constexpr SwitchDisplayTraits switch_display_traits()
{
    return kSwitchDisplayTraits;
}
#endif
