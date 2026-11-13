// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_compute.h"
#include "raw_compute_lookup.h"
#include "raw_kernel_texture.h"
#include "raw_surface_view.h"
#include "raw_view.h"

// A texture a raw kernel samples as it is (raw_kernel_texture.h), and not one held in two planes: its header is a pair
static bool plainTexture(RMTexture *t)
{
    return t->_rawOk && raw::kernelSamplesInPlace(t->_raw) && !t->_raw.xrPlanes;
}

bool admitComputeDispatch(RMCommandBuffer *cb, size_t at, const raw::ComputeKernelSet<raw::ComputeState> *kernels,
                          const char **why)
{
    auto &plan = cb->_plan;
    const auto &dispatch = plan.dispatch(at);
    const auto &state = plan.kernelStates[dispatch.state];
    const auto &bindings = plan.buffers[2][state.buffers];
    const auto &textures = plan.textures[2][state.textures].objects;
    auto textureAt = [&](uint32_t slot) -> RMTexture * {
        return (__bridge RMTexture *)textures[slot];
    };
    auto bufferAt = [&](uint32_t slot) -> RMBuffer * {
        return bindings[slot].source == raw::BufferBinding::Source::Buffer ? (__bridge RMBuffer *)bindings[slot].object
                                                                           : nil;
    };
    auto refuse = [&](RMPred pred, uint32_t slot, const char *message) {
        hit(cb, pred, slot);
        if (why)
            *why = message;
        return false;
    };
    if (!kernels || kernels->state() != raw::ComputeKernelSet<raw::ComputeState>::State::Ready)
        return refuse(PredComputeKernel, 0, "a dispatch the raw path has no kernel for");
    raw::ComputeResourceSlots used;
    for (uint8_t region = 0; region < kernels->count; ++region)
        used.add(*kernels->jobs[region]->value()->rec);
    uint32_t surfaceSlots = 0;
    for (uint8_t region = 0; region < kernels->count; ++region) {
        const auto *k = kernels->jobs[region]->value();
        for (uint32_t e = 0; e < k->rec->entries; e++) {
            const raw::PackKernelEntry &x = k->rec->entry[e];
            RMTexture *shadow = x.kind == raw::KernelTexture && x.slot < 31 ? textureAt(x.slot) : nil;
            if (shadow && shadow->_raw.surfaceView) {
                surfaceSlots |= 1u << x.slot;
                bool aliases = false;
                const auto *backing = shadow->_raw.import;
                const auto shares = [&](const raw::Import *other) {
                    return other && raw::surfaceBackingAliases(backing->id, backing->base, backing->bytes, other->id,
                                                               other->base, other->bytes);
                };
                aliases = used.aliases(
                    [&](uint32_t slot) {
                        RMTexture *texture = textureAt(slot);
                        return texture && texture != shadow && shares(texture->_raw.import);
                    },
                    [&](uint32_t slot) {
                        RMBuffer *buffer = bufferAt(slot);
                        return buffer && shares(buffer->_raw.import);
                    });
                for (size_t use = dispatch.encoderBegin; use < at; ++use) {
                    if (plan.operation(use).op != OpKernelUse)
                        continue;
                    id resource = (__bridge id)plan.control(use).object;
                    if ([resource isKindOfClass:[RMTexture class]])
                        aliases |= shares(((RMTexture *)resource)->_raw.import);
                    else if (RMBuffer *buffer = cpuBufferOfResource(resource))
                        aliases |= shares(buffer->_raw.import);
                }
                if (aliases) {
                    return refuse(PredComputeImage, x.slot, "a cropped surface view aliases another dispatch resource");
                }
            }
            if (x.kind == raw::KernelTexture && x.slot < 31 && textureAt(x.slot) && !plainTexture(textureAt(x.slot))) {
                return refuse(PredComputeImage, x.slot,
                              "a kernel's texture the raw path samples through an image of its own");
            }
            RMTexture *viewed = x.kind == raw::KernelTexture && x.slot < 31 ? textureAt(x.slot) : nil;
            const raw::FormatInfo *f = viewed ? raw::formatInfo(viewed->_raw.format) : nullptr;
            const raw::SlotView view =
                f ? raw::slotView(x.window, f->tic0, raw::swizzledWord0(f->tic0, viewed->_raw.swizzle), f->bpp,
                                  raw::blockCompressed(f))
                  : raw::SlotView::Own;
            bool viewRefused = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            viewRefused = view == raw::SlotView::View && rmHalfViewRefused();
#endif
            if (view == raw::SlotView::None || viewRefused) {
                return refuse(PredComputeView, x.slot,
                              "a kernel m2v binds a view of another texel size at a texture slot");
            }
            // m2v's descriptor starts at the bind offset rounded down to 16, the rest in a push word the record names
            if (x.kind == raw::KernelBuffer && raw::kernelResidualWord(x) < 0 && x.slot < 31 && bufferAt(x.slot) &&
                ((bufferAt(x.slot)->_raw.va + bindings[x.slot].offset) & 15)) {
                return refuse(PredComputeOff16, x.slot,
                              "a kernel's buffer bound at an offset that is not a multiple of 16");
            }
        }
    }
    plan.setKernelUses(at, kernels, [&](std::vector<raw::ResourceUse> &exact) {
        const auto &state = plan.kernelStates[plan.dispatch(at).state];
        for (uint8_t region = 0; region < kernels->count; ++region) {
            const auto &record = *kernels->jobs[region]->value()->rec;
            for (uint32_t i = 0; i < record.entries; ++i) {
                const auto &entry = record.entry[i];
                const uint8_t access =
                    raw::ResourceUse::Read | (raw::kernelEntryWrites(record, i) ? raw::ResourceUse::Write : 0);
                if (entry.kind == raw::KernelBuffer && entry.slot < 31) {
                    const auto &binding = plan.buffers[2][state.buffers][entry.slot];
                    if (binding.source == raw::BufferBinding::Source::Buffer)
                        exact.push_back({binding.object, access});
                } else if (entry.kind == raw::KernelTexture && entry.slot < 31) {
                    exact.push_back({plan.textures[2][state.textures].objects[entry.slot],
                                     uint8_t(access | raw::ResourceUse::Sample)});
                } else if (entry.kind == raw::KernelSampler && entry.slot < 16) {
                    exact.push_back({plan.samplers[2][state.samplers].objects[entry.slot], raw::ResourceUse::Read});
                }
            }
        }
    });
    plan.resolveDispatch(at, kernels, surfaceSlots);
    return true;
}

