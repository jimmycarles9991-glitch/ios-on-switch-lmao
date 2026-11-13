// SPDX-License-Identifier: MIT
#include "raw_state_segment.h"

#include <algorithm>
#include <cstring>
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "raw_owned.h"
#include "raw_pass_writes.h"

namespace raw {

StateSegment StateSegmentHeap::place(const uint32_t *words, uint32_t count)
{
    if (count <= kSegmentBindWords)
        return {};
    std::vector<uint32_t> stream(words, words + count);
    if (const auto found = placedStreams_.find(stream); found != placedStreams_.end())
        return found->second;
    const uint64_t bytes = (uint64_t(count) * 4 + 15) & ~uint64_t(15);
    if (chunks_.empty() || used_ + bytes > chunks_.back().bytes) {
        SegmentChunk chunk;
        if (!grow_ || !grow_(std::max(kChunkBytes, bytes), chunk) || chunk.bytes < bytes)
            return {};
        chunks_.push_back(chunk);
        used_ = 0;
    }
    const SegmentChunk &c = chunks_.back();
    memcpy(c.cpu + used_, words, uint64_t(count) * 4);
    StateSegment s{c.va + used_, count, keptAcrossPasses(words, count)};
    used_ += bytes;
    placed_ += bytes;
    placedStreams_.emplace(std::move(stream), s);
    return s;
}

bool keptAcrossPasses(const uint32_t *words, uint32_t count)
{
    std::vector<switch_gm20b_method> methods(count);
    const long n = switch_gm20b_push_decode(words, count, methods.data(), methods.size());
    if (n < 0)
        return false;
    return std::none_of(methods.begin(), methods.begin() + n, [](const switch_gm20b_method &m) {
        return m.subchannel != 0 || passWrites(m.method);
    });
}

void bindSegment(std::vector<PushBody> &bodies, uint32_t *&start, uint32_t *at, const StateSegment &segment)
{
    if (at > start)
        bodies.push_back({start, at});
    bodies.push_back(PushBody::resident(segment.va, segment.words, 0));
    start = at;
}

void matrixCall(std::vector<PushBody> &bodies, uint32_t *start, uint32_t *at, uint64_t uniformVa, uint32_t words,
                uint64_t clampVa, uint32_t clampWords)
{
    PushBody header{start, at};
    header.incomplete = true;
    bodies.push_back(header);
    bodies.push_back(
        PushBody::resident(uniformVa, words, TegraGM20BPushNoPrefetch | (clampWords ? TegraGM20BPushIncomplete : 0u)));
    if (clampWords)
        bodies.push_back(PushBody::resident(clampVa, clampWords, TegraGM20BPushNoPrefetch));
}

uint64_t appendBodies(PushBatches &pushes, const std::vector<PushBody> &bodies, const uint8_t *ringCpu, uint64_t ringVa)
{
    uint64_t words = 0;
    for (const PushBody &b : bodies) {
        if (b.va) {
            pushes.append({b.va, b.words, b.flags}, false);
        } else if (b.second > b.first) {
            const uint64_t va = ringVa + uint64_t(reinterpret_cast<const uint8_t *>(b.first) - ringCpu);
            pushes.append({va, uint32_t(b.second - b.first), 0});
            if (b.incomplete)
                pushes.back().flags |= TegraGM20BPushIncomplete;
            words += uint64_t(b.second - b.first);
        }
        if (b.batchEnd)
            pushes.finishBatch();
    }
    return words;
}

}  // namespace raw
