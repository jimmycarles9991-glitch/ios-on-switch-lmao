// SPDX-License-Identifier: MIT
#include "TegraGM20BMetalPlan.h"
#include <cstring>
#include <unordered_set>

namespace {

template <typename List> uint32_t append(List &values, typename List::value_type value)
{
    const auto index = static_cast<uint32_t>(values.size());
    values.push_back(std::move(value));
    return index;
}

// The bound slots alone are visited: a slot never bound holds nil
template <typename Textures>
void appendBindings(std::vector<gm20b::ResourceUse> &uses, const gm20b::BufferBindings &buffers,
                    const Textures &textures, uint8_t access)
{
    for (const auto &binding : buffers)
        if (binding.source == gm20b::BufferBinding::Source::Buffer && binding.object)
            uses.push_back({binding.object, access});
    gm20b::forEachSlot(textures.set, [&](uint32_t slot) {
        if (void *texture = textures.objects[slot])
            uses.push_back({texture, uint8_t(access | gm20b::ResourceUse::Sample)});
    });
}

}  // namespace

uint32_t SwitchMetalCommandPlan::saveUses(const std::vector<gm20b::ResourceUse> &uses)
{
    // a draw's few uses are merged by a scan of the span so far, an encoder's many by their positions
    constexpr size_t kScanned = 16;
    if (uses.empty())
        return 0;
    const uint32_t begin = static_cast<uint32_t>(uses_.size());
    if (uses.size() <= kScanned) {
        for (const auto &use : uses) {
            if (!use.object)
                continue;
            uint32_t at = begin;
            while (at < uses_.size() && uses_.own(at).object != use.object)
                ++at;
            if (at == uses_.size())
                uses_.push_back(use);
            else
                uses_.own(at).access |= use.access;
        }
    } else {
        usePositions_.clear();
        for (const auto &use : uses) {
            if (!use.object)
                continue;
            const auto [position, inserted] = usePositions_.insert(use.object, static_cast<uint32_t>(uses_.size()));
            if (inserted)
                uses_.push_back(use);
            else
                uses_.own(*position).access |= use.access;
        }
    }
    if (useSpans_.empty())
        useSpans_.push_back({});
    return append(useSpans_, {begin, static_cast<uint32_t>(uses_.size()) - begin});
}

uint32_t SwitchMetalCommandPlan::captureRender(bool bindingUses)
{
    if (savedRender_ == renderGeneration_)
        return currentRender_;
    gm20b::RenderStateRef state;
    state.epoch = stateEpoch_;
    state.resourceRevision = resourceRevision_[0];
    for (uint32_t stage = 0; stage < 2; ++stage) {
        state.buffers[stage] = buffers[stage].capture();
        state.textures[stage] = textures[stage].capture();
        state.samplers[stage] = samplers[stage].capture();
    }
    state.raster = raster.capture();
    if (bindingUses && savedResources_[0] != resourceRevision_[0]) {
        mergeUses_.clear();
        for (uint32_t stage = 0; stage < 2; ++stage)
            appendBindings(mergeUses_, buffers[stage].current(), textures[stage].current(), gm20b::ResourceUse::Read);
        resourceUses_[0] = saveUses(mergeUses_);
        savedResources_[0] = resourceRevision_[0];
    }
    state.uses = bindingUses ? resourceUses_[0] : 0;
    currentRender_ = append(renderStates, state);
    savedRender_ = renderGeneration_;
    return currentRender_;
}

uint32_t SwitchMetalCommandPlan::captureKernel()
{
    if (savedKernel_ == kernelGeneration_)
        return currentKernel_;
    if (savedResources_[1] != resourceRevision_[1]) {
        mergeUses_.clear();
        appendBindings(mergeUses_, buffers[2].current(), kernelTextures.current(),
                       gm20b::ResourceUse::Read | gm20b::ResourceUse::Write);
        resourceUses_[1] = saveUses(mergeUses_);
        savedResources_[1] = resourceRevision_[1];
    }
    currentKernel_ = append(kernelStates, {buffers[2].capture(), kernelTextures.capture(), samplers[2].capture(),
                                           kernel.capture(), resourceUses_[1], stateEpoch_, resourceRevision_[1]});
    savedKernel_ = kernelGeneration_;
    return currentKernel_;
}

