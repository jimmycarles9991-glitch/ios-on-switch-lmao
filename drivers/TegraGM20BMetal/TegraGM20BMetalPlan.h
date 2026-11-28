// SPDX-License-Identifier: MIT
#pragma once
#include "dispatch_regions.h"
#include "grow_list.h"
#include "TegraGM20BMetalArguments.h"
#include "pointer_map.h"
#include "semantic_state.h"
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <tuple>

struct SwitchMetalPlanEvent {
    size_t position = 0;
    void *event = nullptr;
    uint64_t value = 0;
    uint32_t timeout = 0;
    bool signal = false;
};

struct SwitchMetalPlanBegin {
    void *descriptor = nullptr;
    uint32_t pass = 0;
    uint32_t dispatchType = 0;
    size_t end = 0;
    uint32_t rasterBegin = 0, rasterCount = 0;
    uint32_t range = gm20b::kNoSnapshot;  // its own prepared range (SwitchMetalCommandPlan::rangeIndex_), once it ended
    uint32_t unfinalDraws = 0;
    bool hasWaitFence = false;
    bool hasNonDraw = false;
};

struct SwitchMetalPlanRasterGroup {
    uint32_t state = 0, draws = 0;
};

struct SwitchMetalPlanDraw {
    uint32_t state = 0;
    uint32_t first = 0, count = 0, instances = 1, baseInstance = 0;
    int32_t baseVertex = 0;
    void *indices = nullptr;
    uint64_t indexOffset = 0;
    void *arguments = nullptr;
    uint64_t argumentOffset = 0;
    uint8_t primitive = 0, indexType = 0;
    uint32_t patches = gm20b::kNoSnapshot;  // a patch draw's SwitchMetalPlanPatches
};

// A patch draw's (OpDrawPatches, OpDrawPatchesIndirect): the encoder's factor buffer, offset, instance stride and scale, its
// control points, and its control-point index buffer (its patch index buffer is the draw's `indices`)
struct SwitchMetalPlanPatches {
    void *factors = nullptr, *pointIndices = nullptr;
    uint64_t factorOffset = 0, pointIndexOffset = 0;
    uint32_t factorStride = 0, controlPoints = 0;
    float scale = 1;
};

struct SwitchMetalPlanDispatch {
    uint32_t state = 0;
    uint32_t surfaceSlots = 0;
    const void *kernel = nullptr;
    void *pending = nullptr;
    size_t encoderBegin = 0;
    std::array<uint64_t, 3> grid{}, groups{};
    std::array<uint32_t, 3> threads{};
    std::array<uint32_t, 6> stageIn{};  // setStageInRegion:'s origin and size
    // dispatchThreadgroupsWithIndirectBuffer:'s buffer (null: a grid of the CPU's) and setStageInRegionWithIndirectBuffer:'s
    // (null: stageIn), each an SwitchMetalBuffer, and their offsets
    void *arguments = nullptr, *stageInBuffer = nullptr;
    uint64_t argumentsOffset = 0, stageInOffset = 0;
    bool perThread = false;
    gm20b::ComputeDispatch regions;
};

struct SwitchMetalPlanTransfer {
    void *source = nullptr, *destination = nullptr;
    uint64_t offset = 0, length = 0, parameters = 0;
    uint32_t aspect = 0;
    uint8_t value = 0;
};

struct SwitchMetalPlanControl {
    void *object = nullptr;
    uint64_t value = 0;
    uint32_t scope = 0, before = 0, after = 0;
    uint32_t compute = gm20b::kNoSnapshot;
    uint8_t stage = 0;
};

struct SwitchMetalPlanOperation {
    Op op = OpEnd;
    uint32_t payload = 0;
    uint32_t uses = 0;
};

enum class SwitchMetalPlanError : uint8_t {
    None,
    NestedEncoder,
    EventInsideEncoder,
    UnclosedEncoder,
    CommandAfterEnd,
    // a command recorded on an encoder that had ended once other work began (an encoder of another kind, an event),
    // which an M4 Max with validation off crashes on
    CommandAfterOtherWork
};

enum class SwitchMetalResourceKind : uint8_t {
    Other,
    Buffer,
    Texture,
    Sampler,
    Heap,
    Count
};

struct SwitchMetalResourceFacts {
    enum Role : uint8_t {
        Converted = 1
    };

    void *cpuBuffer = nullptr;
    SwitchMetalResourceKind kind = SwitchMetalResourceKind::Other;
    uint8_t roles = 0;
    void *surfaceWrites =
        nullptr;  // a texture over a surface: the surface's write count (TegraGM20BMetalSurfaceWrites.mm)
};

struct SwitchMetalRangeResource {
    void *object = nullptr;
    SwitchMetalResourceFacts facts;
    uint8_t access = 0;
};

