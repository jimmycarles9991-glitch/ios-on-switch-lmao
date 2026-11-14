// SPDX-License-Identifier: MIT
// A dispatch of an indirect grid, and a stage-in region read from a buffer, on the raw channel: the dispatch's own QMD and
// root table (raw_compute.cpp) with the words the GPU reads patched in by the compute class's inline engine, its data the
// buffer's words fetched by the channel after the method header, as NVK's pre-Turing nvk_CmdDispatchIndirect2KHR writes
// them. No CPU reads the words.
#include "raw_compute.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "raw_kit.h"

#include <cstring>

namespace raw {
namespace {

constexpr uint32_t kRootBytes = 0x800, kRootGroupCount = 12, kRootPush = 0x700;
constexpr uint32_t kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeightDepth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT);
constexpr uint32_t kSetReference = 0x0050;  // NV906F_SET_REFERENCE: the fetch after it sees what the engines wrote

}  // namespace

void ComputeEncoder::inlineFrom(uint64_t dst, uint64_t src, uint32_t bytes)
{
    room(8);
    p_.inc(NVB1C0_LINE_LENGTH_IN, {bytes, 1, uint32_t(dst >> 32), uint32_t(dst)}, SUBC_COMPUTE);
    *p_.at++ = Push::hdr(5, SUBC_COMPUTE, NVB1C0_LAUNCH_DMA, 1 + (bytes + 3) / 4);
    *p_.at++ =
        NVDEF(NVB1C0, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) | NVDEF(NVB1C0, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    cb_.segment(start_, p_.at);
    cb_.segments_.back().flags |= TegraGM20BPushIncomplete;
    cb_.segments_.append({src, (bytes + 3) / 4, TegraGM20BPushNoPrefetch}, false);
    start_ = p_.at;
}

void ComputeEncoder::setStageInRegionIndirect(const Buffer *region, uint64_t offset)
{
    stageInRegion_ = region;
    stageInRegionOffset_ = offset;
}

// The work before has written and its stores reached L2, and the channel fetches what follows only then: words the GPU
// wrote that a continuation fetches are there (NVK's pre-Turing indirect-read barrier)
static void fetchAfterWrites(Push &p)
{
    p.m(NVB1C0_WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    p.m(NVB1C0_INVALIDATE_SHADER_CACHES, NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE), SUBC_COMPUTE);
    p.m(kSetReference, 0, SUBC_COMPUTE);
}

void ComputeEncoder::stageInFromMemory(const PackKernel &r, uint64_t rootVa, bool fetched)
{
    if (!stageInRegion_ || !r.stageInRegionWord || r.stageInRegionWord + 7u > 32)
        return;
    const uint64_t src = stageInRegion_->va + stageInRegionOffset_,
                   dst = rootVa + kRootPush + 4 * (r.stageInRegionWord - 1u);
    cb_.use(*stageInRegion_, false);
    if (stageInRegion_->hostVisible && !stageInRegion_->wc && stageInRegion_->cpu)
        cb_.clean(stageInRegion_->cpu + stageInRegionOffset_, 24);
    if (!fetched) {
        room(8);
        fetchAfterWrites(p_);
    }
    inlineFrom(dst, src, 12);
    inlineFrom(dst + 16, src + 12, 12);
}

bool ComputeEncoder::dispatchIndirect(const Buffer *arguments, uint64_t offset, const char **why)
{
    constexpr uint32_t kArgumentBytes = 12;
    if (!arguments || !arguments->va || (offset & 3) || offset > arguments->length ||
        arguments->length - offset < kArgumentBytes) {
        *why = "indirect dispatch arguments outside their buffer or not four-byte aligned";
        return false;
    }
    if (!state_) {
        *why = "a dispatch without a kernel";
        return false;
    }
    const PackKernel &r = *state_->rec;
    if (r.regionWord >= 0) {
        *why = "an indirect dispatch of a kernel compiled for the regions of an edge";
        return false;
    }
    if (state_->heapMissing) {
        *why = "no memory for the tables a kernel reads argument buffers through";
        return false;
    }
    if (!prepareDescriptors(why))
        return false;
    cb_.bufferWrites_.anything();
    if (serial_ && dispatched_)
        barrier();
    dispatched_ = true;
    slotTableVa_ = 0;
    const uint32_t none[3] = {};
    const uint32_t region[8] = {};
    uint32_t root[kRootBytes / 4] = {};
    rootWords(r, none, none, setVa_, residualMask_, residuals_, region, root);
    argumentRoot(r, root);
    stageInWords(r, stageIn_, root + kRootPush / 4);
    uint64_t rootVa = 0, qmdVa = 0;
    memcpy(dev_.alloc(dev_.upload_, kRootBytes, 256, rootVa), root, sizeof root);
    rootVa_ = 0;
    uint32_t qmd[kQmdWords];
    if (!qmdWords(*state_, none, rootVa, setVa_, qmd, why))
        return false;
    memcpy(dev_.alloc(dev_.upload_, sizeof qmd, 256, qmdVa), qmd, sizeof qmd);
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += kRootBytes + sizeof qmd);
    cb_.use(*arguments, false);
    if (arguments->hostVisible && !arguments->wc && arguments->cpu)
        cb_.clean(arguments->cpu + offset, kArgumentBytes);
    const bool looping = cb_.loops_.open();
    room(8 + (looping ? SWITCH_GM20B_INLINE_WORDS_WORDS(kQmdWords) : 0));
    if (looping)
        kit(p_, switch_gm20b_compute_inline_words, qmdVa, qmd, kQmdWords);
    fetchAfterWrites(p_);
    stageInFromMemory(r, rootVa, true);
    const uint64_t at = arguments->va + offset;
    inlineFrom(rootVa + kRootGroupCount, at, kArgumentBytes);
    inlineFrom(qmdVa + 4 * kQmdRasterWidth, at, 4);
    inlineFrom(qmdVa + 4 * kQmdRasterHeightDepth, at + 4, 2);
    inlineFrom(qmdVa + 4 * kQmdRasterHeightDepth + 2, at + 8, 2);
    room(8 + Predication::kGuardWords + Predication::kReleaseWords);
    cb_.predication_.guard(p_);
    p_.m(NVB1C0_SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
    p_.m(NVB1C0_SEND_SIGNALING_PCAS_B,
         NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) | NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE),
         SUBC_COMPUTE);
    cb_.predication_.release(p_);
    RAW_DIAGNOSTIC(dev_.stats.dispatches++);
    return true;
}

}  // namespace raw