bool SwitchMetalCommandPlan::record(const Cmd &c, const uint64_t *extra, size_t extraWords, bool resolvesUses)
{
    // A visibility mode set on a pass with a result buffer is also an operation: the raw encode emits each one in order
    // (visibility.h), sets that no draw follows included
    if (setter(c, extra) && !(c.op == OpVisibility && c.obj))
        return false;
    SwitchMetalPlanOperation operation;
    operation.op = c.op;
    switch (c.op) {
    case OpRender:
    case OpBlit:
    case OpCompute:
        if (encoder_ != OpEnd && recordingError_ == SwitchMetalPlanError::None)
            recordingError_ = SwitchMetalPlanError::NestedEncoder;
        encoder_ = c.op;
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
        operation.payload = append(begins_, {c.obj, uint32_t(c.x), c.a});
        begins_.back().rasterBegin = uint32_t(rasterGroups_.size());
        lastRasterGroup_ = gm20b::kNoSnapshot;
        break;
    case OpDraw:
    case OpDrawIndexed:
    case OpDrawIndirect:
    case OpDrawIndexedIndirect:
    case OpExecuteCommands:
    case OpDrawPatches:
    case OpDrawPatchesIndirect: {
        SwitchMetalPlanDraw draw;
        draw.state = captureRender(!resolvesUses);
        draw.primitive = c.stage;
        if (c.op == OpDrawPatches || c.op == OpDrawPatchesIndirect) {
            const uint64_t *e = extra + c.x;
            if (c.op == OpDrawPatches) {
                draw.first = c.a;
                draw.count = c.b;
                draw.instances = uint32_t(c.c >> 32);
                draw.baseInstance = uint32_t(c.c);
            }
            draw.indices = c.obj;
            draw.indexOffset = e[PatchIndexOffset];
            draw.arguments = c.obj2;
            draw.argumentOffset = e[PatchArgumentOffset];
            SwitchMetalPlanPatches patches;
            patches.factors = reinterpret_cast<void *>(uintptr_t(e[PatchFactors]));
            patches.pointIndices = reinterpret_cast<void *>(uintptr_t(e[PatchPointIndices]));
            patches.factorOffset = e[PatchFactorOffset];
            patches.pointIndexOffset = e[PatchPointIndexOffset];
            patches.factorStride = uint32_t(e[PatchFactorStride]);
            patches.controlPoints = uint32_t(e[PatchControlPoints]);
            const uint32_t scale = uint32_t(e[PatchScale]);
            memcpy(&patches.scale, &scale, sizeof scale);
            draw.patches = append(patches_, patches);
        } else if (c.op == OpDraw) {
            draw.first = c.a;
            draw.count = c.b;
            draw.instances = uint32_t(c.c >> 32);
            draw.baseInstance = uint32_t(c.c);
        } else if (c.op == OpDrawIndexed) {
            draw.count = c.a;
            draw.indexType = uint8_t(c.b);
            draw.indices = c.obj;
            draw.indexOffset = c.c;
            draw.instances = uint32_t(extra[c.x]);
            draw.baseVertex = int32_t(extra[c.x + 1]);
            draw.baseInstance = uint32_t(extra[c.x + 2]);
        } else {
            draw.indices = c.obj;
            draw.indexType = uint8_t(c.a);
            draw.indexOffset = c.c;
            draw.arguments = c.obj2;
            draw.argumentOffset = c.x;
            if (c.op == OpExecuteCommands)
                draw.first = c.b;  // the range buffer's offset (TegraGM20BMetalIndirect.mm)
        }
        operation.uses = renderStates.own(draw.state).uses;
        if (encoder_ == OpRender) {
            if (draw.count && draw.instances) {
                const uint32_t state = renderStates.own(draw.state).raster;
                // a raster snapshot is captured after every one before it: a state other than the last group's is new
                if (lastRasterGroup_ == gm20b::kNoSnapshot || rasterGroups_.own(lastRasterGroup_).state != state) {
                    lastRasterGroup_ = uint32_t(rasterGroups_.size());
                    rasterGroups_.push_back({state, 0});
                    ++begins_.own(operations_.own(encoderBegin_).payload).rasterCount;
                }
                ++rasterGroups_.own(lastRasterGroup_).draws;
            }
        } else if (encoder_ != OpEnd && recordingError_ == SwitchMetalPlanError::None) {
            // a draw of a render encoder that had ended, recorded inside the blit or compute encoder begun since
            recordingError_ = SwitchMetalPlanError::CommandAfterOtherWork;
        }
        operation.payload = append(draws_, draw);
        break;
    }
    case OpDispatch: {
        SwitchMetalPlanDispatch dispatch;
        dispatch.state = captureKernel();
        dispatch.surfaceSlots = c.a;
        dispatch.kernel = c.obj2;
        dispatch.pending = c.obj;
        dispatch.encoderBegin = encoderBegin_;
        dispatch.perThread = c.index;
        dispatch.threads = {uint32_t(c.c & 0xffff), uint32_t((c.c >> 16) & 0xffff), uint32_t(c.c >> 32)};
        std::copy_n(extra + c.x, 3, dispatch.grid.begin());
        std::copy_n(extra + c.x + 3, 3, dispatch.groups.begin());
        std::copy_n(extra + c.x + 6, 6, dispatch.stageIn.begin());
        if (extraWords >=
            c.x + 16) {  // the indirect forms' words (SwitchMetalComputeEncoder records them with every dispatch)
            dispatch.arguments = reinterpret_cast<void *>(uintptr_t(extra[c.x + 12]));
            dispatch.argumentsOffset = extra[c.x + 13];
            dispatch.stageInBuffer = reinterpret_cast<void *>(uintptr_t(extra[c.x + 14]));
            dispatch.stageInOffset = extra[c.x + 15];
        }
        dispatch.regions = gm20b::computeDispatch(dispatch.grid, dispatch.threads, dispatch.perThread);
        operation.uses = kernelStates.own(dispatch.state).uses;
        operation.payload = append(dispatches_, dispatch);
        break;
    }
    case OpSignalEvent:
    case OpWaitEvent:
        if (encoder_ != OpEnd && recordingError_ == SwitchMetalPlanError::None)
            recordingError_ = SwitchMetalPlanError::EventInsideEncoder;
        operation.payload = append(events_, {size(), c.obj, c.c, c.a, c.op == OpSignalEvent});
        break;
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer:
    case OpFillBuffer:
    case OpFillTexture:
    case OpMipmaps:
    case OpResetCommands:
    case OpCopyCommands: {
        const bool writes =
            c.op == OpFillBuffer || c.op == OpFillTexture || c.op == OpMipmaps || c.op == OpResetCommands;
        operation.payload = append(transfers_, {c.obj, c.obj2, c.c, 0, c.x, c.a, uint8_t(c.a)});
        operation.uses = saveUses({{c.obj, uint8_t(writes ? gm20b::ResourceUse::Write : gm20b::ResourceUse::Read)},
                                   {c.obj2, gm20b::ResourceUse::Write}});
        break;
    }
    case OpEnd:
        if (encoder_ != OpEnd)
            begins_.own(operations_.own(encoderBegin_).payload).end = size() + 1;
        encoder_ = OpEnd;
        break;
    default: {
        SwitchMetalPlanControl control{c.obj, c.c, c.a, uint32_t(c.c), c.b, gm20b::kNoSnapshot, c.stage};
        // what a buffer of dispatches inherits: the encoder's pipeline and buffers
        if (c.op == OpExecuteKernels && encoder_ == OpCompute) {
            control.compute = captureKernel();
            operation.uses = kernelStates.own(control.compute).uses;
        }
        operation.payload = append(controls_, control);
        if (c.op == OpUse || c.op == OpKernelUse) {
            const uint32_t usage = c.op == OpUse ? c.a : uint32_t(c.c);
            operation.uses = saveUses(
                {{c.obj,
                  uint8_t((usage & 2 ? gm20b::ResourceUse::Write : 0) | (usage & 5 ? gm20b::ResourceUse::Read : 0) |
                          (usage & 4 ? gm20b::ResourceUse::Sample : 0) | gm20b::ResourceUse::Explicit)}});
        }
        break;
    }
    }
    operations_.push_back(operation);
    if (c.op == OpDispatch) {
        const auto &last = dispatch(size() - 1);
        if (last.arguments || last.stageInBuffer)
            addUses(size() - 1,
                    {{last.arguments, gm20b::ResourceUse::Read}, {last.stageInBuffer, gm20b::ResourceUse::Read}});
    }
    if (isPlanDraw(c.op)) {
        const SwitchMetalPlanDraw &last = draws_.own(operation.payload);
        const bool resolvedAfter =
            resolvesUses && !renderStates.own(last.state).usesFinal && last.patches == gm20b::kNoSnapshot;
        if (!resolvedAfter)
            operations_.back().uses = drawUses(operations_.back().uses, last.indices, last.arguments);
        if (last.patches != gm20b::kNoSnapshot)
            addUses(size() - 1, {{patches_.own(last.patches).factors, gm20b::ResourceUse::Read},
                                 {patches_.own(last.patches).pointIndices, gm20b::ResourceUse::Read}});
    }
    return true;
}

