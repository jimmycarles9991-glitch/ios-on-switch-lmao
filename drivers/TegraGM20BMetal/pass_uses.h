// SPDX-License-Identifier: MIT
// A pass's reads and writes for rule barrier.pass_pixel (pass_hazard.h): what TegraGM20BMetalEncode found in its recording
// (its samples and named uses), with the targets, depth and area the encoder holds; a memoryless target's backing as its
// scratch. A pass the encode never described, one rendered through shadow images, a memoryless target with no backing of
// the pass's or a turned plane, or one whose storage forms read and write a target through the SMs' data cache (which
// only the drain drops; no held cell drew one), is unknown and keeps the drain.
#pragma once

namespace gm20b {

PassUses RenderEncoder::passUses() const
{
    PassUses uses = planned_;
    uses.vertexSamples = vertexSamples_;
    uses.areaWidth = areaW_;
    uses.areaHeight = areaH_;
    uses.depthStencil = pass_.depthStencil.texture != nullptr;
    if (rot_.turned() || pass_.storageMask)
        uses.known = false;
    for (uint32_t j = 0; j < pass_.count && j < 8 && uses.known; j++) {
        const Texture *t = pass_.color[j].texture;
        if (!t)
            continue;
        if (shadowOf_[j] || (t->memoryless && !memoryless_[j]))
            uses.known = false;
        else if (memoryless_[j])
            uses.scratchWrite(hazardImage(*t));
        else
            uses.write(hazardImage(*t));
    }
    return uses;
}

PassForm RenderEncoder::passRule(BarrierEngine before, const PassUses &uses) const
{
    return before == BarrierEngine::ThreeD && pixelBarrierHolds(cb_->chain_, uses) ? PassForm::PixelOrdered
                                                                                   : PassForm::Today;
}

}  // namespace gm20b
