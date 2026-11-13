// SPDX-License-Identifier: MIT
#include "raw_visibility.h"
#include <algorithm>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"
#include "raw_kit.h"

namespace raw {

const char *visibilityRefusal(const VisibilityAsk &a)
{
    if (a.mode == kVisibilityDisabled)
        return nullptr;
    if (a.mode != kVisibilityCounting && a.mode != kVisibilityBoolean)
        return "a visibility result mode Metal does not have";
    if (!a.rawBuffer)
        return "a visibility result buffer the raw path does not hold";
    if ((a.offset & ~uint64_t(7)) + 8 > a.bufferLength)
        return "a visibility offset past its buffer";
    if (a.accumulate)
        return "visibility results accumulated across passes";
    return nullptr;
}

void VisibilityQueries::close(Push &p)
{
    if (!open_)
        return;
    kit(p, switch_gm20b_samples_report, queries_.back().slotVa);
    open_ = false;
}

VisibilityQueries::Set VisibilityQueries::set(Push &p, uint32_t mode, uint64_t resultVa, uint64_t slotVa)
{
    if (mode != kVisibilityDisabled) {
        if (std::any_of(queries_.begin(), queries_.end(), [&](const Query &q) {
                return q.resultVa == resultVa;
            }))
            return Set::Retaken;
        if (queries_.size() >= kVisibilityQueriesPerEncoder)
            return Set::Full;
    }
    close(p);
    if (mode == kVisibilityDisabled) {
        if (counting_)
            kit(p, switch_gm20b_samples_end);
        counting_ = false;
        return Set::Done;
    }
    queries_.push_back({resultVa, slotVa, mode});
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
    if (!queries_.empty())
        kit(p, switch_gm20b_wait_for_idle);
}

// Boolean: each half of the count made at most 1 by its copy's own semaphore release (an unsigned IMIN, as predication
// reduces a word), so the result is 1 where any sample passed and 0 where none did, written by the GPU alone
void VisibilityQueries::finishResult(Push &p, size_t i) const
{
    const Query &q = queries_[i];
    if (q.mode != kVisibilityBoolean) {
        kit(p, switch_gm20b_copy_bytes, q.slotVa, q.resultVa, 8u,
            uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
        return;
    }
    for (uint32_t half : {0u, 4u})
        kit(p, switch_gm20b_copy_word_reduce, q.slotVa + half, q.resultVa + half, 1u,
            uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN), false);
}

}  // namespace raw
