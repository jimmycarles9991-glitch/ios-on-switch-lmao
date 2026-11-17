// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_compute.h"
#include "raw_compute_lookup.h"
#include "raw_kernel_shadow.h"
#include "raw_kernel_texture.h"
#include "raw_sampler_map.h"
#include "raw_view.h"

namespace {

// Which of a texture's terms refused it: compute.image's b, named in its first line
enum ImageTerm : uint8_t {
    TermTwoPlanesBare = 2,  // RGB10A8_2P_XR10 reached through a view neither its planes' nor its stand-in's
    TermConvertedSampler,  // a converted texture beside a sampler its address map would change (draws take its variant)
};

const char *imageTermName(uint64_t term)
{
    switch (term) {
    case TermTwoPlanesBare:
        return "an RGB10A8_2P_XR10 texture reached through a view other than its planes' or its stand-in's";
    case TermConvertedSampler:
        return "a converted texture beside a sampler that does not clamp to its edge";
    }
    return "?";
}

NSString *textureFacts(RMTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const raw::Texture &x = t->_raw;
    NSMutableString *s = [NSMutableString
        stringWithFormat:@"%lux%lu format %lu type %lu levels %lu array %lu usage 0x%lx storage %lu kind %d pitch %u "
                         @"va&31 %llu form %s",
                         (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.arrayLength,
                         (unsigned long)d.usage, (unsigned long)d.storageMode, int(t->_kind), x.pitch,
                         (unsigned long long)(x.va & 31), raw::kernelTextureFormName(raw::kernelTextureForm(x))];
    if (IOSurfaceRef surface = t->_surface) {
        const OSType f = IOSurfaceGetPixelFormat(surface);
        [s appendFormat:@" surface '%c%c%c%c' plane %lu of %zu rows %zu", char(f >> 24), char(f >> 16), char(f >> 8),
                        char(f), (unsigned long)t->_plane, IOSurfaceGetPlaneCount(surface),
                        IOSurfaceGetPlaneCount(surface) ? IOSurfaceGetBytesPerRowOfPlane(surface, t->_plane)
                                                        : IOSurfaceGetBytesPerRow(surface)];
    }
    return s;
}

// The process's first refusal of `pred` names the texture and the kernel (key) it refused at `slot`
void noteRefusal(RMCommandBuffer *cb, RMPred pred, uint32_t slot, uint64_t term, RMTexture *t, uint64_t key)
{
    if (!predLogged(pred))
        logPred(pred, slot, term,
                [NSString stringWithFormat:@"(%s; texture %@; kernel %016llx)",
                                           pred == PredComputeImage ? imageTermName(term) : "a view of another size",
                                           t ? textureFacts(t) : @"none", (unsigned long long)key]);
    hit(cb, pred, slot, term);
}

// Whether a kernel's samplers can take address map `map` of the converted texture at `slot`. A sampler entry that holds
// its pairs and samples that texture is compiled with its own modes, so only where the map leaves them (raw_sampler_map.h);
// the others where the map changes none of them, or where every texture the kernel binds is under that map.
template <typename Plan, typename State, typename Objects>
bool kernelSamplersMapped(Plan &plan, const State &state, const raw::PackKernel &record, const Objects &textures,
                          uint32_t slot, uint8_t map)
{
    bool allMapped = true, unchanged = true, constant = false, kept = true;
    const auto &samplers = plan.samplers[2][state.samplers].objects;
    for (uint32_t i = 0; i < record.entries; ++i) {
        const raw::PackKernelEntry &e = record.entry[i];
        RMSampler *s = e.kind == raw::KernelSampler && e.slot < 16 ? (__bridge RMSampler *)samplers[e.slot] : nil;
        const bool leaves =
            !s || raw::mapLeavesModes(s->_raw.desc.sAddress, s->_raw.desc.tAddress, s->_raw.desc.rAddress, map);
        if (e.kind == raw::KernelTexture && e.slot < 31 && textures[e.slot]) {
            RMTexture *t = (__bridge RMTexture *)textures[e.slot];
            allMapped = allMapped && t->_rawOk && t->_raw.addressMap == map;
        } else if (e.kind == raw::KernelSampler && raw::kernelSamplerPairsKnown(e)) {
            if (raw::kernelSamplerPairs(e) >> slot & 1)
                kept = kept && ((e.slot & 0x80) ? gRM->raw->constexprLeaves(e.slot & 0x7f, map) : leaves);
        } else if (e.kind == raw::KernelSampler) {
            constant = constant || (e.slot & 0x80);  // a constexpr sampler: its record's header, never mapped
            unchanged = unchanged && leaves;
        }
    }
    return kept && !constant && (unchanged || allMapped);
}

raw::KernelUse textureUse(RMTexture *t, bool writes)
{
    const raw::KernelTextureForm form = raw::kernelTextureForm(t->_raw);
    return {(__bridge const void *)t, raw::kernelTextureSpan(t->_raw), form == raw::KernelTextureForm::Shadow, writes};
}

raw::KernelUse bufferUse(RMBuffer *b, uint64_t offset, bool writes)
{
    const uint64_t length = b->_raw.length;
    return {(__bridge const void *)b,
            {b->_raw.va + std::min(offset, length), length - std::min(offset, length)},
            false,
            writes};
}

}  // namespace

void kernelUses(RMCommandBuffer *cb, size_t at, std::vector<raw::KernelUse> &uses)
{
    auto &plan = cb->_plan;
    const auto &dispatch = plan.dispatch(at);
    const auto *kernels = static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(dispatch.kernel);
    const auto &state = plan.kernelStates[dispatch.state];
    const auto &bindings = plan.buffers[2][state.buffers];
    const auto &textures = plan.kernelTextures[state.textures].objects;
    uses.clear();
    // a window slot's one window covers what every region's record reaches of it
    raw::KernelUse windows[31];
    for (uint8_t region = 0; kernels && region < kernels->count; ++region) {
        const auto &record = *kernels->jobs[region]->value()->rec;
        for (uint32_t i = 0; i < record.entries; ++i) {
            const auto &e = record.entry[i];
            const bool writes = raw::kernelEntryWrites(record, i);
            const bool buffer = e.kind == raw::KernelBuffer && e.slot < 31 &&
                                bindings[e.slot].source == raw::BufferBinding::Source::Buffer &&
                                bindings[e.slot].object;
            if (e.kind == raw::KernelTexture && e.slot < 31 && textures[e.slot]) {
                uses.push_back(textureUse((__bridge RMTexture *)textures[e.slot], writes));
            } else if (buffer && (dispatch.windowSlots >> e.slot & 1)) {
                const raw::KernelUse bound =
                    bufferUse((__bridge RMBuffer *)bindings[e.slot].object, bindings[e.slot].offset, writes);
                const raw::ByteSpan span =
                    raw::kernelBufferWindowSpan(bound.span.va, bound.span.bytes, raw::kernelBufferWindow(e));
                raw::KernelUse &w = windows[e.slot];
                w = {bound.object,
                     {span.va, std::max(w.span.bytes, span.bytes)},
                     true,
                     w.writes || writes,
                     int8_t(e.slot)};
            } else if (buffer) {
                uses.push_back(
                    bufferUse((__bridge RMBuffer *)bindings[e.slot].object, bindings[e.slot].offset, writes));
            }
        }
    }
    for (const raw::KernelUse &w : windows)
        if (w.windowSlot >= 0)
            uses.push_back(w);
    for (size_t use = dispatch.encoderBegin; use < at; ++use) {
        if (plan.operation(use).op != OpKernelUse)
            continue;
        const RMPlanControl &control = plan.control(use);
        id resource = (__bridge id)control.object;
        const bool writes = control.value & MTLResourceUsageWrite;
        if ([resource isKindOfClass:[RMTexture class]])
            uses.push_back(textureUse((RMTexture *)resource, writes));
        else if (RMBuffer *buffer = cpuBufferOfResource(resource))
            uses.push_back(bufferUse(buffer, 0, writes));
    }
}

bool admitComputeDispatch(RMCommandBuffer *cb, size_t at, const raw::ComputeKernelSet<raw::ComputeState> *kernels,
                          const char **why)
{
    auto &plan = cb->_plan;
    const auto &dispatch = plan.dispatch(at);
    const auto &state = plan.kernelStates[dispatch.state];
    const auto &bindings = plan.buffers[2][state.buffers];
    const auto &textures = plan.kernelTextures[state.textures].objects;
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
    uint32_t shadowSlots = 0, windowSlots = 0;
    for (uint8_t region = 0; region < kernels->count; ++region) {
        const auto *k = kernels->jobs[region]->value();
        for (uint32_t e = 0; e < k->rec->entries; e++) {
            const raw::PackKernelEntry &x = k->rec->entry[e];
            const bool texture = x.kind == raw::KernelTexture || x.kind == raw::KernelTexturePlane;
            RMTexture *t = texture && x.slot < 31 ? textureAt(x.slot) : nil;
            if (t && !t->_rawOk) {  // m2v's texture, refused as it was made: its own predicate's command buffer
                hit(cb, RMPred(t->_pred ?: PredTexShape), t->_descriptor.pixelFormat, t->_kind);
                if (why)
                    *why = "a kernel's texture the raw path does not keep";
                return false;
            }
            const bool writes = raw::kernelEntryWrites(*k->rec, e);
            const raw::KernelTextureForm form = t ? raw::kernelTextureForm(t->_raw) : raw::KernelTextureForm::InPlace;
            if (form == raw::KernelTextureForm::Shadow)
                shadowSlots |= 1u << x.slot;
            // a sampler entry that holds its pairs samples a converted texture with the modes its kernel was compiled with,
            // right where the texture's map leaves them; the others, not paired with a texture as a draw's handles are,
            // take its map where it changes none of them, or where every texture the kernel binds is under that one map
            if (form == raw::KernelTextureForm::Converted && t->_raw.addressMap &&
                !kernelSamplersMapped(plan, state, *k->rec, textures, x.slot, t->_raw.addressMap)) {
                noteRefusal(cb, PredComputeImage, x.slot, TermConvertedSampler, t, k->rec->keyHash);
                if (why)
                    *why = imageTermName(TermConvertedSampler);
                return false;
            }
            if (form == raw::KernelTextureForm::NoHeader && writes) {
                failCommandBuffer(cb,
                                  [NSString stringWithFormat:@"A kernel stores a texture with no memory a shader can "
                                                             @"address (memoryless, or a depth texture without "
                                                             @"MTLTextureUsageShaderRead): %@",
                                                             textureFacts(t)],
                                  MTLCommandBufferErrorInvalidResource);
                if (why)
                    *why = "a kernel stores a texture with no memory a shader can address";
                return false;
            }
            auto viewOf = [&](const raw::Texture &b) {
                const raw::FormatInfo *f = raw::formatInfo(b.format);
                return f ? raw::slotView(x.window, f->tic0, raw::swizzledWord0(f->tic0, b.swizzle), f->bpp,
                                         raw::blockCompressed(f))
                         : raw::SlotView::Own;
            };
            // a load of an extended-range texture binds its stand-in where the record's view is the stand-in's own,
            // else its codes (raw_compute.cpp)
            const bool standIn = t && !writes && x.kind == raw::KernelTexture && t->_raw.standIn &&
                                 viewOf(*t->_raw.standIn) == raw::SlotView::Own;
            const bool twoPlanes = form == raw::KernelTextureForm::TwoPlanes;
            // a load of a texture made without ShaderRead, which Metal's validation stops
            if (twoPlanes && !writes && !(t->_descriptor.usage & MTLTextureUsageShaderRead)) {
                NSString *reason =
                    [NSString stringWithFormat:@"A kernel loads an RGB10A8_2P_XR10 texture made without ShaderRead: %@",
                                               textureFacts(t)];
                os_log_error(rawLog(), "raw: %{public}s", reason.UTF8String);
                failCommandBuffer(cb, reason, MTLCommandBufferErrorInvalidResource);
                if (why)
                    *why = "a kernel loads an RGB10A8_2P_XR10 texture made without ShaderRead";
                return false;
            }
            // a two-plane texture is reached through its planes (raw_compute.cpp): its color words at the slot's entry,
            // its alpha bytes at its plane entry, each through a view of its own texel size
            const raw::Texture *plane = !twoPlanes                          ? nullptr
                                        : x.kind == raw::KernelTexturePlane ? t->_raw.chroma
                                                                            : t->_raw.luma;
            if (twoPlanes && !standIn && (!plane || viewOf(*plane) == raw::SlotView::None)) {
                const uint64_t term = TermTwoPlanesBare;
                noteRefusal(cb, PredComputeImage, x.slot, term, t, k->rec->keyHash);
                if (why)
                    *why = imageTermName(term);
                return false;
            }
            if (standIn) {  // drawn from its codes as the encoder begins (encodeRaw's standIns)
                cb->_standIns = true;
                if (t->_raw.xrPlanes)
                    cb->_xrStandIns.insert(t);
            }
            const raw::SlotView view = !t || !t->_raw.tic || standIn ? raw::SlotView::Own
                                       : twoPlanes                   ? viewOf(*plane)
                                                                     : viewOf(t->_raw);
            // a record that views a slot in texels of another size is a pack or compiler defect (neither pack holds
            // one): the command buffer fails, naming it
            if (view == raw::SlotView::None) {
                NSString *reason =
                    [NSString stringWithFormat:@"Kernel %016llx's record views texture slot %u in "
                                               @"texels of another size (header word 0 %08x): %@",
                                               (unsigned long long)k->rec->keyHash, x.slot, x.window, textureFacts(t)];
                os_log_error(rawLog(), "raw: %{public}s", reason.UTF8String);
                failCommandBuffer(cb, reason, MTLCommandBufferErrorInternal);
                if (why)
                    *why = "a kernel record that views a texture slot in texels of another size";
                return false;
            }
            // m2v's descriptor starts at the bind offset rounded down to 16, the rest in a push word the record names;
            // a record that names none reaches the bytes through a window on 16 (raw_kernel_shadow.h)
            if (x.kind == raw::KernelBuffer && raw::kernelResidualWord(x) < 0 && x.slot < 31 && bufferAt(x.slot) &&
                ((bufferAt(x.slot)->_raw.va + bindings[x.slot].offset) & 15))
                windowSlots |= 1u << x.slot;
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
                    exact.push_back({plan.kernelTextures[state.textures].objects[entry.slot],
                                     uint8_t(access | raw::ResourceUse::Sample)});
                } else if (entry.kind == raw::KernelSampler && entry.slot < 16) {
                    exact.push_back({plan.samplers[2][state.samplers].objects[entry.slot], raw::ResourceUse::Read});
                }
            }
        }
    });
    if (dispatch.arguments || dispatch.stageInBuffer)
        plan.addUses(at,
                     {{dispatch.arguments, raw::ResourceUse::Read}, {dispatch.stageInBuffer, raw::ResourceUse::Read}});
    plan.resolveDispatch(at, kernels, shadowSlots, windowSlots);
    return true;
}

int computeReadiness(RMCommandBuffer *cb, const RMSegment &range, const char **why)
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
        if (state == Job::State::Refused) {
            hit(cb, PredComputeKernel, at);
            *why = "a compute kernel the shader compiler refused";
            return -1;
        }
        if (state == Job::State::Pending)
            pending = true;
    }
    if (pending)
        return 0;
    bool accepted = true;
    for (size_t at : dispatches) {
        auto *job = static_cast<Job *>(cb->_plan.dispatch(at).pending);
        if (!admitComputeDispatch(cb, at, job, why)) {
            accepted = false;
            break;
        }
    }
    cb->_plan.invalidateUses();
    cb->_plan.prepareRange(range.from, range.to);
    return accepted ? 1 : -1;
}
