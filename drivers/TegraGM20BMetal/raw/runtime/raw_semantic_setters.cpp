// SPDX-License-Identifier: MIT
#include "RawMetalPlan.h"

// A binding command's attributeStride: its x holds the stride + 1 (0: a form of none, MTLAttributeStrideStatic)
static uint32_t bindStride(const Cmd &c)
{
    return c.x && c.x <= raw::kStaticStride ? uint32_t(c.x - 1) : raw::kStaticStride;
}

bool RMCommandPlan::setter(const Cmd &c, const std::vector<uint64_t> &extra)
{
    const bool compute = c.op >= OpKernelState && c.op <= OpKernelMemory;
    const uint32_t stage = compute ? 2 : c.stage ? c.stage - 1 : 0;
    bool changed = false;
    bool resourcesChanged = false;
    switch (c.op) {
    case OpBuffer:
    case OpKernelBuffer:
    case OpBytes:
    case OpKernelBytes:
        if (stage < 3 && c.index < 31)
            changed = buffers[stage].update(c.index, [&](auto &bindings) {
                raw::BufferBinding value;
                value.source = c.op == OpBytes || c.op == OpKernelBytes ? raw::BufferBinding::Source::Bytes
                                                                        : raw::BufferBinding::Source::Buffer;
                value.object = c.obj;
                value.offset = value.source == raw::BufferBinding::Source::Bytes ? c.a : c.c;
                value.chunk = static_cast<uint32_t>(c.c);
                value.length = c.b;
                value.stride = bindStride(c);
                if (value.source == raw::BufferBinding::Source::Buffer)
                    value.chunk = 0;
                else
                    value.shadow = uint32_t(c.x);
                const auto resource = [](const auto &binding) {
                    return binding.source == raw::BufferBinding::Source::Buffer ? binding.object : nullptr;
                };
                resourcesChanged = resource(bindings[c.index]) != resource(value);
                return raw::assignState(bindings[c.index], value);
            });
        break;
    case OpKernelClockAnchors:
        if (stage < 3 && c.index < 31)
            changed = buffers[stage].update([&](auto &bindings) {
                raw::BufferBinding value;
                value.source = raw::BufferBinding::Source::ClockAnchors;
                value.offset = c.c;
                value.length = c.b;
                resourcesChanged =
                    bindings[c.index].source == raw::BufferBinding::Source::Buffer && bindings[c.index].object;
                return raw::assignState(bindings[c.index], value);
            });
        break;
    case OpBufferOffset:
    case OpKernelBufferOffset:
        if (stage < 3 && c.index < 31)
            changed = buffers[stage].update(c.index, [&](auto &bindings) {
                bool changed = raw::assignState(bindings[c.index].offset, c.c);
                changed |= raw::assignState(bindings[c.index].stride, bindStride(c));
                if (bindings[c.index].source == raw::BufferBinding::Source::Unset) {
                    bindings[c.index].source = raw::BufferBinding::Source::Offset;
                    changed = true;
                }
                return changed;
            });
        break;
    case OpTexture:
    case OpKernelTexture:
        if (stage < 3)
            changed = textures[stage].bind(c.index, c.obj);
        resourcesChanged = changed;
        break;
    case OpSampler:
    case OpKernelSampler:
        if (stage < 3)
            changed = samplers[stage].bind(c.index, c.obj);
        resourcesChanged = changed;
        break;
    case OpKernelState:
        changed = kernel.update([&](auto &state) {
            if (state.pipelineSet && state.pipeline == c.obj)
                return false;
            state.pipelineSet = true;
            state.pipeline = c.obj;
            return true;
        });
        break;
    case OpKernelMemory:
        if (c.index < 31)
            changed = kernel.update([&](auto &state) {
                const uint32_t bit = 1u << c.index;
                if ((state.memorySet & bit) && state.memory[c.index] == c.a)
                    return false;
                state.memorySet |= bit;
                state.memory[c.index] = c.a;
                return true;
            });
        break;
    case OpPipeline:
    case OpDepthStencil:
    case OpScissor:
    case OpViewport:
    case OpCull:
    case OpWinding:
    case OpFillMode:
    case OpDepthClip:
    case OpBlendColor:
    case OpStencilRef:
    case OpDepthBias:
    case OpVisibility:
    case OpAmplification:
        changed = raster.update([&](raw::RasterState &state) {
            using R = raw::RasterState;
            uint32_t bit = 0;
            bool difference = false;
            switch (c.op) {
            case OpPipeline:
                bit = R::Pipeline;
                difference = raw::assignState(state.pipeline, c.obj);
                difference |= raw::assignState(state.nativePipeline, reinterpret_cast<void *>(uintptr_t(c.c)));
                difference |= raw::assignState(state.nativeVariant, c.obj2);
                break;
            case OpDepthStencil:
                bit = R::DepthStencil;
                difference = raw::assignState(state.depthStencil, c.obj);
                break;
            case OpScissor: {
                bit = R::Scissor;
                const uint16_t count = std::max<uint16_t>(c.index, 1);
                std::array<uint32_t, 4> value{c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c)};
                uint32_t at = 0;
                if (count > 1) {
                    for (size_t i = 0; i < value.size(); ++i)
                        value[i] = uint32_t(extra[c.x + i]);
                    at = uint32_t(viewArrays.size());
                    viewArrays.insert(viewArrays.end(), extra.begin() + c.x, extra.begin() + c.x + 4 * count);
                }
                difference = raw::assignState(state.scissor, value);
                difference |= raw::assignState(state.scissors, count);
                difference |= raw::assignState(state.scissorsAt, at);
                break;
            }
            case OpViewport: {
                bit = R::Viewport;
                const uint16_t count = std::max<uint16_t>(c.index, 1);
                std::array<uint64_t, 6> value;
                std::copy_n(extra.begin() + c.x, value.size(), value.begin());
                uint32_t at = 0;
                if (count > 1) {
                    at = uint32_t(viewArrays.size());
                    viewArrays.insert(viewArrays.end(), extra.begin() + c.x, extra.begin() + c.x + 6 * count);
                }
                difference = raw::assignState(state.viewport, value);
                difference |= raw::assignState(state.viewports, count);
                difference |= raw::assignState(state.viewportsAt, at);
                break;
            }
            case OpCull:
                bit = R::Cull;
                difference = raw::assignState(state.cull, c.a);
                break;
            case OpWinding:
                bit = R::Winding;
                difference = raw::assignState(state.winding, c.a);
                break;
            case OpFillMode:
                bit = R::Fill;
                difference = raw::assignState(state.fill, c.a);
                break;
            case OpDepthClip:
                bit = R::DepthClip;
                difference = raw::assignState(state.depthClip, c.a);
                break;
            case OpBlendColor: {
                bit = R::Blend;
                std::array<uint32_t, 4> value;
                for (size_t i = 0; i < value.size(); ++i)
                    value[i] = uint32_t(extra[c.x + i]);
                difference = raw::assignState(state.blend, value);
                break;
            }
            case OpStencilRef:
                bit = R::Stencil;
                difference = raw::assignState(state.stencil, std::array<uint32_t, 2>{c.a, c.b});
                break;
            case OpDepthBias: {
                bit = R::DepthBias;
                std::array<uint32_t, 3> value;
                for (size_t i = 0; i < value.size(); ++i)
                    value[i] = uint32_t(extra[c.x + i]);
                difference = raw::assignState(state.depthBias, value);
                break;
            }
            case OpVisibility:
                bit = R::Visibility;
                difference = raw::assignState(state.visibility, c.a);
                difference |= raw::assignState(state.visibilityOffset, c.c);
                break;
            case OpAmplification: {
                bit = R::Amplification;
                std::array<uint32_t, 17> value{c.a};
                for (size_t i = 0; i < kAmplificationExtraWords; ++i)
                    value[1 + i] = uint32_t(extra[c.x + i]);
                difference = raw::assignState(state.amplification, value);
                break;
            }
            default:
                break;
            }
            difference |= !(state.set & bit);
            state.set |= bit;
            return difference;
        });
        break;
    default:
        return false;
    }
    if (changed) {
        if (resourcesChanged)
            ++resourceRevision_[compute ? 1 : 0];
        ++(compute ? kernelGeneration_ : renderGeneration_);
        if (!compute && (c.op == OpPipeline || c.op == OpDepthStencil || c.op == OpBuffer || c.op == OpBufferOffset ||
                         c.op == OpBytes || c.op == OpTexture || c.op == OpSampler))
            ++eligibilityGeneration_;
    }
    return true;
}
