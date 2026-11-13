// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

enum class ExecutionKind : uint8_t {
    native,
    compatibility,
    wait,
    signal
};

struct ExecutionRange {
    size_t from, to;
    ExecutionKind kind;
    bool gpuWait = false;
};

struct ExecutionAttribution {
    size_t native = 0, compatibility = 0;

    void add(bool raw, bool event)
    {
        if (!event)
            ++(raw ? native : compatibility);
    }

    bool mixed() const
    {
        return native && compatibility;
    }

    bool onlyCompatibility() const
    {
        return !native && compatibility;
    }
};

// A range never crosses an event. Adjacent passes of one backend can share a submission, but an event's exact position
// is retained even when no encoder exists on either side. The caller supplies complete encoder ranges, not draw fragments.
// Every wait is one the GPU may take (gpuWait): the next raw range's job carries it, or a job of its own does before
// signals, m2v work or the command buffer's end (RawMetalEventJobs.h submitWaitJob).
class ExecutionPlan {
    std::vector<ExecutionRange> ranges_;

public:
    void append(size_t from, size_t to, ExecutionKind kind)
    {
        if (from == to)
            return;
        const bool work = kind == ExecutionKind::native || kind == ExecutionKind::compatibility;
        if (work && !ranges_.empty() && ranges_.back().kind == kind && ranges_.back().to == from)
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

// A native range whose encoder [from, to) the raw path refused while encoding it: that encoder alone goes to m2v, and
// the encoders before and after it stay native, each side a range of its own. An encoder not inside the range, or
// none, sends the whole range to m2v.
inline std::vector<ExecutionRange> isolate(const ExecutionRange &range, size_t from, size_t to)
{
    if (from < range.from || to > range.to || from >= to)
        return {{range.from, range.to, ExecutionKind::compatibility}};
    std::vector<ExecutionRange> parts;
    if (range.from < from)
        parts.push_back({range.from, from, ExecutionKind::native});
    parts.push_back({from, to, ExecutionKind::compatibility});
    if (to < range.to)
        parts.push_back({to, range.to, ExecutionKind::native});
    return parts;
}

}  // namespace raw
