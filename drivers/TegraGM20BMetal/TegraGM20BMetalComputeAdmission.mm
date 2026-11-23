// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalIndirectCompute.h"
#include "compute.h"
#include "compute_lookup.h"
#include "kernel_shadow.h"
#include "kernel_texture.h"
#include "sampler_map.h"
#include "texel_rows.h"
#include "view.h"
#include "xr_pools.h"

namespace {

// Which of a texture's terms refused it: compute.image's b, named in its first line
enum ImageTerm : uint8_t {
    // RGB10A8_2P_XR10 through a view neither its planes' nor its stand-in's: its kernel is keyed by which it reads
    // (heldInPlanes), so a record that reads neither breaks what the key promises
    TermTwoPlanesBare = 2,
    // a converted texture beside a sampler its address map changes, in a record not made under the maps its key states
    // (PackKernel::mapsStated 0: its headers were made under its texture formats' maps, not Texture::addressMap)
    TermConvertedSampler,
};

const char *imageTermName(uint64_t term)
{
    switch (term) {
    case TermTwoPlanesBare:
        return "a kernel record that reads an RGB10A8_2P_XR10 texture through neither its planes nor its stand-in";
    case TermConvertedSampler:
        return "a converted texture's sampler whose modes its address map changes, in a kernel compiled without "
               "the map";
    }
    return "?";
}

NSString *textureFacts(SwitchMetalTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const gm20b::Texture &x = t->_raw;
    NSMutableString *s = [NSMutableString
        stringWithFormat:@"%lux%lu format %lu type %lu levels %lu array %lu usage 0x%lx storage %lu kind %d pitch %u "
                         @"va&31 %llu form %s",
                         (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.arrayLength,
                         (unsigned long)d.usage, (unsigned long)d.storageMode, int(t->_kind), x.pitch,
                         (unsigned long long)(x.va & 31), gm20b::kernelTextureFormName(gm20b::kernelTextureForm(x))];
    if (IOSurfaceRef surface = t->_surface) {
        const OSType f = IOSurfaceGetPixelFormat(surface);
        [s appendFormat:@" surface '%c%c%c%c' plane %lu of %zu rows %zu", char(f >> 24), char(f >> 16), char(f >> 8),
                        char(f), (unsigned long)t->_plane, IOSurfaceGetPlaneCount(surface),
                        IOSurfaceGetPlaneCount(surface) ? IOSurfaceGetBytesPerRowOfPlane(surface, t->_plane)
                                                        : IOSurfaceGetBytesPerRow(surface)];
    }
    return s;
}

// A kernel's texture the dispatch cannot bind as its record reads it fails the command buffer with the driver's error;
// the process's first names the texture and the kernel (key) at `slot`
void imageRefused(SwitchMetalCommandBuffer *cb, uint32_t slot, uint64_t term, SwitchMetalTexture *t, uint64_t key)
{
    if (!predLogged(PredComputeImage))
        logPred(PredComputeImage, slot, term,
                [NSString stringWithFormat:@"(%s; texture %@; kernel %016llx)", imageTermName(term), textureFacts(t),
                                           (unsigned long long)key]);
    gm20bComputeFails(cb, gm20b::ComputeFailure::Internal, PredComputeImage, slot, imageTermName(term));
}

// Whether a kernel's samplers can take address map `map` of the converted texture at `slot`. A sampler entry that holds
// its pairs and samples that texture is compiled with its own modes, so only where the map leaves them (sampler_map.h);
// the others where the map changes none of them, or where every texture the kernel binds is under that map.
template <typename Plan, typename State, typename Objects>
bool kernelSamplersMapped(Plan &plan, const State &state, const gm20b::PackKernel &record, const Objects &textures,
                          uint32_t slot, uint8_t map)
{
    bool allMapped = true, unchanged = true, constant = false, kept = true;
    const auto &samplers = plan.samplers[2][state.samplers].objects;
    for (uint32_t i = 0; i < record.entries; ++i) {
        const gm20b::PackKernelEntry &e = record.entry[i];
        SwitchMetalSampler *s =
            e.kind == gm20b::KernelSampler && e.slot < 16 ? (__bridge SwitchMetalSampler *)samplers[e.slot] : nil;
        const bool leaves =
            !s || gm20b::mapLeavesModes(s->_raw.desc.sAddress, s->_raw.desc.tAddress, s->_raw.desc.rAddress, map);
        if (e.kind == gm20b::KernelTexture && e.slot < 31 && textures[e.slot]) {
            SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)textures[e.slot];
            allMapped = allMapped && t->_gm20bOk && t->_raw.addressMap == map;
        } else if (e.kind == gm20b::KernelSampler && gm20b::kernelSamplerPairsKnown(e)) {
            if (gm20b::kernelSamplerPairs(e) >> slot & 1)
                kept = kept && ((e.slot & 0x80) ? gRM->raw->constexprLeaves(e.slot & 0x7f, map) : leaves);
        } else if (e.kind == gm20b::KernelSampler) {
            constant = constant || (e.slot & 0x80);  // a constexpr sampler: its record's header, never mapped
            unchanged = unchanged && leaves;
        }
    }
    return kept && !constant && (unchanged || allMapped);
}

gm20b::KernelUse textureUse(SwitchMetalTexture *t, bool writes)
{
    const bool derived = !writes && t->_raw.standIn;
    if (gm20b::xrPlanesPooled(t->_raw))  // its kernels read and store its planes' pools (xr_pools.h)
        return {(__bridge const void *)t, gm20b::kernelTextureSpan(*t->_raw.luma), true, writes, derived};
    if (const gm20b::TexelRows *rows = t->_raw.texelRows)  // its texels lie in its surface's image (texel_rows.h)
        return {(__bridge const void *)t, {rows->import->bo.va, rows->import->layout.bytes}, true, writes, derived};
    const gm20b::KernelTextureForm form = gm20b::kernelTextureForm(t->_raw);
    return {(__bridge const void *)t, gm20b::kernelTextureSpan(t->_raw), form == gm20b::KernelTextureForm::Shadow,
            writes, derived};
}

gm20b::KernelUse bufferUse(SwitchMetalBuffer *b, uint64_t offset, bool writes)
{
    const uint64_t length = b->_raw.length;
    return {(__bridge const void *)b,
            {b->_raw.va + std::min(offset, length), length - std::min(offset, length)},
            false,
            writes};
}

}  // namespace

