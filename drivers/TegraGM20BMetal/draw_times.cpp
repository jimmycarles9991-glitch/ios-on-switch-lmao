// SPDX-License-Identifier: MIT
// DEVELOPMENT: the frame-weights capture's hooks in the encoder (draw_times.h, draw_capture.h). Release builds
// compile none of it.
#include "gpu.h"

#if SWITCH_METAL_DIAGNOSTICS
#include <os/log.h>
#include <stdlib.h>
#include <algorithm>
#include <cstring>
#include "tiled_cache.h"

namespace gm20b {

void Device::askDrawTimes(uint32_t buffers)
{
    drawTimes_.ask(buffers);
}

// The marks' memory, made at the first capture and kept: the GPU's reports land in it, the CPU reads it after completion
bool Device::openDrawTimes()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (!k_.allocControl(uint64_t(kTimedMarks) * sizeof(TimedMark), drawTimeMarks_, ControlUse::Stamps))
        return false;
    memset(drawTimeMarks_.cpu, 0, drawTimeMarks_.bytes);
    if (set_.add(k_, drawTimeMarks_, TegraGM20BSurfaceWrite))
        return true;
    k_.free(drawTimeMarks_);
    drawTimeMarks_ = {};
    return false;
}

// At begin(), in the submission prologue `p`: a captured command buffer counts fragment invocations and starts with its
// first mark; the first one after a capture stops the counter
void Device::beginDrawTimes(CommandBuffer &cb, Push &p)
{
    cb.drawTimes_ = nullptr;
    if (drawTimes_.asking()) {
        if (!drawTimeMarks_.va && !openDrawTimes()) {
            drawTimes_.cancel();
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
                         "SwitchMetal: fw refused: no memory the GPU may write for the capture's marks");
            return;
        }
        cb.drawTimes_ = drawTimes_.begin(cb.stamp);
    }
    if (cb.drawTimes_) {
        p.m(NVB197_SET_STATISTICS_COUNTER, kStatisticsPsInvocations);
        countingInvocations_ = true;
        TimedLabel start;
        start.kind = TimedKind::Start;
        emitDrawTime(p, drawTimes_.reserve(*cb.drawTimes_, start));
    } else if (countingInvocations_) {
        p.m(NVB197_SET_STATISTICS_COUNTER, 0);
        countingInvocations_ = false;
    }
}

void Device::emitDrawTime(Push &p, uint32_t index)
{
    if (index != kNoMark)
        emitTimedMark(p, drawTimeMarks_.va + uint64_t(index) * sizeof(TimedMark), drawTimes_.serial(),
                      tiledCache(k_.info()));
}

void Device::committedDrawTimes(CommandBuffer &cb, uint32_t seqno)
{
    drawTimes_.commit(*cb.drawTimes_, seqno, channelGeneration());
    cb.drawTimes_ = nullptr;
}

// Every completer: the committed command buffers of a capture the GPU has run, logged
void Device::reportDrawTimes()
{
    if (!drawTimes_.committed())
        return;
    const auto ran = [this](uint32_t seqno, uint64_t generation) {
        if (channelGeneration() != generation)
            return -1;
        if (!done(seqno))
            return 0;
        return lost(seqno) ? -1 : 1;
    };
    const auto stamps = [this](uint32_t stamp, uint64_t &start, uint64_t &end) {
        const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
        if (slot->start.payload != stamp || slot->end.payload != stamp)
            return false;
        start = slot->start.ptimer;
        end = slot->end.ptimer;
        return end >= start;
    };
    const auto gpuNs = [this](uint32_t stamp) {
        uint64_t ns = 0;
        return gpuNanoseconds(stamp, ns) ? int64_t(ns) : int64_t(-1);
    };
    const auto *marks = reinterpret_cast<const volatile TimedMark *>(drawTimeMarks_.cpu);
    const os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    for (const std::string &line : drawTimes_.report(marks, getprogname(), ran, stamps, gpuNs))
        os_log(log, "SwitchMetal: fw %{public}s", line.c_str());
}

DrawTimesTaken Device::drawTimesTaken()
{
    reportDrawTimes();
    return drawTimes_.taken();
}

// At the pass's begin, ahead of its draws' marks: the two its prologue writes, one where it starts (what ran since the last
// mark) and one where it ends
void RenderEncoder::timePassBegin()
{
    TimedBuffer &b = *cb_->drawTimes_;
    TimedPass tp;
    if (const Texture *t = pass_.color[0].texture ? pass_.color[0].texture : pass_.depthStencil.texture) {
        tp.format = t->format;
        tp.width = uint16_t(std::min<uint32_t>(t->width, 0xffff));
        tp.height = uint16_t(std::min<uint32_t>(t->height, 0xffff));
        tp.samples = t->samples;
        tp.blockLinear = t->blockLinear;
        tp.compressed = t->comptags;
    }
    DrawTimes &times = dev_->drawTimes_;
    TimedLabel l;
    l.pass = timedPass_ = times.pass(b, tp);
    l.kind = TimedKind::Between;
    betweenTime_ = times.reserve(b, l);
    l.kind = TimedKind::Prologue;
    prologueTime_ = times.reserve(b, l);
}

TimedState RenderEncoder::timedState(const Pipeline &drawn) const
{
    const Pipeline &named = drawn.caName || drawn.spanName || !pipeline_ ? drawn : *pipeline_;
    TimedState s;
    s.key = named.rec ? named.rec->keyHash : drawn.rec ? drawn.rec->keyHash : 0;
    s.name = named.caName ? named.caName : named.spanName;
    s.fragmentWords = drawn.fragmentInstructions;
    s.vertexWords = drawn.vertexInstructions;
    s.gprs = uint8_t(std::min<uint32_t>(pixelRegisters(drawn.methods.data(), drawn.methods.size()), 0xff));
    s.samplePasses = drawn.samplePasses;
    s.form = uint8_t(drawn.splitOf                      ? SpanForm::Twin
                     : drawn.freeForm.rung != FetchNone ? SpanForm::FetchFree
                     : drawn.storageBits                ? SpanForm::Interlock
                     : pipeline_ && &drawn != pipeline_ ? SpanForm::Variant
                                                        : SpanForm::Whole);
    return s;
}

// After a draw's words (all its bands): its mark, labelled with what it drew
void RenderEncoder::timeDraw(uint32_t vertices, uint32_t instances, bool indirect)
{
    DrawTimes &times = dev_->drawTimes_;
    TimedLabel l;
    l.kind = indirect ? TimedKind::Indirect : TimedKind::Draw;
    l.pass = timedPass_;
    l.topology = uint8_t(std::min<uint32_t>(topology_, 0xff));
    l.vertices = vertices;
    l.instances = instances;
    l.box = uint32_t(std::min<uint64_t>(drawPixels_, UINT32_MAX));
    if (const Pipeline *drawn = emitted_)
        l.state = times.state(drawn, [&] {
            return timedState(*drawn);
        });
    const uint32_t index = times.reserve(*cb_->drawTimes_, l);
    if (index == kNoMark)
        return;
    if (p_.room() < kTimedMarkWords)
        reserve();
    dev_->emitDrawTime(p_, index);
}

}  // namespace gm20b
#endif
