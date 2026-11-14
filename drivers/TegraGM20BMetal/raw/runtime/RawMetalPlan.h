// SPDX-License-Identifier: MIT
#pragma once
#include "raw_dispatch_regions.h"
#include "raw_grow_list.h"
#include "RawMetalArguments.h"
#include "raw_pointer_map.h"
#include "raw_semantic_state.h"
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <tuple>

struct RMPlanEvent {
    size_t position = 0;
    void *event = nullptr;
    uint64_t value = 0;
    uint32_t timeout = 0;
    bool signal = false;
};

struct RMPlanBegin {
    void *descriptor = nullptr;
    uint32_t pass = 0;
    uint32_t dispatchType = 0;
    bool refused = false;
    size_t end = 0;
    uint32_t rasterBegin = 0, rasterCount = 0;
    uint32_t range = raw::kNoSnapshot;  // its own prepared range (RMCommandPlan::rangeIndex_), once it ended
};

struct RMPlanRasterGroup {
    uint32_t state = 0, draws = 0;
};

struct RMPlanDraw {
    uint32_t state = 0;
    uint32_t first = 0, count = 0, instances = 1, baseInstance = 0;
    int32_t baseVertex = 0;
    void *indices = nullptr;
    uint64_t indexOffset = 0;
    void *arguments = nullptr;
    uint64_t argumentOffset = 0;
    uint8_t primitive = 0, indexType = 0;
    uint32_t patches = raw::kNoSnapshot;  // a patch draw's RMPlanPatches
};

// A patch draw's (OpDrawPatches, OpDrawPatchesIndirect): the encoder's factor buffer, offset, instance stride and scale, its
// control points, and its control-point index buffer (its patch index buffer is the draw's `indices`)
struct RMPlanPatches {
    void *factors = nullptr, *pointIndices = nullptr;
    uint64_t factorOffset = 0, pointIndexOffset = 0;
    uint32_t factorStride = 0, controlPoints = 0;
    float scale = 1;
};

struct RMPlanDispatch {
    uint32_t state = 0;
    uint32_t surfaceSlots = 0;
    const void *kernel = nullptr;
    void *pending = nullptr;
    size_t encoderBegin = 0;
    std::array<uint64_t, 3> grid{}, groups{};
    std::array<uint32_t, 3> threads{};
    std::array<uint32_t, 6> stageIn{};  // setStageInRegion:'s origin and size
    // dispatchThreadgroupsWithIndirectBuffer:'s buffer (null: a grid of the CPU's) and setStageInRegionWithIndirectBuffer:'s
    // (null: stageIn), each an RMBuffer, and their offsets
    void *arguments = nullptr, *stageInBuffer = nullptr;
    uint64_t argumentsOffset = 0, stageInOffset = 0;
    bool perThread = false;
    raw::ComputeDispatch regions;
};

struct RMPlanTransfer {
    void *source = nullptr, *destination = nullptr;
    uint64_t offset = 0, length = 0, parameters = 0;
    uint32_t aspect = 0;
    uint8_t value = 0;
};

struct RMPlanControl {
    void *object = nullptr;
    uint64_t value = 0;
    uint32_t scope = 0, before = 0, after = 0;
    uint32_t render = raw::kNoSnapshot, compute = raw::kNoSnapshot;
    uint8_t stage = 0;
};

struct RMPlanOperation {
    Op op = OpEnd;
    uint32_t payload = 0;
    uint32_t uses = 0;
};

enum class RMPlanError : uint8_t {
    None,
    NestedEncoder,
    EventInsideEncoder,
    UnclosedEncoder
};

enum class RMResourceKind : uint8_t {
    Other,
    Buffer,
    Texture,
    Sampler,
    Heap,
    Count
};

struct RMResourceFacts {
    enum Role : uint8_t {
        Converted = 1,
        PlanarTarget = 2
    };

    void *cpuBuffer = nullptr;
    RMResourceKind kind = RMResourceKind::Other;
    uint8_t roles = 0;
    void *surfaceWrites = nullptr;  // a texture over a surface: the surface's write count (RawMetalSurfaceWrites.mm)
};

struct RMRangeResource {
    void *object = nullptr;
    RMResourceFacts facts;
    uint8_t access = 0;
};

class RMCommandPlan {
    struct UseSpan {
        uint32_t begin = 0, count = 0;
    };