void kernelUses(SwitchMetalCommandBuffer *cb, size_t at, std::vector<gm20b::KernelUse> &uses)
{
    auto &plan = cb->_plan;
    const auto &dispatch = plan.dispatch(at);
    const auto *kernels = static_cast<const gm20b::ComputeKernelSet<gm20b::ComputeState> *>(dispatch.kernel);
    const auto &state = plan.kernelStates[dispatch.state];
    const auto &bindings = plan.buffers[2][state.buffers];
    const auto &textures = plan.kernelTextures[state.textures].objects;
    uses.clear();
    for (uint8_t region = 0; kernels && region < kernels->count; ++region) {
        const auto &record = *kernels->jobs[region]->value()->rec;
        for (uint32_t i = 0; i < record.entries; ++i) {
            const auto &e = record.entry[i];
            const bool writes = gm20b::kernelEntryWrites(record, i);
            if (e.kind == gm20b::KernelTexture && e.slot < 31 && textures[e.slot])
                uses.push_back(textureUse((__bridge SwitchMetalTexture *)textures[e.slot], writes));
            else if (e.kind == gm20b::KernelBuffer && e.slot < 31 &&
                     bindings[e.slot].source == gm20b::BufferBinding::Source::Buffer && bindings[e.slot].object)
                uses.push_back(
                    bufferUse((__bridge SwitchMetalBuffer *)bindings[e.slot].object, bindings[e.slot].offset, writes));
        }
    }
    for (size_t use = dispatch.encoderBegin; use < at; ++use) {
        if (plan.operation(use).op != OpKernelUse)
            continue;
        const SwitchMetalPlanControl &control = plan.control(use);
        id resource = (__bridge id)control.object;
        const bool writes = control.value & MTLResourceUsageWrite;
        if ([resource isKindOfClass:[SwitchMetalTexture class]])
            uses.push_back(textureUse((SwitchMetalTexture *)resource, writes));
        else if (SwitchMetalBuffer *buffer = cpuBufferOfResource(resource))
            uses.push_back(bufferUse(buffer, 0, writes));
    }
}