uint32_t SwitchMetalCommandPlan::drawUses(uint32_t base, void *indices, void *arguments)
{
    if (!indices && !arguments)
        return base;
    const auto key =
        std::make_tuple(base, reinterpret_cast<uintptr_t>(indices), reinterpret_cast<uintptr_t>(arguments));
    if (const uint32_t *found = drawUseSpans_.find(key))
        return *found;
    const UseSpan span = ownSpan(base);
    mergeUses_.assign(uses_.ownData() + span.begin, uses_.ownData() + span.begin + span.count);
    mergeUses_.push_back({indices, gm20b::ResourceUse::Read});
    mergeUses_.push_back({arguments, gm20b::ResourceUse::Read});
    const uint32_t result = saveUses(mergeUses_);
    drawUseSpans_.insert(key, result);
    return result;
}

void SwitchMetalCommandPlan::addUses(size_t at, const std::vector<gm20b::ResourceUse> &uses)
{
    const UseSpan old = ownSpan(operations_.own(at).uses);
    mergeUses_.assign(uses_.ownData() + old.begin, uses_.ownData() + old.begin + old.count);
    mergeUses_.insert(mergeUses_.end(), uses.begin(), uses.end());
    operations_.own(at).uses = saveUses(mergeUses_);
}

void SwitchMetalCommandPlan::setRenderUses(uint32_t state, const std::vector<gm20b::ResourceUse> &uses)
{
    applyRenderUses(state, saveUses(uses));
}

