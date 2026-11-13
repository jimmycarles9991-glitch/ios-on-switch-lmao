// SPDX-License-Identifier: MIT
#pragma once
#include "../../../TegraGM20B/TegraGM20BABI.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace raw {

enum class CpuCompletionResult {
    Legacy,
    Current,
    Stale,
    Failed
};

struct CpuCompletion {
    bool valid = true;
    uint64_t channelUid = 0, generation = 0;
    uint32_t channel = 0, seqno = 0;
    std::vector<TegraGM20BAbiCpuRange> ranges;

    void coalesce()
    {
        std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
            return a.handle < b.handle || (a.handle == b.handle && a.offset < b.offset);
        });
        size_t count = 0;
        for (const auto range : ranges) {
            if (!range.handle || !range.bytes || range.bytes > UINT64_MAX - range.offset) {
                valid = false;
                return;
            }
            if (count && ranges[count - 1].handle == range.handle &&
                range.offset <= ranges[count - 1].offset + ranges[count - 1].bytes) {
                auto &previous = ranges[count - 1];
                previous.bytes =
                    std::max(previous.offset + previous.bytes, range.offset + range.bytes) - previous.offset;
            } else {
                ranges[count++] = range;
            }
        }
        ranges.resize(count);
    }
};

template <class Submit> CpuCompletionResult completeCpuRanges(const CpuCompletion &completion, Submit submit)
{
    if (!completion.valid)
        return CpuCompletionResult::Failed;
    if (completion.ranges.empty())
        return CpuCompletionResult::Current;
    if (!completion.channelUid || !completion.channel)
        return CpuCompletionResult::Failed;
    for (const auto &range : completion.ranges)
        if (!range.handle || !range.bytes || range.bytes > UINT64_MAX - range.offset)
            return CpuCompletionResult::Failed;
    bool stale = false;
    for (size_t at = 0; at < completion.ranges.size();) {
        const uint32_t count = uint32_t(std::min<size_t>(TegraGM20BMaxJobUses, completion.ranges.size() - at));
        const TegraGM20BAbiCpuComplete request = {completion.channelUid,
                                                  uint64_t(uintptr_t(completion.ranges.data() + at)),
                                                  completion.channel,
                                                  completion.seqno,
                                                  count,
                                                  0};
        TegraGM20BAbiCpuCompleteReply reply = {};
        if (!submit(request, reply) || reply.reserved ||
            uint64_t(reply.current_count) + reply.stale + reply.pending + reply.failed != count || reply.pending ||
            reply.failed || reply.first_error)
            return CpuCompletionResult::Failed;
        stale |= reply.stale != 0;
        at += count;
    }
    return stale ? CpuCompletionResult::Stale : CpuCompletionResult::Current;
}

}  // namespace raw