    raw::GrowList<RMPlanOperation> operations_;
    raw::GrowList<RMPlanBegin> begins_;
    raw::GrowList<RMPlanDraw> draws_;
    raw::GrowList<RMPlanRasterGroup> rasterGroups_;
    uint32_t lastRasterGroup_ = raw::kNoSnapshot;
    raw::GrowList<RMPlanDispatch> dispatches_;
    raw::GrowList<RMPlanPatches> patches_;
    raw::GrowList<RMPlanTransfer> transfers_;
    raw::GrowList<RMPlanControl> controls_;
    raw::GrowList<RMPlanEvent> events_;
    raw::GrowList<raw::ResourceUse> uses_;
    // [0] the empty span, which the first saveUses makes: a constructed plan allocates nothing (a command buffer's
    // own is replaced by the recording pool's)
    raw::GrowList<UseSpan> useSpans_;
    uint64_t renderGeneration_ = 0, kernelGeneration_ = 0, stateEpoch_ = 0;
    uint64_t eligibilityGeneration_ = 0;
    uint64_t savedRender_ = UINT64_MAX, savedKernel_ = UINT64_MAX;
    uint64_t resourceRevision_[2]{}, savedResources_[2]{UINT64_MAX, UINT64_MAX};
    uint32_t resourceUses_[2]{};
    uint32_t currentRender_ = raw::kNoSnapshot, currentKernel_ = raw::kNoSnapshot;
    Op encoder_ = OpEnd;
    RMPlanError recordingError_ = RMPlanError::None;
    size_t encoderBegin_ = 0;

    struct Range {
        size_t from = 0, to = 0;
        uint32_t uses = 0;
        std::vector<void *> pipelines, variants;
        std::vector<size_t> computes;
        std::vector<RMRangeResource> resources;
        std::array<uint32_t, size_t(RMResourceKind::Count) + 1> first{};
        void *planarFeedback = nullptr;
    };

    // the ranges prepared, the first rangeCount_ of them live: a cleared plan keeps them, their lists' capacity too, for
    // the next command buffer's
    std::deque<Range> ranges_;
    size_t rangeCount_ = 0;
    // the live ranges, which a reader of the tape finds by their encoder's begin (RMPlanBegin::range) while the writer
    // prepares more
    raw::GrowList<const Range *> rangeIndex_;
    raw::PointerMap<RMResourceFacts> resourceFacts_;
    // prepareRange's and saveUses' working lists, kept from one call to the next
    std::vector<uint32_t> spanMarks_;
    uint32_t spanStamp_ = 0;
    std::vector<raw::ResourceUse> mergedUses_;
    std::vector<void *> mergedPipelines_, mergedVariants_;
    std::vector<size_t> mergedComputes_;
    raw::PointerMap<uint32_t> usePositions_;

    UseSpan useSpan(uint32_t index) const
    {
        return index ? useSpans_[index] : UseSpan{};
    }

    // The writer's (or the one thread that has the plan after its commit)
    UseSpan ownSpan(uint32_t index)
    {
        return index ? useSpans_.own(index) : UseSpan{};
    }

    const Range *findRange(size_t from, size_t to) const;
    const Range &rangeAt(size_t from, size_t to) const;
    void classifyRange(Range &range);
    size_t rangeStorageBytes() const;
    raw::StampMap<std::tuple<uint32_t, uintptr_t, uintptr_t>, uint32_t, raw::WordsHash> drawUseSpans_;
    using ResourceKey = std::pair<uint64_t, uintptr_t>;
    raw::StampMap<ResourceKey, uint32_t, raw::WordsHash> kernelUseSpans_;
    // the use lists merged or built before saveUses takes them, and the recording's own (recordUses), kept from one
    // command buffer to the next
    std::vector<raw::ResourceUse> mergeUses_, buildUses_, recordUses_;
    // The render use lists resolved last, by resource revision and pipeline: a revision moves with every bind of
    // another resource, so only draws close together share one (a state switched back to over the same resources)
    static constexpr uint32_t kRecentRenderUses = 4;
    std::array<ResourceKey, kRecentRenderUses> recentRenderKeys_{};
    std::array<uint32_t, kRecentRenderUses> recentRenderUses_{};
    uint32_t recentRenderAt_ = 0;
    uint32_t drawUses(uint32_t base, void *indices, void *arguments);
    uint32_t captureRender(bool bindingUses = true);
    void forgetRecentRenderUses();
    uint32_t captureKernel();
    uint32_t saveUses(const std::vector<raw::ResourceUse> &uses);
    void applyRenderUses(uint32_t state, uint32_t span);
    bool setter(const Cmd &c, const uint64_t *extra);

public:
    static bool isEncoderBegin(Op op)
    {
        return op == OpRender || op == OpCompute || op == OpBlit;
    }

