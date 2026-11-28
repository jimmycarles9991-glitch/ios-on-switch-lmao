// SPDX-License-Identifier: MIT
// RenderEncoder's draws in scissor bands (draw_bands.h), priced by their state's learned rate (band_rates.h).
#include "gpu.h"
#include "mme.h"
#include "stamps.h"
#include "../../kits/SwitchGM20B/SwitchGM20B.h"

namespace gm20b {

uint64_t RenderEncoder::drawUnits(uint32_t instances) const
{
    return laneWork(laneWork(drawPixels_, instances), emitted_->samplePasses);
}

BandGrid RenderEncoder::drawBands(uint32_t vertices, uint32_t instances, uint64_t units) const
{
    if (dev_->compositorClass())
        return {};
    const Pipeline &pl = *emitted_;
    const uint64_t budget = dev_->bandBudgetNs();
    if (!budget || pl.vertexStores)
        return {};
    const bool hasProgress = dev_->channelProgressVa() != 0;
    if (!hasProgress && cb_->bands >= kBandsPerCommandBuffer)
        return {};
    const bool unmeasured = units >= kMeasureUnits && !measuredOnce(pl.bandRate);
    const uint32_t staticInstructions = std::min(pl.fragmentInstructions, kMaxStaticInstructions);
    BandDraw d;
    d.fragmentNs = estimatedNs(pl.bandRate, units, workNs(laneWork(units, staticInstructions)));
    // what each band repeats: its vertices at their static price, or what its banded draws were measured to repeat
    d.vertexNs = std::max<uint64_t>(workNs(laneWork(laneWork(vertices, instances), pl.vertexInstructions)),
                                    pl.bandRate.nsPerBand.load(std::memory_order_relaxed));
    d.rows = drawBox_.empty() ? 0 : drawBox_.y1 - drawBox_.y0;
    d.cols = drawBox_.empty() ? 0 : drawBox_.x1 - drawBox_.x0;
    d.unmeasured = unmeasured;
    const uint32_t maxBands = hasProgress ? kMaxTileBands : kMaxBands;
    const uint32_t left = hasProgress ? kMaxTileBands : kBandsPerCommandBuffer - cb_->bands;
    const BandGrid grid = bandGrid(d, budget, left, maxBands);
    const uint32_t total = grid.total();
    const uint32_t probed = probeBands(pl.bandRate, units, total, passDraws_ == 1);
    if (probed == total)
        return grid;
    return bandGridFor(d, probed, left, unmeasured ? std::min(maxBands, kMaxUnmeasuredBands) : maxBands);
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
void RenderEncoder::notePrice(uint64_t units, BandGrid, uint32_t) const
{
    const Pipeline &pl = *emitted_;
    if (units < kMeasureUnits || dev_->compositorClass() || pl.vertexStores)
        return;
    const bool learned = pl.bandRate.psPerUnit.load(std::memory_order_relaxed) != 0;
    (learned ? dev_->stats.pricedLearned : dev_->stats.pricedStatic)++;
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
    noteProbeBands(*candidate_.rate, candidate_.bands);
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
    const BandGrid grid = drawBands(count, instances, units);
    const uint32_t bands = grid.total();
    SWITCH_METAL_DIAGNOSTIC(notePrice(units, grid, instances));
    if (bands < 2) {
        noteCandidate(units, bands);
        if (p_.room() < kBandWords)
            reserve();
        words(p_);
        finishDraw(count, instances);
        return;
    }
    const PixelBox area = {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (dev_->probeMacroBands.load(std::memory_order_relaxed) && !indexed && instances == 1 && rot_.turn == 0 &&
        grid.cols == 1 && area.y1 > area.y0) {
        noteCandidate(units, bands);
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
    const uint64_t progressVa = dev_->channelProgressVa();
    if (cb_->batchWork_.operations) {
        if (progressVa) {
            if (p_.room() < kProgressReleaseWords)
                reserve();
            emitProgressRelease(p_, progressVa, dev_->nextProgressSeq());
            cb_->batchWork_ = {};
        } else {
            cutBatch();
        }
    }
    bool selfMeasured = false;
    DrawTiming selfTiming;
    if (!dev_->compositorClass() && cb_->timings.size() < kMeasuredPerCommandBuffer &&
        measure(emitted_->bandRate, units)) {
        noteProbeBands(emitted_->bandRate, bands);
        selfMeasured = true;
        const uint32_t n = dev_->drawStamps_++;
        selfTiming = {&emitted_->bandRate,
                      units,
                      n % kDrawStampSlots,
                      n + 1 ? n + 1 : 1,
                      emitted_->rec ? emitted_->rec->keyHash : 0,
                      emitted_->fragmentInstructions,
                      bands};
        cb_->timings.push_back(selfTiming);
        if (p_.room() < kDrawStampWords)
            reserve();
        emitDrawStamp(p_, dev_->stamps_.va, selfTiming.slot, false, selfTiming.payload);
    }
    uint32_t bandIndex = 0;
    const uint32_t perBandRoom = kBandWords + kDrawStampWords + kProgressReleaseWords;
    emitBandGrid(
        p_, rot_, area, drawBox_, grid,
        [&] {
            if (p_.room() < perBandRoom)
                reserve();
        },
        words,
        [&] {
            if (progressVa)
                emitProgressRelease(p_, progressVa, dev_->nextProgressSeq());
            else
                cutBatch();
            if (selfMeasured && ++bandIndex == bands) {
                if (p_.room() < kDrawStampWords)
                    reserve();
                emitDrawStamp(p_, dev_->stamps_.va, selfTiming.slot, true, selfTiming.payload);
            }
        });
    if (progressVa)
        cb_->batchWork_ = {};
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
            learn(*t.rate, ns, t.units, t.bands);
            SWITCH_METAL_DIAGNOSTIC(stats.learnedDraws++);
            continue;
        }
        if (taken - (t.payload - 1) <= kDrawStampSlots)
            committedTimings_[kept++] = t;
    }
    committedTimings_.resize(kept);
}

}  // namespace gm20b