bool admitComputeDispatch(SwitchMetalCommandBuffer *cb, size_t at,
                          const gm20b::ComputeKernelSet<gm20b::ComputeState> *kernels, const char **why)
{
    auto &plan = cb->_plan;
    const auto &dispatch = plan.dispatch(at);
    const auto &state = plan.kernelStates[dispatch.state];
    const auto &textures = plan.kernelTextures[state.textures].objects;
    auto textureAt = [&](uint32_t slot) -> SwitchMetalTexture * {
        return (__bridge SwitchMetalTexture *)textures[slot];
    };
    // each refusal has failed the command buffer first, at the recording and at readiness alike
    auto refuse = [&](const char *message) {
        if (why)
            *why = message;
        return false;
    };
    // the recording admits a dispatch whose kernels are made, and readiness one whose kernels it found made
    if (!kernels || kernels->state() != gm20b::ComputeKernelSet<gm20b::ComputeState>::State::Ready) {
        constexpr const char *unmade = "a dispatch admitted before its kernels were made: the driver's error";
        gm20bComputeFails(cb, gm20b::ComputeFailure::Internal, PredComputeKernel, 0, unmade);
        return refuse(unmade);
    }
    uint32_t shadowSlots = 0;
    for (uint8_t region = 0; region < kernels->count; ++region) {
        const auto *k = kernels->jobs[region]->value();
        for (uint32_t e = 0; e < k->rec->entries; e++) {
            const gm20b::PackKernelEntry &x = k->rec->entry[e];
            const bool texture = x.kind == gm20b::KernelTexture || x.kind == gm20b::KernelTexturePlane;
            SwitchMetalTexture *t = texture && x.slot < 31 ? textureAt(x.slot) : nil;
            const bool writes = gm20b::kernelEntryWrites(*k->rec, e);
            const gm20b::KernelTextureForm form =
                t ? gm20b::kernelTextureForm(t->_raw) : gm20b::KernelTextureForm::InPlace;
            if (form == gm20b::KernelTextureForm::Shadow)
                shadowSlots |= 1u << x.slot;
            // a sampler entry that holds its pairs samples a converted texture with the modes its kernel was compiled with,
            // right where the texture's map leaves them; the others, not paired with a texture as a draw's handles are,
            // take its map where it changes none of them, or where every texture the kernel binds is under that one map.
            // A record made under the maps its key states (mapsStated: the key holds each texture's own) is right as
            // made.
            if (form == gm20b::KernelTextureForm::Converted && t->_raw.addressMap && !k->rec->mapsStated &&
                !kernelSamplersMapped(plan, state, *k->rec, textures, x.slot, t->_raw.addressMap)) {
                imageRefused(cb, x.slot, TermConvertedSampler, t, k->rec->keyHash);
                return refuse(imageTermName(TermConvertedSampler));
            }
            if (form == gm20b::KernelTextureForm::NoHeader && writes) {
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
            auto viewOf = [&](const gm20b::Texture &b) {
                const gm20b::FormatInfo *f = gm20b::formatInfo(b.format);
                return f ? gm20b::slotView(x.window, f->tic0, gm20b::swizzledWord0(f->tic0, b.swizzle), f->bpp,
                                           gm20b::blockCompressed(f))
                         : gm20b::SlotView::Own;
            };
            // a load of an extended-range texture binds its stand-in where the record's view is the stand-in's own,
            // else its codes (compute.cpp)
            const bool standIn = t && !writes && x.kind == gm20b::KernelTexture && t->_raw.standIn &&
                                 viewOf(*t->_raw.standIn) == gm20b::SlotView::Own;
            const bool twoPlanes = form == gm20b::KernelTextureForm::TwoPlanes;
            // a load of a texture made without ShaderRead, which Metal's validation stops
            if (twoPlanes && !writes && !(t->_descriptor.usage & MTLTextureUsageShaderRead)) {
                NSString *reason =
                    [NSString stringWithFormat:@"A kernel loads an RGB10A8_2P_XR10 texture made without ShaderRead: %@",
                                               textureFacts(t)];
                failCommandBuffer(cb, reason, MTLCommandBufferErrorInvalidResource);
                if (why)
                    *why = "a kernel loads an RGB10A8_2P_XR10 texture made without ShaderRead";
                return false;
            }
            // a two-plane texture is reached through its planes (compute.cpp): its color words at the slot's entry,
            // its alpha bytes at its plane entry, each through a view of its own texel size
            const gm20b::Texture *plane = !twoPlanes                            ? nullptr
                                          : x.kind == gm20b::KernelTexturePlane ? t->_raw.chroma
                                                                                : t->_raw.luma;
            if (twoPlanes && !standIn && (!plane || viewOf(*plane) == gm20b::SlotView::None)) {
                imageRefused(cb, x.slot, TermTwoPlanesBare, t, k->rec->keyHash);
                return refuse(imageTermName(TermTwoPlanesBare));
            }
            // drawn from its codes as the encoder begins (encodeRaw's standIns), and again before a dispatch of it
            // that a write since reached (kernel_shadow.h KernelUse::derived)
            if (standIn) {
                shadowSlots |= 1u << x.slot;
                cb->_standIns = true;
                if (t->_raw.xrPlanes)
                    cb->_xrStandIns.insert(t);
            }
            const gm20b::SlotView view = !t || !t->_raw.tic || standIn ? gm20b::SlotView::Own
                                         : twoPlanes                   ? viewOf(*plane)
                                                                       : viewOf(t->_raw);
            // a record that views a slot in texels of another size is a pack or compiler defect (neither pack holds
            // one): the command buffer fails, naming it
            if (view == gm20b::SlotView::None) {
                NSString *reason =
                    [NSString stringWithFormat:@"Kernel %016llx's record views texture slot %u in "
                                               @"texels of another size (header word 0 %08x): %@",
                                               (unsigned long long)k->rec->keyHash, x.slot, x.window, textureFacts(t)];
                failCommandBuffer(cb, reason, MTLCommandBufferErrorInternal);
                if (why)
                    *why = "a kernel record that views a texture slot in texels of another size";
                return false;
            }
        }
    }
    plan.setKernelUses(at, kernels, [&](std::vector<gm20b::ResourceUse> &exact) {
        const auto &state = plan.kernelStates[plan.dispatch(at).state];
        for (uint8_t region = 0; region < kernels->count; ++region) {
            const auto &record = *kernels->jobs[region]->value()->rec;
            for (uint32_t i = 0; i < record.entries; ++i) {
                const auto &entry = record.entry[i];
                const uint8_t access =
                    gm20b::ResourceUse::Read | (gm20b::kernelEntryWrites(record, i) ? gm20b::ResourceUse::Write : 0);
                if (entry.kind == gm20b::KernelBuffer && entry.slot < 31) {
                    const auto &binding = plan.buffers[2][state.buffers][entry.slot];
                    if (binding.source == gm20b::BufferBinding::Source::Buffer)
                        exact.push_back({binding.object, access});
                } else if (entry.kind == gm20b::KernelTexture && entry.slot < 31) {
                    exact.push_back({plan.kernelTextures[state.textures].objects[entry.slot],
                                     uint8_t(access | gm20b::ResourceUse::Sample)});
                } else if (entry.kind == gm20b::KernelSampler && entry.slot < 16) {
                    exact.push_back({plan.samplers[2][state.samplers].objects[entry.slot], gm20b::ResourceUse::Read});
                }
            }
        }
    });
    if (dispatch.arguments || dispatch.stageInBuffer)
        plan.addUses(
            at, {{dispatch.arguments, gm20b::ResourceUse::Read}, {dispatch.stageInBuffer, gm20b::ResourceUse::Read}});
    plan.resolveDispatch(at, kernels, shadowSlots);
    return true;
}

int computeReadiness(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range, const char **why)
{
    using Job = gm20b::ComputeKernelSet<gm20b::ComputeState>;
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
            nameFailure(cb, PredComputeKernel);
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

// AGX runs none of a compute unit's dispatches once a predication Start or an EndDoWhile in it names no buffer, and
// completes the command buffer (an M4 Max: c38-audit/m2v-compute/probe): the dispatches before it too, and those of
// the unit's other compute encoders. A blit, a pass, an event signal or wait, or another dispatch type between
// encoders ends the unit, as gm20b::CommandBuffer::endPredicationUnit ends it.
bool gm20bComputeUnitWithheld(SwitchMetalCommandBuffer *cb, size_t at, bool serial)
{
    const SwitchMetalCommandPlan &plan = cb->_plan;
    const auto ends = [serial](const Cmd &c) {
        return c.op == OpBlit || c.op == OpRender || c.op == OpSignalEvent || c.op == OpWaitEvent ||
               (c.op == OpCompute && (c.a == MTLDispatchTypeSerial) != serial);
    };
    const auto withholds = [](const Cmd &c) {
        return c.op == OpPredicate && !c.obj &&
               (c.stage == gm20b::PredicateRecords::StartIf || c.stage == gm20b::PredicateRecords::StartWhile ||
                c.stage == gm20b::PredicateRecords::EndDoWhile);
    };
    for (size_t k = at; k < plan.size() && !ends(plan.legacy(k)); ++k)
        if (withholds(plan.legacy(k)))
            return true;
    // back from `at` an encoder's commands come before the OpCompute that says whether they are of the unit: each run
    // back to an OpCompute counts only where that OpCompute is of the unit's dispatch type, other work stopping the scan
    for (size_t k = at; k > 0;) {
        size_t open = k;
        bool withheld = false;
        while (open > 0 && plan.legacy(open - 1).op != OpCompute && !ends(plan.legacy(open - 1)))
            withheld = withholds(plan.legacy(--open)) || withheld;
        if (open == 0 || ends(plan.legacy(open - 1)))
            return false;
        if (withheld)
            return true;
        k = open - 1;
    }
    return false;
}