    raw::BufferSnapshots buffers[3];
    raw::TextureSnapshots textures[3];
    raw::SamplerSnapshots samplers[3];
    raw::StateSnapshots<raw::RasterState> raster;
    raw::StateSnapshots<raw::KernelState> kernel;
    raw::GrowList<raw::RenderStateRef> renderStates;
    raw::GrowList<raw::KernelStateRef> kernelStates;
    raw::GrowList<uint64_t> viewArrays;  // raw::RasterState's viewport and scissor arrays

    // `extraWords`: how many words `extra` holds (a dispatch's indirect words are read only where its record has them).
    // `resolvesUses`: a draw's caller resolves its uses (resolveRenderUses) right after, so its state's uses of every
    // bound resource are not listed
    bool record(const Cmd &c, const uint64_t *extra, size_t extraWords = 0, bool resolvesUses = false);

    // A binding or raster command (record's setters, one that takes no operation), as record takes it
    void bind(const Cmd &c, const uint64_t *extra)
    {
        setter(c, extra);
    }

    // `build` fills the list it is handed with the dispatch's uses, where the kernel and resource revision are new
    template <typename Build> void setKernelUses(size_t at, const void *nativeKernel, Build build)
    {
        auto &operation = operations_.own(at);
        const auto &state = kernelStates.own(dispatches_.own(operation.payload).state);
        const auto key = std::make_pair(state.resourceRevision, reinterpret_cast<uintptr_t>(nativeKernel));
        auto [found, inserted] = kernelUseSpans_.insert(key, 0);
        if (inserted) {
            buildUses_.clear();
            build(buildUses_);
            *found = saveUses(buildUses_);
        }
        operation.uses = *found;
    }

    // `build` fills the list it is handed with the draw's uses, where the pipeline and resource revision are new
    template <typename Build> void resolveRenderUses(uint32_t state, const void *nativePipeline, Build build)
    {
        const ResourceKey key{renderStates.own(state).resourceRevision, reinterpret_cast<uintptr_t>(nativePipeline)};
        for (uint32_t i = 0, at = recentRenderAt_; i < kRecentRenderUses;
             ++i, at = (at + kRecentRenderUses - 1) % kRecentRenderUses)
            if (recentRenderKeys_[at] == key)
                return applyRenderUses(state, recentRenderUses_[at]);
        buildUses_.clear();
        build(buildUses_);
        const uint32_t uses = saveUses(buildUses_);
        recentRenderAt_ = (recentRenderAt_ + 1) % kRecentRenderUses;
        recentRenderKeys_[recentRenderAt_] = key;
        recentRenderUses_[recentRenderAt_] = uses;
        applyRenderUses(state, uses);
    }

    // An empty list for the recording's uses of one command, its capacity the largest one's so far
    std::vector<raw::ResourceUse> &recordUses()
    {
        recordUses_.clear();
        return recordUses_;
    }

    // whether a draw recorded now captures a new render state (a binding or the raster state moved since the last)
    bool capturesRender() const
    {
        return savedRender_ != renderGeneration_;
    }

    size_t size() const
    {
        return operations_.size();
    }

    bool empty() const
    {
        return operations_.empty();
    }

    const RMPlanOperation &operation(size_t at) const
    {
        return operations_[at];
    }

    RMPlanError recordingError() const
    {
        return recordingError_ != RMPlanError::None ? recordingError_
               : encoder_ != OpEnd                  ? RMPlanError::UnclosedEncoder
                                                    : RMPlanError::None;
    }

    const RMPlanBegin &begin(size_t at) const
    {
        return begins_[operations_[at].payload];
    }

    const RMPlanEvent &event(size_t at) const
    {
        return events_[operations_[at].payload];
    }

    const RMPlanDraw &draw(size_t at) const
    {
        return draws_[operations_[at].payload];
    }

    // The writer's (as the draw is recorded)
    const RMPlanDraw &ownDraw(size_t at)
    {
        return draws_.own(operations_.own(at).payload);
    }