void SwitchMetalCommandPlan::applyRenderUses(uint32_t state, uint32_t span)
{
    renderStates.own(state).uses = span;
    renderStates.own(state).usesFinal = true;
    const SwitchMetalPlanDraw &last = draws_.own(operations_.back().payload);
    operations_.back().uses = drawUses(span, last.indices, last.arguments);
}

Cmd SwitchMetalCommandPlan::legacy(size_t at) const
{
    const auto &operation = operations_[at];
    Cmd c = cmd(operation.op);
    switch (c.op) {
    case OpRender:
    case OpBlit:
    case OpCompute: {
        const auto &begin = begins_[operation.payload];
        c.obj = begin.descriptor;
        c.x = begin.pass;
        c.a = begin.dispatchType;
        break;
    }
    case OpDraw:
    case OpDrawIndexed:
    case OpDrawIndirect:
    case OpDrawIndexedIndirect:
    case OpExecuteCommands:
    case OpDrawPatches:
    case OpDrawPatchesIndirect: {
        const auto &draw = draws_[operation.payload];
        c.stage = draw.primitive;
        c.a = c.op == OpDraw ? draw.first : draw.count;
        c.b = c.op == OpDraw ? draw.count : draw.indexType;
        c.c = c.op == OpDraw ? uint64_t(draw.instances) << 32 | draw.baseInstance : draw.indexOffset;
        c.obj = draw.indices;
        c.x = operation.payload;
        break;
    }
    case OpDispatch: {
        const auto &dispatch = dispatches_[operation.payload];
        c.a = dispatch.surfaceSlots;
        c.index = dispatch.perThread;
        c.obj2 = const_cast<void *>(dispatch.kernel);
        c.x = operation.payload;
        break;
    }
    case OpSignalEvent:
    case OpWaitEvent: {
        const auto &event = events_[operation.payload];
        c.obj = event.event;
        c.c = event.value;
        c.a = event.timeout;
        break;
    }
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer:
    case OpFillBuffer:
    case OpFillTexture:
    case OpMipmaps:
    case OpResetCommands:
    case OpCopyCommands: {
        const auto &transfer = transfers_[operation.payload];
        c.obj = transfer.source;
        c.obj2 = transfer.destination;
        c.c = transfer.offset;
        c.x = transfer.parameters;
        c.a = transfer.aspect;
        break;
    }
    case OpEnd:
        break;
    default: {
        const auto &control = controls_[operation.payload];
        c.obj = control.object;
        c.c = control.value;
        c.a = control.scope;
        c.b = control.after;
        c.stage = control.stage;
        break;
    }
    }
    return c;
}

