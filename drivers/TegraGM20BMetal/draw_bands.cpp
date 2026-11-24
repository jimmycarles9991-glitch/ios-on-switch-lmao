// SPDX-License-Identifier: MIT
// RenderEncoder's draws in scissor bands (draw_bands.h), priced by their state's learned rate (band_rates.h).
#include <os/log.h>
#include "gpu.h"
#include "mme.h"
#include "stamps.h"
#include "../../kits/SwitchGM20B/SwitchGM20B.h"

namespace gm20b {

uint64_t RenderEncoder::drawUnits(uint32_t instances) const
{
    return laneWork(laneWork(drawPixels_, instances), emitted_->samplePasses);
}

uint32_t RenderEncoder::drawBands(uint32_t vertices, uint32_t instances, uint64_t units) const
{
    const Pipeline &pl = *emitted_;
    if (dev_->compositorClass() || pl.vertexStores || cb_->bands >= kBandsPerCommandBuffer)
        return 1;
    BandDraw d;
    d.fragmentNs = estimatedNs(pl.bandRate, units, workNs(laneWork(units, pl.fragmentInstructions)));
    d.vertexNs = workNs(laneWork(laneWork(vertices, instances), pl.vertexInstructions));
    d.rows = drawBox_.empty() ? 0 : drawBox_.y1 - drawBox_.y0;
    return bandCount(d, dev_->bandBudgetNs(), kBandsPerCommandBuffer - cb_->bands);
}

// The pass's first draw, the one a pass of one draw measures at its end (measurePass): the render server is never banded,
// so its draws teach nothing
void RenderEncoder::noteCandidate(uint64_t units, uint32_t bands)
{
    if (passDraws_ != 1 || dev_->compositorClass())
        return;
    candidate_ = {&emitted_->bandRate,
                  units,
                  0,
                  0,
                  emitted_->rec ? emitted_->rec->keyHash : 0,
                  emitted_->fragmentInstructions,
                  bands};
}

#if SWITCH_METAL_DIAGNOSTICS
// A draw large enough to measure, by where its price came from, and a line for one priced at a long draw's length
void RenderEncoder::notePrice(uint64_t units, uint32_t bands) const
{
    const Pipeline &pl = *emitted_;
    if (units < kMeasureUnits || dev_->compositorClass() || pl.vertexStores)
        return;
    const bool learned = pl.bandRate.psPerUnit.load(std::memory_order_relaxed) != 0;
    (learned ? dev_->stats.pricedLearned : dev_->stats.pricedStatic)++;
    const uint64_t ns = estimatedNs(pl.bandRate, units, workNs(laneWork(units, pl.fragmentInstructions)));
    if (ns >= kLongDrawNs)
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
               "SwitchMetal: banded draw k%016llx %llu kunits priced %llu us (%s) in %u bands",
               (unsigned long long)(pl.rec ? pl.rec->keyHash : 0), (unsigned long long)(units / 1000),
               (unsigned long long)(ns / 1000), learned ? "learned" : "static", bands);
}
#endif

// At the pass's end, `drained` where its draws' writes are out of the tiled cache: whether its one draw is measured, its
// stamps' slot taken. The start stamp goes at the end of the pass's prologue and the end stamp after the drain, so the
// measured draw keeps every word and every flush it has unmeasured.
bool RenderEncoder::measurePass(bool drained)
{
    if (!drained || passDraws_ != 1 || !candidate_.rate || cb_->timings.size() >= kMeasuredPerCommandBuffer ||
        !measure(*candidate_.rate, candidate_.units))
        return false;
    const uint32_t n = dev_->drawStamps_++;
    candidate_.slot = n % kDrawStampSlots;
    candidate_.payload = n + 1 ? n + 1 : 1;
    cb_->timings.push_back(candidate_);
    return true;
}

void RenderEncoder::emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                             uint32_t baseInstance)
{
    const auto words = [&](Push &p) {
        mme::draw(p, params_, indexed, first, count, instances, root, baseInstance);
    };
    const uint64_t units = drawUnits(instances);
    const uint32_t bands = drawBands(count, instances, units);
    noteCandidate(units, bands);
    SWITCH_METAL_DIAGNOSTIC(notePrice(units, bands));
    if (bands < 2) {
        if (p_.room() < kBandWords)
            reserve();
        words(p_);
        finishDraw(count, instances);
        return;
    }
    const PixelBox area = {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (dev_->probeMacroBands.load(std::memory_order_relaxed) && !indexed && instances == 1 && rot_.turn == 0 &&
        area.y1 > area.y0) {
        if (p_.room() < mme::kBandDrawMaxWords)
            reserve();
        mme::bandDraw(p_, params_, bands, first, count, root, baseInstance, uint16_t(area.x0), uint16_t(area.x1),
                      uint16_t(area.y0), uint16_t(area.y1));
        cb_->bands += bands;
        SWITCH_METAL_DIAGNOSTIC(if (cb_->drawTimes_) timeDraw(count, instances, false));
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.bandedDraws++; dev_->stats.bands += bands);
        return;
    }
#endif
    if (cb_->batchWork_.operations)
        cutBatch();
    emitBands(
        p_, rot_, area, drawBox_, bands,
        [&] {
            if (p_.room() < kBandWords)
                reserve();
        },
        words,
        [&] {
            cutBatch();
        });
    cb_->bands += bands;
    SWITCH_METAL_DIAGNOSTIC(if (cb_->drawTimes_) timeDraw(count, instances, false));
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.bandedDraws++; dev_->stats.bands += bands);
}

void Device::noteCommittedTimings(std::vector<DrawTiming> &timings)
{
    if (timings.empty())
        return;
    committedTimings_.insert(committedTimings_.end(), timings.begin(), timings.end());
    timings.clear();
}

// Each command buffer's begin calls this, on the thread its commit handed the measured draws over on: the one thread that
// encodes at a time, which alone takes stamp slots, so the list has one owner and no completion thread reads it. Each
// committed measured draw whose two stamps the GPU wrote teaches its state, one whose slot a later draw took is dropped,
// the rest wait for a later begin.
void Device::learnCompleted()
{
    if (committedTimings_.empty())
        return;
    const uint32_t taken = drawStamps_;
    size_t kept = 0;
    for (const DrawTiming &t : committedTimings_) {
        const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + drawStampOffset(t.slot, false));
        uint64_t ns = 0;
        if (stampedNanoseconds(*slot, t.payload, ns)) {
            learn(*t.rate, ns, t.units);
            SWITCH_METAL_DIAGNOSTIC(stats.learnedDraws++);
#if SWITCH_METAL_DIAGNOSTICS
            if (ns >= kLongDrawNs)
                os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                       "SwitchMetal: long draw %llu us k%016llx %llu kunits %u instr in %u bands, learned %u ps a unit",
                       (unsigned long long)(ns / 1000), (unsigned long long)t.key, (unsigned long long)(t.units / 1000),
                       t.instructions, t.bands, t.rate->psPerUnit.load(std::memory_order_relaxed));
#endif
            continue;
        }
        if (taken - (t.payload - 1) <= kDrawStampSlots)
            committedTimings_[kept++] = t;
    }
    committedTimings_.resize(kept);
}

}  // namespace gm20b
