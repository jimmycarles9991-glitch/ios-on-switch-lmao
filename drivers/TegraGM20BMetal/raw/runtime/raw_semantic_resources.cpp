// SPDX-License-Identifier: MIT
#include "RawMetalPlan.h"

void RMCommandPlan::registerResource(void *object, RMResourceFacts facts)
{
    if (object)
        resourceFacts_.insert(object, facts);
}

// An encoder's own range by its begin (what a reader of the tape asks for, which reads no list the writer extends); any
// other by a look through the live ones
const RMCommandPlan::Range *RMCommandPlan::findRange(size_t from, size_t to) const
{
    if (from < to && isEncoderBegin(operations_[from].op)) {
        const RMPlanBegin &begin = begins_[operations_[from].payload];
        if (begin.end == to && begin.range < rangeIndex_.size()) {
            const Range *own = rangeIndex_[begin.range];
            if (own->from == from && own->to == to)
                return own;
        }
    }
    for (size_t i = 0; i < rangeCount_; ++i)
        if (ranges_[i].from == from && ranges_[i].to == to)
            return &ranges_[i];
    return nullptr;
}

const RMCommandPlan::Range &RMCommandPlan::rangeAt(size_t from, size_t to) const
{
    static const Range none;
    const Range *found = findRange(from, to);
    return found ? *found : none;
}

// The range's resources with their facts, ordered by kind (Range::first), in the order of their uses within a kind
void RMCommandPlan::classifyRange(Range &range)
{
    const UseSpan span = ownSpan(range.uses);
    range.resources.clear();
    range.first = {};
    for (uint32_t i = 0; i < span.count; ++i) {
        const auto &use = uses_.own(span.begin + i);
        const RMResourceFacts *found = resourceFacts_.find(use.object);
        const RMResourceFacts facts = found ? *found : RMResourceFacts{};
        range.resources.push_back({use.object, facts, use.access});
        range.first[size_t(facts.kind) + 1]++;
        const uint8_t feedback = raw::ResourceUse::Sample | raw::ResourceUse::Write;
        if (facts.kind == RMResourceKind::Texture &&
            (facts.roles & (RMResourceFacts::Converted | RMResourceFacts::PlanarTarget)) ==
                (RMResourceFacts::Converted | RMResourceFacts::PlanarTarget) &&
            (use.access & feedback) == feedback)
            range.planarFeedback = use.object;
    }
    for (size_t kind = 1; kind <= size_t(RMResourceKind::Count); ++kind)
        range.first[kind] += range.first[kind - 1];
    // a stable counting sort: each kind's resources from its first slot, in order
    std::array<uint32_t, size_t(RMResourceKind::Count)> next{};
    for (size_t kind = 0; kind < next.size(); ++kind)
        next[kind] = range.first[kind];
    const size_t count = range.resources.size();
    range.resources.resize(2 * count);
    for (size_t i = 0; i < count; ++i)
        range.resources[count + next[size_t(range.resources[i].facts.kind)]++] = range.resources[i];
    std::copy(range.resources.begin() + count, range.resources.end(), range.resources.begin());
    range.resources.resize(count);
}

void *RMCommandPlan::nativeRangeRefusal(size_t from, size_t to) const
{
    const Range *found = findRange(from, to);
    return found && found->computes.empty() ? found->planarFeedback : nullptr;
}

size_t RMCommandPlan::rangeStorageBytes() const
{
    size_t bytes = resourceFacts_.storageBytes() + usePositions_.storageBytes() +
                   spanMarks_.capacity() * sizeof(uint32_t) + mergedUses_.capacity() * sizeof(raw::ResourceUse) +
                   (mergedPipelines_.capacity() + mergedVariants_.capacity()) * sizeof(void *) +
                   mergedComputes_.capacity() * sizeof(size_t);
    for (const Range &range : ranges_)
        bytes += sizeof(Range) + (range.pipelines.capacity() + range.variants.capacity()) * sizeof(void *) +
                 range.computes.capacity() * sizeof(size_t) + range.resources.capacity() * sizeof(RMRangeResource);
    return bytes;
}

