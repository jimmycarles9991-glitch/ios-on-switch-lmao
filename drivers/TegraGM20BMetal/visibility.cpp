// SPDX-License-Identifier: MIT
#include "visibility.h"
#include <algorithm>
#include "../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "../../kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"
#include "kit.h"

namespace gm20b {

const char *visibilityRefusal(const VisibilityAsk &a)
{
    if (a.mode == kVisibilityDisabled)
        return nullptr;
    if (a.mode != kVisibilityCounting && a.mode != kVisibilityBoolean)
        return "a visibility result mode Metal does not have";
    if (!a.gm20bBuffer)
        return "a mode set in a pass with no visibility result buffer";
    if ((a.offset & ~uint64_t(7)) + 8 > a.bufferLength)
        return "a visibility offset past its buffer";
    return nullptr;
}

void VisibilityQueries::close(Push &p)
{
    if (!open_)
        return;
    const Query &q = queries_.back();
    if (!q.accumulate)
        kit(p, switch_gm20b_samples_report, q.slotVa);
    else if (q.mode == kVisibilityCounting)
        kit(p, switch_gm20b_samples_word, q.resultVa, true,
            uint32_t(NVB197_SET_REPORT_SEMAPHORE_D_REDUCTION_OP_RED_ADD));
    else
        kit(p, switch_gm20b_samples_word, q.slotVa, false, 0u);
    open_ = false;
}

VisibilityQueries::Set VisibilityQueries::set(Push &p, uint32_t mode, uint64_t resultVa, uint64_t slotVa,
                                              bool accumulate)
{
    const size_t key = size_t(resultVa >> 3) % kVisibilityQueriesPerEncoder;
    if (mode != kVisibilityDisabled) {
        if (taken_.test(key))
            return Set::Retaken;
        taken_.set(key);
    }
    close(p);
    if (mode == kVisibilityDisabled) {
        if (counting_)
            kit(p, switch_gm20b_samples_end);
        counting_ = false;
        return Set::Done;
    }
    queries_.push_back({resultVa, slotVa, mode, accumulate});
    if (accumulate && mode == kVisibilityBoolean)
        for (uint64_t zero : {slotVa + 4, slotVa + 16, slotVa + 20})
            kit(p, switch_gm20b_release_word, zero, 0u);
    kit(p, switch_gm20b_samples_begin);
    open_ = counting_ = true;
    return Set::Done;
}

void VisibilityQueries::finish(Push &p)
{
    close(p);
    if (counting_)
        kit(p, switch_gm20b_samples_end);
    counting_ = false;
    if (std::any_of(queries_.begin(), queries_.end(), [](const Query &q) {
            return !q.accumulate || q.mode == kVisibilityBoolean;
        }))
        kit(p, switch_gm20b_wait_for_idle);
}

// Boolean: each half of the count made at most 1 by its copy's own semaphore release (an unsigned IMIN, as predication
// reduces a word), so the result is 1 where any sample passed and 0 where none did, written by the GPU alone
void VisibilityQueries::finishResult(Push &p, size_t i) const
{
    const Query &q = queries_[i];
    if (q.accumulate) {
        if (q.mode != kVisibilityBoolean)
            return;
        kit(p, switch_gm20b_copy_predicate, q.slotVa, uint32_t(NVB0B5_SET_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL));
        kit(p, switch_gm20b_copy_word_reduce, q.resultVa, q.resultVa, 1u,
            uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IOR), false);
        kit(p, switch_gm20b_copy_predicate, 0ull, uint32_t(NVB0B5_SET_RENDER_ENABLE_C_MODE_TRUE));
        return;
    }
    if (q.mode != kVisibilityBoolean) {
        kit(p, switch_gm20b_copy_bytes, q.slotVa, q.resultVa, 8u,
            uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
        return;
    }
    for (uint32_t half : {0u, 4u})
        kit(p, switch_gm20b_copy_word_reduce, q.slotVa + half, q.resultVa + half, 1u,
            uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN), false);
}

}  // namespace gm20b