class SwitchMetalCommandPlan {
    struct UseSpan {
        uint32_t begin = 0, count = 0;
    };

    gm20b::GrowList<SwitchMetalPlanOperation> operations_;
    gm20b::GrowList<SwitchMetalPlanBegin> begins_;
    gm20b::GrowList<SwitchMetalPlanDraw> draws_;
    gm20b::GrowList<SwitchMetalPlanRasterGroup> rasterGroups_;
    uint32_t lastRasterGroup_ = gm20b::kNoSnapshot;
    gm20b::GrowList<SwitchMetalPlanDispatch> dispatches_;
    gm20b::GrowList<SwitchMetalPlanPatches> patches_;
    gm20b::GrowList<SwitchMetalPlanTransfer> transfers_;
    gm20b::GrowList<SwitchMetalPlanControl> controls_;
    gm20b::GrowList<SwitchMetalPlanEvent> events_;
    gm20b::GrowList<gm20b::ResourceUse> uses_;
    // [0] the empty span, which the first saveUses makes: a constructed plan allocates nothing (a command buffer's
    // own is replaced by the recording pool's)
    gm20b::GrowList<UseSpan> useSpans_;
    uint64_t renderGeneration_ = 0, kernelGeneration_ = 0, stateEpoch_ = 0;
    uint64_t eligibilityGeneration_ = 0;
    uint64_t savedRender_ = UINT64_MAX, savedKernel_ = UINT64_MAX;
    uint64_t resourceRevision_[2]{}, savedResources_[2]{UINT64_MAX, UINT64_MAX};
    uint32_t resourceUses_[2]{};
    uint32_t currentRender_ = gm20b::kNoSnapshot, currentKernel_ = gm20b::kNoSnapshot;
    Op encoder_ = OpEnd;
    SwitchMetalPlanError recordingError_ = SwitchMetalPlanError::None;
    size_t encoderBegin_ = 0;

    struct Range {
        size_t from = 0, to = 0;
        uint32_t uses = 0;
        std::vector<void *> pipelines, variants;
        std::vector<size_t> computes;
        std::vector<SwitchMetalRangeResource> resources;
        std::array<uint32_t, size_t(SwitchMetalResourceKind::Count) + 1> first{};
    };

    // the ranges prepared, the first rangeCount_ of them live: a cleared plan keeps them, their lists' capacity too, for
    // the next command buffer's
    std::deque<Range> ranges_;
    size_t rangeCount_ = 0;
    // the live ranges, which a reader of the tape finds by their encoder's begin (SwitchMetalPlanBegin::range) while the writer
    // prepares more
    gm20b::GrowList<const Range *> rangeIndex_;
    gm20b::PointerMap<SwitchMetalResourceFacts> resourceFacts_;
    // prepareRange's and saveUses' working lists, kept from one call to the next
    std::vector<uint32_t> spanMarks_;
    uint32_t spanStamp_ = 0;
    std::vector<gm20b::ResourceUse> mergedUses_;
    std::vector<void *> mergedPipelines_, mergedVariants_;
    std::vector<size_t> mergedComputes_;
    gm20b::PointerMap<uint32_t> usePositions_;

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
    gm20b::StampMap<std::tuple<uint32_t, uintptr_t, uintptr_t>, uint32_t, gm20b::WordsHash> drawUseSpans_;
    using ResourceKey = std::pair<uint64_t, uintptr_t>;
    gm20b::StampMap<ResourceKey, uint32_t, gm20b::WordsHash> kernelUseSpans_;
    // the use lists merged or built before saveUses takes them, and the recording's own (recordUses), kept from one
    // command buffer to the next
    std::vector<gm20b::ResourceUse> mergeUses_, buildUses_, recordUses_;
    // The render use lists resolved last, by resource revision and pipeline: a revision moves with every bind of
    // another resource, so only draws close together share one (a state switched back to over the same resources)
    static constexpr uint32_t kRecentRenderUses = 4;
    std::array<ResourceKey, kRecentRenderUses> recentRenderKeys_{
        {{UINT64_MAX, 0}, {UINT64_MAX, 0}, {UINT64_MAX, 0}, {UINT64_MAX, 0}}};
    std::array<uint32_t, kRecentRenderUses> recentRenderUses_{};
    uint32_t recentRenderAt_ = 0;
    uint32_t drawUses(uint32_t base, void *indices, void *arguments);
    uint32_t captureRender(bool bindingUses = true);
    void forgetRecentRenderUses();
    uint32_t captureKernel();
    uint32_t saveUses(const std::vector<gm20b::ResourceUse> &uses);
    void applyRenderUses(uint32_t state, uint32_t span);
    bool setter(const Cmd &c, const uint64_t *extra);

public:
    static bool isEncoderBegin(Op op)
    {
        return op == OpRender || op == OpCompute || op == OpBlit;
    }

