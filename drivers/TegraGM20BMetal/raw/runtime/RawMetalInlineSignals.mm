// SPDX-License-Identifier: MIT
// A kernel event's signal between two encoders on the raw path: one job, the signal released into the event's word after the
// work before it (raw_event_release.h), where a signal used to end the first range's job and start another for the rest.
#import "RawMetalInternal.h"
#include <algorithm>
#include "raw_inline_signals.h"

namespace {

uint64_t gInlineStamp;  // the submit thread's: one per range encoded

bool signalRange(RMCommandBuffer *cb, const RMSegment &range)
{
    return range.event && range.to == range.from + 1 && cb->_plan.legacy(range.from).op == OpSignalEvent;
}

// Its signals' events are the kernel's (a first one imported here, on the committing thread)
bool leased(RMCommandBuffer *cb, const raw::InlineJoin &join)
{
    for (size_t k = join.first + 1; k < join.last; ++k)
        if (cb->_segs[k].event && !sharedEventLease((__bridge id)cb->_plan.legacy(cb->_segs[k].from).obj))
            return false;
    return true;
}

}  // namespace

void inlineSignals(RMCommandBuffer *cb)
{
    cb->_inlineSignals.clear();
    auto &segs = cb->_segs;
    if (segs.size() < 3 || !gRM->eventWords.load(std::memory_order_relaxed))
        return;
    std::vector<raw::InlineRangeView> views;
    for (const RMSegment &range : segs)
        views.push_back({range.raw && !range.event, signalRange(cb, range)});
    const std::vector<raw::InlineJoin> joins = raw::inlineJoins(views);
    for (auto join = joins.rbegin(); join != joins.rend(); ++join) {
        const uint32_t from = segs[join->first].from, to = segs[join->last].to;
        if (!leased(cb, *join))
            continue;
        cb->_plan.prepareRange(from, to);
        if (cb->_plan.nativeRangeRefusal(from, to))
            continue;
        for (size_t k = join->first + 1; k < join->last; ++k)
            if (segs[k].event)
                cb->_inlineSignals.push_back(segs[k].from);
        segs[join->first].to = to;
        segs.erase(segs.begin() + ptrdiff_t(join->first + 1), segs.begin() + ptrdiff_t(join->last + 1));
    }
    std::sort(cb->_inlineSignals.begin(), cb->_inlineSignals.end());
}

std::vector<RMSegment> compatibilityParts(RMCommandBuffer *cb, const RMSegment &range)
{
    std::vector<RMSegment> parts;
    uint32_t from = range.from;
    auto &inlined = cb->_inlineSignals;
    for (auto at = std::lower_bound(inlined.begin(), inlined.end(), size_t(range.from));
         at != inlined.end() && *at < range.to;) {
        if (from < *at)
            parts.push_back({from, uint32_t(*at), false});
        parts.push_back({uint32_t(*at), uint32_t(*at + 1), false, true});
        from = uint32_t(*at + 1);
        at = inlined.erase(at);
    }
    if (from < range.to)
        parts.push_back({from, range.to, false});
    return parts;
}

void beginInlineSignals(RMCommandBuffer *cb)
{
    ++gInlineStamp;
    cb->_released.clear();
}

bool isInlineSignal(RMCommandBuffer *cb, size_t at)
{
    return std::binary_search(cb->_inlineSignals.begin(), cb->_inlineSignals.end(), at);
}

bool releaseInlineSignal(RMCommandBuffer *cb, size_t at, raw::CommandBuffer *commands)
{
    const Cmd c = cb->_plan.legacy(at);
    if (!sharedEventLease((__bridge id)c.obj))
        return false;
    uint64_t address = 0;
    uint32_t payload = 0;
    if (sharedEventWord((__bridge id)c.obj, c.c, gInlineStamp, address, payload))
        commands->releaseEvent(address, payload);
    else
        payload = 0;
    cb->_released.emplace_back(at, payload);
    return true;
}
