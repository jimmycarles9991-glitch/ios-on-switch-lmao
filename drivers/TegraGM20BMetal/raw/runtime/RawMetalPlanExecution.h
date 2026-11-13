// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalPlan.h"
#include "raw_execution_plan.h"

template <class Observable>
RMPlanError makeExecutionPlan(const RMCommandPlan &commands, bool whole, raw::ExecutionPlan &result,
                              Observable observable)
{
    const auto error = commands.recordingError();
    if (error != RMPlanError::None)
        return error;
    for (size_t at = 0; at < commands.size();) {
        const auto op = commands.operation(at).op;
        if (op == OpRender || op == OpCompute || op == OpBlit) {
            const auto &begin = commands.begin(at);
            if (begin.end <= at || begin.end > commands.size())
                return RMPlanError::UnclosedEncoder;
            result.append(at, begin.end,
                          !whole && !begin.refused ? raw::ExecutionKind::native : raw::ExecutionKind::compatibility);
            at = begin.end;
        } else {
            raw::ExecutionKind kind = raw::ExecutionKind::compatibility;
            if ((op == OpSignalEvent || op == OpWaitEvent) && observable(commands.event(at)))
                kind = op == OpSignalEvent ? raw::ExecutionKind::signal : raw::ExecutionKind::wait;
            result.append(at, at + 1, kind);
            ++at;
        }
    }
    return RMPlanError::None;
}