int computeReadiness(RMCommandBuffer *cb, const RMSegment &range)
{
    using Job = raw::ComputeKernelSet<raw::ComputeState>;
    const auto &dispatches = cb->_plan.pendingComputes(range.from, range.to);
    if (dispatches.empty())
        return 1;
    bool pending = false;
    for (size_t at : dispatches) {
        auto *job = static_cast<Job *>(cb->_plan.dispatch(at).pending);
        std::function<void()> observer;
        if (cb->_variantObserved.insert(job).second) {
            void *queue = (__bridge void *)cb->_queue;
            observer = [queue] {
                wakeSubmit(queue);
            };
        }
        const auto state = job->observe(std::move(observer));
        if (state == Job::State::Failed)
            return -2;
        if (state == Job::State::Refused)
            return -1;
        if (state == Job::State::Pending)
            pending = true;
    }
    if (pending)
        return 0;
    bool accepted = true;
    for (size_t at : dispatches) {
        auto *job = static_cast<Job *>(cb->_plan.dispatch(at).pending);
        const char *why = nullptr;
        if (!admitComputeDispatch(cb, at, job, &why)) {
            accepted = false;
            break;
        }
    }
    cb->_plan.invalidateUses();
    cb->_plan.prepareRange(range.from, range.to);
    return accepted ? 1 : -1;
}
