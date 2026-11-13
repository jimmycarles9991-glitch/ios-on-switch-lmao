// SPDX-License-Identifier: BSD-2-Clause-Patent
// Core Animation's SDR dimming factor for EDR. With EDR content on screen it renders SDR content at full scale, sends
// f = 1/headroom through IOMobileFramebufferSetParameter(fb, 6, f * 65536) between swap_begin and swap_end
// (QuartzCore swap_display_edr_factor 0x18436591c, when CADeviceDisplaySupportsEDRFactor: every chip id but 0x7000 and
// 0x8301), and leaves the 1/headroom scale to the display (Renderer::render 0x18433f6e4 skips its own EDR pass then)
// while the backlight rises by the headroom. The family takes parameter 6 only when its gate byte is set
// (IOMobileFramebufferLegacy init: boot-arg iomfb_enable_edr_scaling, 24B119 +0x1db8, 21H461 +0x1878) and hands the
// subclass f as a float, 0 for EDR off (24B119 slot 344, 21H461 slot 326). Apple's J181 pipe scales every plane by it.
#pragma once
#include <stdint.h>

// 16.16, unity for 0 (EDR off) and anything outside (0, 1].
inline uint32_t switch_edr_display_scale(float factor)
{
    constexpr uint32_t kUnity = 1u << 16;
    if (!(factor > 0.0f) || factor >= 1.0f)
        return kUnity;
    const uint32_t scale = static_cast<uint32_t>(factor * 65536.0f + 0.5f);
    return scale ? scale : 1;
}