size_t SwitchMetalCommandPlan::storageBytes() const
{
    size_t snapshots = raster.storageBytes() + kernel.storageBytes();
    for (const auto &state : buffers)
        snapshots += state.storageBytes();
    for (const auto &state : textures)
        snapshots += state.storageBytes();
    snapshots += kernelTextures.storageBytes();
    for (const auto &state : samplers)
        snapshots += state.storageBytes();
    snapshots += renderStates.capacity() * sizeof(gm20b::RenderStateRef) +
                 kernelStates.capacity() * sizeof(gm20b::KernelStateRef);
    return snapshots + operations_.capacity() * sizeof(SwitchMetalPlanOperation) +
           draws_.capacity() * sizeof(SwitchMetalPlanDraw) + dispatches_.capacity() * sizeof(SwitchMetalPlanDispatch) +
           patches_.capacity() * sizeof(SwitchMetalPlanPatches) +
           transfers_.capacity() * sizeof(SwitchMetalPlanTransfer) +
           controls_.capacity() * sizeof(SwitchMetalPlanControl) + begins_.capacity() * sizeof(SwitchMetalPlanBegin) +
           events_.capacity() * sizeof(SwitchMetalPlanEvent) + uses_.capacity() * sizeof(gm20b::ResourceUse) +
           useSpans_.capacity() * sizeof(UseSpan) + rasterGroups_.capacity() * sizeof(SwitchMetalPlanRasterGroup) +
           rangeStorageBytes() + drawUseSpans_.storageBytes() + kernelUseSpans_.storageBytes() +
           (mergeUses_.capacity() + buildUses_.capacity() + recordUses_.capacity()) * sizeof(gm20b::ResourceUse);
}

void SwitchMetalCommandPlan::clear()
{
    operations_.clear();
    begins_.clear();
    draws_.clear();
    rasterGroups_.clear();
    lastRasterGroup_ = gm20b::kNoSnapshot;
    dispatches_.clear();
    patches_.clear();
    transfers_.clear();
    controls_.clear();
    events_.clear();
    uses_.clear();
    useSpans_.clear();
    renderStates.clear();
    kernelStates.clear();
    viewArrays.clear();
    for (auto &state : buffers)
        state.clear();
    for (auto &state : textures)
        state.clear();
    kernelTextures.clear();
    for (auto &state : samplers)
        state.clear();
    raster.clear();
    kernel.clear();
    renderGeneration_ = kernelGeneration_ = stateEpoch_ = eligibilityGeneration_ = 0;
    savedRender_ = savedKernel_ = UINT64_MAX;
    resourceRevision_[0] = resourceRevision_[1] = 0;
    savedResources_[0] = savedResources_[1] = UINT64_MAX;
    resourceUses_[0] = resourceUses_[1] = 0;
    forgetRecentRenderUses();
    currentRender_ = currentKernel_ = gm20b::kNoSnapshot;
    encoder_ = OpEnd;
    recordingError_ = SwitchMetalPlanError::None;
    encoderBegin_ = 0;
    rangeCount_ = 0;
    rangeIndex_.clear();
    resourceFacts_.clear();
    drawUseSpans_.clear();
    kernelUseSpans_.clear();
}

void SwitchMetalCommandPlan::forgetRecentRenderUses()
{
    recentRenderKeys_.fill({UINT64_MAX, 0});
    recentRenderUses_.fill(0);
    recentRenderAt_ = 0;
}
