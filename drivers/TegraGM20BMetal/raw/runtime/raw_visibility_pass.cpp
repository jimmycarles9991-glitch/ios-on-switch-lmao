// SPDX-License-Identifier: MIT
// A render encoder's visibility results (raw_visibility.h) in its pass body, and the device's page of report slots.
#include "raw_gpu.h"

namespace raw {

uint64_t Device::visibilitySlot()
{
    std::call_once(visibilityOnce_, [this] {
        if (!createControlBuffer(uint64_t(kVisibilitySlots) * kVisibilitySlotBytes, visibilityPage_))
            visibilityPage_ = {};
    });
    if (!visibilityPage_.va)
        return 0;
    // The channel runs a process's work in order, so a slot is free again long before 4,096 later queries reach it
    const uint32_t slot = visibilityNext_.fetch_add(1, std::memory_order_relaxed) % kVisibilitySlots;
    return visibilityPage_.va + uint64_t(slot) * kVisibilitySlotBytes;
}

bool RenderEncoder::setVisibility(uint32_t mode, const Buffer *results, uint64_t offset)
{
    if (mode == kVisibilityDisabled && !visibility_.any())
        return true;  // Core Animation sets Disabled at every encoder state change: it emits nothing
    uint64_t resultVa = 0, slotVa = 0;
    if (mode != kVisibilityDisabled) {
        if (!results || !results->va || !(slotVa = dev_->visibilitySlot()))
            return false;
        resultVa = results->va + (offset & ~uint64_t(7));
        cb_->use(*results, true);
        cb_->use(dev_->visibilityPage_, true);
    }
    if (p_.room() < VisibilityQueries::kSetWords)
        reserve();
    return visibility_.set(p_, mode, resultVa, slotVa) == VisibilityQueries::Set::Done;
}

void RenderEncoder::finishVisibility()
{
    if (!visibility_.any())
        return;
    if (p_.room() < VisibilityQueries::kFinishWords)
        reserve();
    visibility_.finish(p_);
    for (size_t i = 0; i < visibility_.results(); i++) {
        if (p_.room() < VisibilityQueries::kResultWords)
            reserve();
        visibility_.finishResult(p_, i);
    }
    visibility_.clear();
}

}  // namespace raw
