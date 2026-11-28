// SPDX-License-Identifier: MIT
// gpu.perf.knobs' and gpu.perf.nvn's probes at a pass's end and prologue (nvn_probes.h), debug builds only
#include "nvn_probes.h"
#include "gpu.h"
#include "gm20b_gpu_internal.h"

#if SWITCH_METAL_DIAGNOSTICS
namespace gm20b {

// gpu.perf.nvn (nvn_probes.h), at the body's end, before the prologue is written: the discards of what nothing reads
// after the pass, while its targets are bound and before the tiled cache's flush, and the zcull state a probe changed
// put back for the channel's next pass (the prologue writes SET_ZCULL's stencil-on form where stencilOn is set here)
void RenderEncoder::nvnPassEnd()
{
    const uint32_t nvn = dev_->probeNvn.load(std::memory_order_relaxed);
    zcull_.stencilOn = zcull_.candidate && (nvn & kNvnStencilZcull);
    if (!nvn && !zcull_.boundsOpen)
        return;
    Push &p = p_;
    if (p.room() < 2 * 11)
        reserve();
    Device::Stats &stats = dev_->stats;
    stats.nvnStencilPasses += zcull_.stencilOn;
    if (nvn & kNvnDiscard) {
        for (uint32_t j = 0; j < pass_.count && j < 8; j++)
            if (pass_.color[j].texture && pass_.color[j].discard) {
                p.m(kDiscardRenderTarget, discardColour(j));
                stats.nvnDiscards++;
            }
        const DepthStencilAttachment &zs = pass_.depthStencil;
        if (zs.texture && !zs.stored && !zcull_.store) {
            p.m(kDiscardRenderTarget, kDiscardDepthStencil);
            stats.nvnDiscards++;
        }
    }
    if (zcull_.boundsOpen) {
        p.m(NVB197_SET_ZCULL_BOUNDS, 0);
        zcull_.boundsOpen = false;
    }
    if (zcull_.stencilOn)
        p.m(NVB197_SET_ZCULL, kZcullDepth);
}

// The channel keeps the knob after the probe is cleared: HW Test writes the golden value back through it last
void RenderEncoder::nvnKnob(Push &p)
{
    if (const uint32_t knob = dev_->probeSubtilingKnob.load(std::memory_order_relaxed)) {
        p.m(NVB197_SET_SUBTILING_PERF_KNOB_A, knob);
        dev_->stats.subtileWrites++;
    }
}

// The zcull region's format and Z format the probe asks of a pass with depth target `zs`, and SET_ZCULL's stencil-on form
// where nvnPassEnd set stencilOn
void RenderEncoder::nvnZcullFormats(Push &p, const DepthStencilAttachment &zs, uint32_t &regionFormat,
                                    uint32_t &zFormat)
{
    const uint32_t nvn = dev_->probeNvn.load(std::memory_order_relaxed);
    if (nvn & kNvnZcull4x2)
        regionFormat = kZcullRegionZ4x2;
    if ((nvn & kNvnZcullFp) && zs.clearDepth && (zs.depth == 0.0f || zs.depth == 1.0f) &&
        zs.texture->format != 250)  // Depth16Unorm is FP already
        zFormat = kZcullFormatFp;
    dev_->stats.nvnFormatPasses += regionFormat != 0 || zFormat != ~0u;
    if (zcull_.stencilOn)
        p.m(NVB197_SET_ZCULL, kZcullDepthAndStencil);
}

}  // namespace gm20b
#endif
