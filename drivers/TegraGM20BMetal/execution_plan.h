// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm20b {

enum class ExecutionKind : uint8_t {
    native,
    wait,
    signal
};

struct ExecutionRange {
    size_t from, to;
    ExecutionKind kind;
    bool gpuWait = false;
};

// A range never crosses an event. Adjacent encoders can share a submission, but an event's exact position is retained
// even when no encoder exists on either side. The caller supplies complete encoder ranges, not draw fragments.
// Every wait is one the GPU may take (gpuWait): the next raw range's job carries it, or a job of its own does before
// signals or the command buffer's end (TegraGM20BMetalEventJobs.h submitWaitJob).
class ExecutionPlan {
    std::vector<ExecutionRange> ranges_;

public:
    void append(size_t from, size_t to, ExecutionKind kind)
    {
        if (from == to)
            return;
        if (kind == ExecutionKind::native && !ranges_.empty() && ranges_.back().kind == kind &&
            ranges_.back().to == from)
            ranges_.back().to = to;
        else
            ranges_.push_back({from, to, kind, kind == ExecutionKind::wait});
    }

    const std::vector<ExecutionRange> &ranges() const
    {
        return ranges_;
    }

    void clear()
    {
        ranges_.clear();
    }
};

}  // namespace gm20b