    gm20b::BufferSnapshots buffers[3];
    gm20b::TextureSnapshots textures[2];  // vertex, fragment
    gm20b::KernelTextureSnapshots kernelTextures;
    gm20b::SamplerSnapshots samplers[3];
    gm20b::StateSnapshots<gm20b::RasterState> raster;
    gm20b::StateSnapshots<gm20b::KernelState> kernel;
    gm20b::GrowList<gm20b::RenderStateRef> renderStates;
    gm20b::GrowList<gm20b::KernelStateRef> kernelStates;
    gm20b::GrowList<uint64_t> viewArrays;  // gm20b::RasterState's viewport and scissor arrays

    // `extraWords`: how many words `extra` holds (a dispatch's indirect words are read only where its record has them).
    // `resolvesUses`: a draw's caller resolves its uses (resolveRenderUses) right after, so its state's uses of every
    // bound resource are not listed
    bool record(const Cmd &c, const uint64_t *extra, size_t extraWords = 0, bool resolvesUses = false);
    void recordDraw(uint8_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
                    bool resolvesUses);

    // A binding or raster command (record's setters, one that takes no operation), as record takes it
    void bind(const Cmd &c, const uint64_t *extra)
    {
        setter(c, extra);
    }

    void bindBytes(uint32_t stage, uint32_t slot, uint32_t chunk, uint32_t offset, uint32_t length, uint32_t shadow)
    {
        if (stage >= 2 || slot >= 31)
            return;
        bool resourcesChanged = false;
        buffers[stage].setUnchecked(slot, [&](gm20b::BufferBinding &dst) {
            resourcesChanged = dst.source == gm20b::BufferBinding::Source::Buffer && dst.object != nullptr;
            dst.source = gm20b::BufferBinding::Source::Bytes;
            dst.object = nullptr;
            dst.offset = offset;
            dst.chunk = chunk;
            dst.length = length;
            dst.stride = gm20b::kStaticStride;
            dst.shadow = shadow;
        });
        if (resourcesChanged)
            ++resourceRevision_[0];
        ++renderGeneration_;
        ++eligibilityGeneration_;
    }

    void bindPipeline(void *pipeline, void *nativePipeline)
    {
        const bool changed = raster.update([&](gm20b::RasterState &state) {
            using R = gm20b::RasterState;
            bool diff = !(state.set & R::Pipeline);
            diff |= gm20b::assignState(state.pipeline, pipeline);
            diff |= gm20b::assignState(state.nativePipeline, nativePipeline);
            diff |= gm20b::assignState(state.nativeVariant, static_cast<void *>(nullptr));
            state.set |= R::Pipeline;
            return diff;
        });
        if (changed) {
            ++renderGeneration_;
            ++eligibilityGeneration_;
        }
    }

    void recordRenderBegin(void *descriptor, uint32_t pass)
    {
        if (encoder_ != OpEnd && recordingError_ == SwitchMetalPlanError::None)
            recordingError_ = SwitchMetalPlanError::NestedEncoder;
        encoder_ = OpRender;
        encoderBegin_ = size();
        ++stateEpoch_;
        ++eligibilityGeneration_;
        ++resourceRevision_[0];
        ++resourceRevision_[1];
        for (auto &state : buffers)
            state.resetCurrent();
        for (auto &state : textures)
            state.resetCurrent();
        kernelTextures.resetCurrent();
        for (auto &state : samplers)
            state.resetCurrent();
        raster.resetCurrent();
        kernel.resetCurrent();
        ++renderGeneration_;
        ++kernelGeneration_;
        const uint32_t payload = static_cast<uint32_t>(begins_.size());
        SwitchMetalPlanBegin *begin = begins_.emplaceUninit();
        *begin = SwitchMetalPlanBegin{descriptor, pass, 0};
        begin->rasterBegin = uint32_t(rasterGroups_.size());
        lastRasterGroup_ = gm20b::kNoSnapshot;
        operations_.push_back({OpRender, payload, 0});
    }

    void recordEnd()
    {
        if (encoder_ != OpEnd)
            begins_.own(operations_.own(encoderBegin_).payload).end = size() + 1;
        encoder_ = OpEnd;
        operations_.push_back({OpEnd, 0, 0});
    }

