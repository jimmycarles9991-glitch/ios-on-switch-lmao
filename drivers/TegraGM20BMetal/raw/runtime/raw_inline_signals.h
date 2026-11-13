// SPDX-License-Identifier: MIT
// Which of a command buffer's submission ranges one raw job carries (RawMetalInlineSignals.mm): a native range, the signals of
// kernel events right after it, and the native range after those, the signals released into their events' words between the
// two ranges' work (raw_event_release.h).
#pragma once
#include <cstddef>
#include <vector>

namespace raw {

struct InlineRangeView {
    bool native = false;      // an encoder range the raw path encodes
    bool inlineable = false;  // a signal of a kernel event, alone in its range
};

struct InlineJoin {
    size_t first, last;  // ranges first..last as one: natives, each pair of them with only inlineable signals between
};

inline std::vector<InlineJoin> inlineJoins(const std::vector<InlineRangeView> &ranges)
{
    std::vector<InlineJoin> joins;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (!ranges[i].native)
            continue;
        size_t j = i + 1;
        while (j < ranges.size() && ranges[j].inlineable)
            ++j;
        if (j == i + 1 || j == ranges.size() || !ranges[j].native)
            continue;
        if (!joins.empty() && joins.back().last == i)
            joins.back().last = j;
        else
            joins.push_back({i, j});
        i = j - 1;
    }
    return joins;
}

}  // namespace raw
