// SPDX-License-Identifier: MIT
// RenderEncoder's draws in scissor bands (raw_draw_bands.h), priced by their state's learned rate (raw_band_rates.h).
#include <os/log.h>
#include "raw_gpu.h"
#include "raw_mme.h"
#include "raw_stamps.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"

namespace raw {

// a draw's shortcuts around its words (raw_draw_shortcuts.cpp), as raw_gpu.cpp reserves them
constexpr uint32_t kShortcutWords = 2 * (SWITCH_GM20B_CONSTANT_COLOR_WORDS + SWITCH_GM20B_FILL_VIA_TRIANGLE_WORDS);

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
void RenderEncoder::noteCandidate(uint64_t units)
{
    if (passDraws_ != 1 || dev_->compositorClass())
        return;
    candidate_ = {&emitted_->bandRate,           units, 0, 0, emitted_->rec ? emitted_->rec->keyHash : 0,
                  emitted_->fragmentInstructions};
}

// At the pass's end, `drained` where its draws' writes are out of the tiled cache: whether its one draw is measured, its
// stamps' slot taken. The start stamp goes at the end of the pass's prologue and the end stamp after the drain, so the
// measured draw keeps every word and every flush it has unmeasured.
bool RenderEncoder::measurePass(bool drained)
{
    if (!drained || passDraws_ != 1 || !candidate_.rate || cb_->timings.size() >= kMeasuredPerCommandBuffer ||
        !measure(*candidate_.rate, candidate_.units))
        return false;
    const uint32_t n = dev_->drawStamps_.fetch_add(1, std::memory_order_relaxed);
    candidate_.slot = n % kDrawStampSlots;
    candidate_.payload = n + 1 ? n + 1 : 1;
    cb_->timings.push_back(candidate_);
    return true;
}

void RenderEncoder::emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                             uint32_t baseInstance, const Shortcut &cut)
{
    const auto words = [&](Push &p) {
        shortcutBegin(cut);
        mme::draw(p, params_, indexed, first, cut.bbox ? 3 : count, instances, root, baseInstance);
        shortcutEnd(cut);
    };
    const uint64_t units = drawUnits(instances);
    const uint32_t bands = drawBands(count, instances, units);
    noteCandidate(units);
    if (bands < 2) {
        if (p_.room() < kBandWords + kShortcutWords)
            reserve();
        words(p_);
        finishDraw(count, instances);
        return;
    }
    if (cb_->batchWork_.operations)
        cutBatch();
    const PixelBox area = {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]};
    emitBands(
        p_, rot_, area, drawBox_, bands,
        [&] {
            if (p_.room() < kBandWords + kShortcutWords)
                reserve();
        },
        words,
        [&] {
            cutBatch();
        });
    cb_->bands += bands;
    RAW_DIAGNOSTIC(dev_->stats.bandedDraws++; dev_->stats.bands += bands);
}

void Device::noteCommittedTimings(std::vector<DrawTiming> &timings)
{
    if (timings.empty())
        return;
    std::lock_guard<std::mutex> l(timingsLock_);
    committedTimings_.insert(committedTimings_.end(), timings.begin(), timings.end());
    timings.clear();
    committedTimingCount_.store(uint32_t(committedTimings_.size()), std::memory_order_release);
}

// Every completer calls this (the Metal facade's finish, a raw-direct client after its wait): each committed measured
// draw whose two stamps the GPU wrote teaches its state, one whose slot a later draw took is dropped, the rest wait
void Device::learnCompleted()
{
    if (!committedTimingCount_.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> l(timingsLock_);
    const uint32_t taken = drawStamps_.load(std::memory_order_relaxed);
    size_t kept = 0;
    for (const DrawTiming &t : committedTimings_) {
        const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + drawStampOffset(t.slot, false));
        uint64_t ns = 0;
        if (stampedNanoseconds(*slot, t.payload, ns)) {
            learn(*t.rate, ns, t.units);
            RAW_DIAGNOSTIC(stats.learnedDraws++);
#if SWITCH_RAW_DIAGNOSTICS
            if (ns >= kLongDrawNs)
                os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                       "raw: long draw %llu us k%016llx %llu kunits %u instr, learned %u ps a unit",
                       (unsigned long long)(ns / 1000), (unsigned long long)t.key, (unsigned long long)(t.units / 1000),
                       t.instructions, t.rate->psPerUnit.load(std::memory_order_relaxed));
#endif
            continue;
        }
        if (taken - (t.payload - 1) <= kDrawStampSlots)
            committedTimings_[kept++] = t;
    }
    committedTimings_.resize(kept);
    committedTimingCount_.store(uint32_t(kept), std::memory_order_release);
}

}  // namespace raw
