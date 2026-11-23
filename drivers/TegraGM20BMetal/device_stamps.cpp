// SPDX-License-Identifier: MIT
// See gpu.h (the device's reads of its stamp page) and render_encoder.h (the span marks).
#include "gpu.h"
#include "stamps.h"

#include <algorithm>
#include <vector>

namespace gm20b {

uint64_t Device::loopRecordVa(uint32_t stamp) const
{
    return stamps_.va + loopRecordOffset(stamp);
}

LoopContinue Device::loopContinue(uint32_t stamp) const
{
    if (!stamp || !stamps_.cpu)
        return {};
    const auto *record = reinterpret_cast<const volatile uint32_t *>(stamps_.cpu + loopRecordOffset(stamp));
    return gm20b::loopContinue(stamp, record[0], record[1]);
}

bool Device::gpuNanoseconds(uint32_t stamp, uint64_t &ns) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    return stampedNanoseconds(*slot, stamp, ns);
}

#if SWITCH_METAL_DIAGNOSTICS
bool Device::gpuStartUnits(uint32_t stamp, uint64_t &units) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    if (slot->start.payload != stamp || slot->end.payload != stamp)
        return false;
    units = slot->start.ptimer;
    return true;
}

std::vector<StateSpan> Device::longSpans(uint32_t stamp)
{
    if (!spans_.on() || !stamp)
        return {};
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    if (slot->start.payload != stamp || slot->end.payload != stamp)
        return {};
    std::vector<StateSpan> spans = spans_.take(stamp, slot->start.ptimer, slot->end.ptimer);
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const StateSpan &s) {
                                   return s.ns < kLongSpanNs;
                               }),
                spans.end());
    return spans;
}

void RenderEncoder::markSpan(Push &p, uint32_t index, SpanKind kind)
{
    const Texture *t = pass_.color[0].texture;
    const uint32_t kpx = t ? uint32_t(uint64_t(t->width) * t->height * t->samples / 1000) : 0;
    SpanLabel label;
    if (kind == SpanKind::Draws && emitted_) {
        label = drawSpanLabel(*emitted_, emitted_->rec ? emitted_->rec->keyHash : 0, kpx, samples_);
        const Pipeline &pl = *emitted_;
        label.form = pl.splitOf                      ? SpanForm::Twin
                     : pl.freeForm.rung != FetchNone ? SpanForm::FetchFree
                     : pl.storageBits                ? SpanForm::Interlock
                     : pipeline_ && &pl != pipeline_ ? SpanForm::Variant
                                                     : SpanForm::Whole;
        label.pixels = uint32_t(std::min<uint64_t>(drawPixels_, UINT32_MAX));
        if (t) {
            label.format = t->format;
            label.width = uint16_t(std::min<uint32_t>(t->width, 0xffff));
            label.height = uint16_t(std::min<uint32_t>(t->height, 0xffff));
            label.blockLinear = t->blockLinear;
            label.compressed = t->comptags;
        }
    }
    label.kind = kind;
    label.kpx = uint16_t(std::min<uint32_t>(kpx, 0xffff));
    label.samples = samples_;
    dev_->spans_.markAt(p, index, label);
}
#endif

}  // namespace gm20b
