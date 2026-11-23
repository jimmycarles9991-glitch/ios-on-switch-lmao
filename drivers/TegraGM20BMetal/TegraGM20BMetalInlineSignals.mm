// SPDX-License-Identifier: MIT
// A kernel event's signal between two encoders on the driver: one job, the signal released into the event's word after the
// work before it (event_release.h), where a signal used to end the first range's job and start another for the rest.
#import "TegraGM20BMetalInternal.h"
#include <algorithm>
#include "inline_signals.h"

namespace {

uint64_t gInlineStamp;  // the submit thread's: one per range encoded

bool signalRange(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range)
{
    return range.event && range.to == range.from + 1 && cb->_plan.legacy(range.from).op == OpSignalEvent;
}

// Its signals' events are the kernel's (a first one imported here, on the committing thread)
bool leased(SwitchMetalCommandBuffer *cb, const gm20b::InlineJoin &join)
{
    for (size_t k = join.first + 1; k < join.last; ++k)
        if (cb->_segs[k].event && !sharedEventLease((__bridge id)cb->_plan.legacy(cb->_segs[k].from).obj))
            return false;
    return true;
}

}  // namespace

void inlineSignals(SwitchMetalCommandBuffer *cb)
{
    cb->_inlineSignals.clear();
    auto &segs = cb->_segs;
    if (segs.size() < 3 || !gRM->eventWords.load(std::memory_order_relaxed))
        return;
    std::vector<gm20b::InlineRangeView> views;
    for (const SwitchMetalSegment &range : segs)
        views.push_back({!range.event, signalRange(cb, range)});
    const std::vector<gm20b::InlineJoin> joins = gm20b::inlineJoins(views);
    for (auto join = joins.rbegin(); join != joins.rend(); ++join) {
        const uint32_t from = segs[join->first].from, to = segs[join->last].to;
        if (!leased(cb, *join))
            continue;
        cb->_plan.prepareRange(from, to);
        for (size_t k = join->first + 1; k < join->last; ++k)
            if (segs[k].event)
                cb->_inlineSignals.push_back(segs[k].from);
        segs[join->first].to = to;
        segs.erase(segs.begin() + ptrdiff_t(join->first + 1), segs.begin() + ptrdiff_t(join->last + 1));
    }
    std::sort(cb->_inlineSignals.begin(), cb->_inlineSignals.end());
}

void beginInlineSignals(SwitchMetalCommandBuffer *cb)
{
    ++gInlineStamp;
    cb->_released.clear();
}

bool isInlineSignal(SwitchMetalCommandBuffer *cb, size_t at)
{
    return std::binary_search(cb->_inlineSignals.begin(), cb->_inlineSignals.end(), at);
}

bool releaseInlineSignal(SwitchMetalCommandBuffer *cb, size_t at, gm20b::CommandBuffer *commands)
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