void RMCommandPlan::prepareRange(size_t from, size_t to)
{
    if (findRange(from, to))
        return;
    const size_t end = std::min(to, size());
    // the encoders inside first, so the merge below runs alone on the working lists
    for (size_t at = from; at < end;) {
        const auto &operation = operations_.own(at);
        if (operation.op == OpRender || operation.op == OpCompute || operation.op == OpBlit) {
            const size_t encoderEnd = begins_.own(operation.payload).end;
            if (encoderEnd > at && encoderEnd <= to && (at != from || encoderEnd != to)) {
                prepareRange(at, encoderEnd);
                at = encoderEnd;
                continue;
            }
        }
        ++at;
    }
    if (spanMarks_.size() < useSpans_.size())
        spanMarks_.resize(useSpans_.size(), 0);
    if (!++spanStamp_) {
        std::fill(spanMarks_.begin(), spanMarks_.end(), 0);
        spanStamp_ = 1;
    }
    mergedUses_.clear();
    mergedPipelines_.clear();
    mergedVariants_.clear();
    mergedComputes_.clear();
    auto variant = [&](void *object) {
        if (object && std::find(mergedVariants_.begin(), mergedVariants_.end(), object) == mergedVariants_.end())
            mergedVariants_.push_back(object);
    };
    auto pipeline = [&](void *object) {
        if (object && std::find(mergedPipelines_.begin(), mergedPipelines_.end(), object) == mergedPipelines_.end())
            mergedPipelines_.push_back(object);
    };
    auto span = [&](uint32_t index) {
        if (!index || spanMarks_[index] == spanStamp_)
            return;
        spanMarks_[index] = spanStamp_;
        const UseSpan use = useSpans_.own(index);
        mergedUses_.insert(mergedUses_.end(), uses_.ownData() + use.begin, uses_.ownData() + use.begin + use.count);
    };
    for (size_t at = from; at < end;) {
        const auto &operation = operations_.own(at);
        if (operation.op == OpRender || operation.op == OpCompute || operation.op == OpBlit) {
            const size_t encoderEnd = begins_.own(operation.payload).end;
            const Range *cached = encoderEnd > at && encoderEnd <= to && (at != from || encoderEnd != to)
                                      ? findRange(at, encoderEnd)
                                      : nullptr;
            if (cached) {
                span(cached->uses);
                for (void *object : cached->pipelines)
                    pipeline(object);
                for (void *object : cached->variants)
                    variant(object);
                mergedComputes_.insert(mergedComputes_.end(), cached->computes.begin(), cached->computes.end());
                at = encoderEnd;
                continue;
            }
        }
        span(operation.uses);
        if (isPlanDraw(operation.op)) {
            const auto &state = raster.own(renderStates.own(draws_.own(operation.payload).state).raster);
            pipeline(state.pipeline);
            variant(state.nativeVariant);
        } else if (operation.op == OpInvocation && controls_.own(operation.payload).render != raw::kNoSnapshot)
            pipeline(raster.own(renderStates.own(controls_.own(operation.payload).render).raster).pipeline);
        else if (operation.op == OpDispatch && dispatches_.own(operation.payload).pending)
            mergedComputes_.push_back(at);
        ++at;
    }
    const uint32_t uses = saveUses(mergedUses_);
    if (rangeCount_ == ranges_.size())
        ranges_.emplace_back();
    Range &range = ranges_[rangeCount_++];
    range.from = from;
    range.to = to;
    range.uses = uses;
    range.pipelines.assign(mergedPipelines_.begin(), mergedPipelines_.end());
    range.variants.assign(mergedVariants_.begin(), mergedVariants_.end());
    range.computes.assign(mergedComputes_.begin(), mergedComputes_.end());
    range.planarFeedback = nullptr;
    classifyRange(range);
    rangeIndex_.push_back(&range);
    if (from < to && isEncoderBegin(operations_.own(from).op) && begins_.own(operations_.own(from).payload).end == to)
        begins_.own(operations_.own(from).payload).range = uint32_t(rangeIndex_.size() - 1);
}
