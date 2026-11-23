// SPDX-License-Identifier: MIT
// The pixel stage's and the rasteriser's switches (owned.h). The channel's context turns both on. A state of no
// fragment program (pack.h kNoProgram) turns the pixel stage off, as NVK binds no pixel shader for a pipeline of no
// fragment stage (nvk_cmd_flush_gfx_shaders: SET_PIPELINE_SHADER(PIXEL) enable false): its draws write depth and
// stencil through the ROP and no colour. One whose key says " rast=0" (rasterizationEnabled NO) turns the rasteriser
// off, as NVK's rasterizer discard does (SET_RASTER_ENABLE false). The next state's bind turns them back on
// (Pipeline::restore). The context init already masks the out-of-range output exception a vertex program raises with no
// pixel stage to take its outputs (gr_gpcs_tpcs_sms_hww_warp_esr_report_mask bit 14).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "pack.h"
#include "push.h"

namespace gm20b {

constexpr uint32_t kPixelStageSwitch = 0x2140;                   // SET_PIPELINE_SHADER(PIXEL)
constexpr uint32_t kRasterSwitch = 0x037C;                       // SET_RASTER_ENABLE
constexpr uint32_t kPixelStageOn = 0x51, kPixelStageOff = 0x50;  // ENABLE (bit 0), TYPE PIXEL (bits 7:4)
constexpr uint32_t kStageSwitchWords = 4;

// The key head pipelineKey writes for a state Metal does not rasterize (TegraGM20BMetalPipelineKey.h)
inline bool rasterOff(const char *keyString)
{
    return keyString && strstr(keyString, " rast=0") != nullptr;
}

// The channel's context's switches: both on
inline void stageSwitchesOn(Push &p)
{
    p.m(kPixelStageSwitch, kPixelStageOn);
    p.m(kRasterSwitch, 1);
}

// The switches a state of `pp` and key head `keyString` turns off, into its methods `w`, and back on, into `restore`
inline void stageSwitches(const PackPipeline &pp, const char *keyString, Push &w, std::vector<uint32_t> &restore)
{
    const bool pixel = pixelStage(pp), raster = !rasterOff(keyString);
    if (pixel && raster)
        return;
    uint32_t back[kStageSwitchWords];
    Push r{back, back + kStageSwitchWords};
    if (!pixel) {
        w.m(kPixelStageSwitch, kPixelStageOff);
        r.m(kPixelStageSwitch, kPixelStageOn);
    }
    if (!raster) {
        w.m(kRasterSwitch, 0);
        r.m(kRasterSwitch, 1);
    }
    restore.insert(restore.end(), back, r.at);
}

}  // namespace gm20b