    const RMPlanPatches &patches(size_t at) const
    {
        return patches_[draw(at).patches];
    }

    template <typename F> void forEachRasterGroup(size_t begin, F visit) const
    {
        const auto &pass = begins_[operations_[begin].payload];
        for (uint32_t i = 0; i < pass.rasterCount; ++i) {
            const auto &group = rasterGroups_[pass.rasterBegin + i];
            visit(raster[group.state], group.draws);
        }
    }

    const RMPlanDispatch &dispatch(size_t at) const
    {
        return dispatches_[operations_[at].payload];
    }

    void resolveDispatch(size_t at, const void *kernel, uint32_t surfaceSlots)
    {
        auto &dispatch = dispatches_[operations_[at].payload];
        dispatch.kernel = kernel;
        dispatch.surfaceSlots = surfaceSlots;
        dispatch.pending = nullptr;
    }

    void invalidateUses()
    {
        rangeCount_ = 0;
        rangeIndex_.clear();
    }

    const RMPlanControl &control(size_t at) const
    {
        return controls_[operations_[at].payload];
    }

    const raw::GrowList<RMPlanEvent> &events() const
    {
        return events_;
    }

    Cmd legacy(size_t at) const;

    void setDescriptor(size_t at, void *descriptor)
    {
        begins_[operations_[at].payload].descriptor = descriptor;
    }

    void setRefused(size_t at, bool refused)
    {
        auto &begin = begins_[operations_[at].payload];
        begin.refused = refused || nativeRangeRefusal(at, begin.end);
    }

    void registerResource(void *object, RMResourceFacts facts);

    const RMResourceFacts *factsOf(void *object) const
    {
        return resourceFacts_.find(object);
    }

    void *nativeRangeRefusal(size_t from, size_t to) const;

    template <typename F> void forEachResource(size_t from, size_t to, RMResourceKind kind, F visit) const
    {
        const auto &range = rangeAt(from, to);
        const size_t index = size_t(kind);
        for (uint32_t i = range.first[index]; i < range.first[index + 1]; ++i)
            visit(range.resources[i]);
    }

    // every resource of the range [from, to) prepareRange cached, with its facts; false: none cached
    template <typename F> bool forEachRangeResource(size_t from, size_t to, F visit) const
    {
        const Range *found = findRange(from, to);
        if (!found)
            return false;
        for (const RMRangeResource &resource : found->resources)
            visit(resource);
        return true;
    }

    void addUses(size_t at, const std::vector<raw::ResourceUse> &uses);
    void setRenderUses(uint32_t state, const std::vector<raw::ResourceUse> &uses);
    void prepareRange(size_t from, size_t to);

    size_t encoderEnd(size_t begin) const
    {
        return begins_[operations_[begin].payload].end;
    }

    uint64_t renderGeneration() const
    {
        return renderGeneration_;
    }

    uint64_t eligibilityGeneration() const
    {
        return eligibilityGeneration_;
    }

    // moves where the encoder's bindings start over: a new encoder, an opaque call
    uint64_t stateEpoch() const
    {
        return stateEpoch_;
    }

    const std::vector<void *> &renderPipelines(size_t from, size_t to) const
    {
        return rangeAt(from, to).pipelines;
    }

    const std::vector<void *> &renderVariants(size_t from, size_t to) const
    {
        return rangeAt(from, to).variants;
    }

    const std::vector<size_t> &pendingComputes(size_t from, size_t to) const
    {
        return rangeAt(from, to).computes;
    }

    size_t storageBytes() const;
    void clear();

    template <typename F> void forEachUse(size_t from, size_t to, F visit) const
    {
        if (const Range *found = findRange(from, to)) {
            const UseSpan span = useSpan(found->uses);
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                visit(use.object, use.access);
            }
            return;
        }
        std::unordered_map<void *, uint8_t> unioned;
        for (size_t at = from; at < std::min(to, size()); ++at) {
            const UseSpan span = useSpan(operations_[at].uses);
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                if (use.object)
                    unioned[use.object] |= use.access;
            }
        }
        for (const auto &use : unioned)
            visit(use.first, use.second);
    }

    template <typename F> void forEachWritten(size_t from, size_t to, F visit) const
    {
        forEachUse(from, to, [&](void *object, uint8_t access) {
            if (access & raw::ResourceUse::Write)
                visit(object);
        });
    }
};