    void bindScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        const bool changed = raster.update([&](gm20b::RasterState &state) {
            const std::array<uint32_t, 4> value{x, y, w, h};
            bool diff = !(state.set & gm20b::RasterState::Scissor);
            diff |= gm20b::assignState(state.scissor, value);
            diff |= gm20b::assignState(state.scissors, uint16_t(1));
            diff |= gm20b::assignState(state.scissorsAt, uint32_t(0));
            state.set |= gm20b::RasterState::Scissor;
            return diff;
        });
        if (changed)
            ++renderGeneration_;
    }

    void bindTexture(uint32_t stage, uint32_t slot, void *object)
    {
        if (stage >= 2)
            return;
        if (textures[stage].bind(slot, object)) {
            ++resourceRevision_[0];
            ++renderGeneration_;
            ++eligibilityGeneration_;
        }
    }

    void bindSampler(uint32_t stage, uint32_t slot, void *object)
    {
        if (stage >= 2)
            return;
        if (samplers[stage].bind(slot, object)) {
            ++resourceRevision_[0];
            ++renderGeneration_;
            ++eligibilityGeneration_;
        }
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
    std::vector<gm20b::ResourceUse> &recordUses()
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

    const SwitchMetalPlanOperation &operation(size_t at) const
    {
        return operations_[at];
    }

    const SwitchMetalPlanOperation &ownOperation(size_t at) const
    {
        return operations_.own(at);
    }

    SwitchMetalPlanError recordingError() const
    {
        return recordingError_ != SwitchMetalPlanError::None ? recordingError_
               : encoder_ != OpEnd                           ? SwitchMetalPlanError::UnclosedEncoder
                                                             : SwitchMetalPlanError::None;
    }

    const SwitchMetalPlanBegin &begin(size_t at) const
    {
        return begins_[operations_[at].payload];
    }

    const SwitchMetalPlanBegin &ownBegin(size_t at) const
    {
        return begins_.own(operations_.own(at).payload);
    }

    const SwitchMetalPlanEvent &event(size_t at) const
    {
        return events_[operations_[at].payload];
    }

    const SwitchMetalPlanDraw &draw(size_t at) const
    {
        return draws_[operations_[at].payload];
    }

    const SwitchMetalPlanDraw &drawAt(uint32_t payload, bool own = false) const
    {
        return own ? draws_.own(payload) : draws_[payload];
    }

    // The writer's (as the draw is recorded)
    const SwitchMetalPlanDraw &ownDraw(size_t at)
    {
        return draws_.own(operations_.own(at).payload);
    }

    const SwitchMetalPlanPatches &patches(size_t at) const
    {
        return patches_[draw(at).patches];
    }

    template <typename F> void forEachRasterGroup(size_t begin, F visit, bool own = false) const
    {
        const auto &pass = own ? begins_.own(operations_.own(begin).payload) : begins_[operations_[begin].payload];
        if (!pass.rasterCount)
            return;
        const SwitchMetalPlanRasterGroup *groups = own ? rasterGroups_.ownData() : rasterGroups_.data();
        const gm20b::RasterState *states = raster.valuesData(own);
        for (uint32_t i = 0; i < pass.rasterCount; ++i) {
            const auto &group = groups[pass.rasterBegin + i];
            visit(states[group.state], group.draws);
        }
    }

    const SwitchMetalPlanDispatch &dispatch(size_t at) const
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

    const SwitchMetalPlanControl &control(size_t at) const
    {
        return controls_[operations_[at].payload];
    }

    const gm20b::GrowList<SwitchMetalPlanEvent> &events() const
    {
        return events_;
    }

    Cmd legacy(size_t at) const;

    void setDescriptor(size_t at, void *descriptor)
    {
        begins_[operations_[at].payload].descriptor = descriptor;
    }

    void registerResource(void *object, SwitchMetalResourceFacts facts);

    const SwitchMetalResourceFacts *factsOf(void *object) const
    {
        return resourceFacts_.find(object);
    }

    template <typename F> void forEachResource(size_t from, size_t to, SwitchMetalResourceKind kind, F visit) const
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
        for (const SwitchMetalRangeResource &resource : found->resources)
            visit(resource);
        return true;
    }

    void addUses(size_t at, const std::vector<gm20b::ResourceUse> &uses);
    void setRenderUses(uint32_t state, const std::vector<gm20b::ResourceUse> &uses);
    void prepareRange(size_t from, size_t to);
    void joinEnded(size_t begin, size_t to);

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
    void swapClean(SwitchMetalCommandPlan &other) noexcept;

    template <typename F> void forEachOperationUse(size_t from, size_t to, F visit) const
    {
        if (const Range *found = findRange(from, to)) {
            const UseSpan span = useSpan(found->uses);
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                visit(use.object, use.access);
            }
            return;
        }
        for (size_t at = from; at < std::min(to, size()); ++at) {
            const UseSpan span = useSpan(operations_[at].uses);
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                if (use.object)
                    visit(use.object, use.access);
            }
        }
    }

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
            if (access & gm20b::ResourceUse::Write)
                visit(object);
        });
    }
};
