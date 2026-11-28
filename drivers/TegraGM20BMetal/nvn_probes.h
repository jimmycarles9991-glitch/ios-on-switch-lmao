// SPDX-License-Identifier: MIT
// HW Test gpu.perf.nvn's A/B probes (Device::probeNvn, debug builds): state NVN writes and the driver does not, each
// tried on the device before any becomes a rule (c38-audit/perf1/learn/deko3d.md Z1-Z3 and C5). Method words from
// deko3d's maxwell/engine_3d.def (kits/SwitchGM20B/SwitchGM20BTegra.h) and clb197.h. The host tests' method check
// (switch_gm20b_method_legal) covers these methods' addresses, never their values: a value the chip refuses shows only on
// the device.
#pragma once
#include <cstdint>
#include "../../kits/SwitchGM20B/SwitchGM20BTegra.h"

namespace gm20b {

enum : uint32_t {
    // a depth-writing program opens zcull's bounds (SET_ZCULL_BOUNDS Z_MIN and Z_MAX unbounded) for its draws, in place
    // of invalidating the pass's region, and closes them for the next program that does not write depth
    kNvnZcullBounds = 1u << 0,
    kNvnStencilZcull = 1u << 1,  // SET_ZCULL 0x11 (stencil zcull beside depth) over each zcull pass, 0x1 after it
    kNvnZcull4x2 = 1u << 2,      // SET_ZCULL_REGION_FORMAT Z_4X2 in place of Z_4X4
    kNvnZcullFp = 1u << 3,       // ZFORMAT FP for a float depth cleared to 0.0 or 1.0, in place of ZF32_1
    // DISCARD_RENDER_TARGET at the pass end for each attachment nothing reads after the pass (ColorAttachment::discard,
    // a depth/stencil neither stored nor loaded later)
    kNvnDiscard = 1u << 4,
};

constexpr uint32_t kZcullBoundsUnbounded = 0x11;
constexpr uint32_t kZcullDepthAndStencil = 0x11, kZcullDepth = 0x1;
constexpr uint32_t kZcullRegionZ4x2 = 2, kZcullFormatFp = 1;
constexpr uint32_t kDiscardRenderTarget = SWITCH_NVB197_DISCARD_RENDER_TARGET;
constexpr uint32_t kDiscardDepthStencil = 1;

constexpr uint32_t discardColour(uint32_t target)
{
    return (target & 7) << 4;
}

}  // namespace gm20b
