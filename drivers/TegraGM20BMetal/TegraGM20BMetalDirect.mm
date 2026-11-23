// SPDX-License-Identifier: MIT
// See TegraGM20BMetalDirect.h.
#import "TegraGM20BMetalDirect.h"

#include <algorithm>
#include <cstring>
#include <map>
#include "compute_key.h"
#include "direct_compute.h"
#include "direct_abi.h"
#include "pack.h"
#include "record_parts.h"

namespace gm20b {
namespace {

struct Asked {
    const StageCompile &compile;
    const PackRules &rules;
    const std::string &key;
    const std::vector<uint32_t> &attributes;
    RuntimeRecord &out;
    std::map<uint32_t, uint32_t> *registers;
    bool split;
    bool variants;
    BoundDraw *bound;
    uint16_t xrSlots;
    const SelectorConfigs *selectors;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
    bool splittable = false;
};

bool made(const NmbDirectState *state, void *ctx)
{
    Asked &a = *static_cast<Asked *>(ctx);
    a.splittable = splitsFragment(*state);
    a.outcome = directRecord(*state, a.compile, a.rules, a.key, a.attributes, a.out, a.why, a.registers, a.split,
                             a.variants, a.xrSlots, a.selectors);
    if (a.bound) {
        const NmbDirectDraw &d = state->draws[3];
        a.bound->pushMask = d.pushMask;
        memcpy(a.bound->push, d.push, sizeof a.bound->push);
        for (uint32_t k = 0; k < 2; k++)
            a.bound->spill[k].assign(d.spill[k], d.spill[k] + d.spillWords[k]);
    }
    return true;
}

struct AskedKernel {
    const StageCompile &compile;
    const std::string &key;
    ComputeRecord &out;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
};

bool madeKernel(const NmbDirectKernel *kernel, void *ctx)
{
    AskedKernel &a = *static_cast<AskedKernel *>(ctx);
    a.outcome = directKernel(*kernel, a.compile, a.key, a.out, a.why);
    return true;
}

}  // namespace

Direct::Direct(PackRules rules, bool rankArtifacts, DirectTranslator translator)
    : rules_(std::move(rules)), translator_(translator), compile_(StageCompile::linked(rankArtifacts))
{}

CompileOutcome Direct::form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                            const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                            std::string &why, std::map<uint32_t, uint32_t> *registers, bool split)
{
    const SelectorConfigs *selectors = nullptr;
    if (selectors_) {
        const auto found = selectors_->find(packKeyHash(key.substr(0, variantNameAt(key))));
        if (found != selectors_->end())
            selectors = &found->second;
    }
    Asked a = {
        compile_,         rules_,    key, attributes, out, registers, split, variants_, keepBound_ ? &bound_ : nullptr,
        textures.xrSlots, selectors, {}};
    if (keepBound_)
        bound_ = {};
    std::vector<NmbDirectCubicSampler> cubic;
    std::vector<NmbDirectTextureFormat> formats;
    for (const CubicBinds::Sampler &c : textures.cubic.samplers)
        cubic.push_back({c.slot, c.s, c.t, c.r, c.border, c.normalized, c.stage == kCubicVertex});
    for (const CubicBinds::Texture &t : textures.cubic.textures)
        formats.push_back({t.slot, t.format, t.stage == kCubicVertex});
    for (const WrittenTexture &t : textures.written)
        formats.push_back({t.slot, t.format, false});
    const NmbDirectBinds binds = {cubic.data(),           uint32_t(cubic.size()),
                                  formats.data(),         uint32_t(formats.size()),
                                  textures.fragmentTests, textures.bindStride};
    const bool plain = formats.empty() && cubic.empty() && !textures.fragmentTests && !textures.bindStride;
    const char *refused = nullptr;
    if (!translator_.render(descriptor, barrier, textures.format, textures.samples, plain ? nullptr : &binds, made, &a,
                            &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? CompileOutcome::Failed : CompileOutcome::Refused;
    }
    why = a.why;
    splittable_ = a.splittable;
    if (a.outcome == CompileOutcome::Made && !compiledFor(out.pipeline, textures.written)) {
        why = "a written texture's variant m2v compiled for another format than the one bound";
        return CompileOutcome::Refused;
    }
    return a.outcome;
}

CompileOutcome Direct::make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                            std::string &why, ProbeTextures textures, std::map<uint32_t, uint32_t> *registers,
                            bool split, PackForm requested)
{
    storageRefusal_.clear();
    splittable_ = false;
    if (!translator_.render || !StageCompile::abiMatches()) {
        why = translator_.render ? "the stage compile's structs are not direct_nvk.h's" : "no translator";
        return CompileOutcome::Failed;
    }
    // The function's reflection marks active an attribute a function constant gates (CoreRE's vsShadow texcoord, under
    // kEnableAlphaCutoutShadows), which a state that leaves the constant undefined or false does not read: m2v links
    // only what the specialised stage reads and refuses a state whose descriptor lacks one of those
    // (pipeline/link/vertex.rs), so an attribute the descriptor leaves unset is not one the stage reads
    MTLVertexDescriptor *vertexDescriptor = descriptor.vertexDescriptor;
    std::vector<uint32_t> attributes;
    for (MTLVertexAttribute *a in descriptor.vertexFunction.vertexAttributes)
        if (a.isActive &&
            (!vertexDescriptor || vertexDescriptor.attributes[a.attributeIndex].format != MTLVertexFormatInvalid))
            attributes.push_back(uint32_t(a.attributeIndex));
    const auto barrierForm = [&](bool twin) {
        const CompileOutcome made = form(descriptor, true, textures, key, attributes, out, why, registers, split);
        if (made != CompileOutcome::Made)
            return made;
        dropUnreadFetches(out);
        if (twin || (out.pipeline.flags & PipelineFetches))
            out.pipeline.flags |= PipelineBarrierForm;
        return strideTwin(descriptor, textures, key, attributes, out, why, split);
    };
    if (requested == PackForm::Barrier)
        return barrierForm(true);
    CompileOutcome outcome = form(descriptor, false, textures, key, attributes, out, why, registers, split);
    if (outcome == CompileOutcome::Made) {
        const bool storage = out.pipeline.flags & PipelineStorageMetadata;
        dropUnreadFetches(out);
        if (!storage || (out.pipeline.storageMask && loadsSurface(out)))
            return strideTwin(descriptor, textures, key, attributes, out, why, split);
        // its program holds the pixel interlock around no load of a colour: made again without the interlock
        storageRefusal_ = out.pipeline.storageMask ? "storage form: its program loads no colour"
                                                   : "storage form: its program reads and writes no colour";
        return barrierForm(false);
    }
    if (outcome != CompileOutcome::Refused || why.rfind("storage form", 0) != 0)
        return outcome;
    storageRefusal_ = why;
    return barrierForm(false);
}

// The probe binds every vertex buffer at a stride no fixed-function stream fetches, so m2v's plan asks for its
// pulled-vertex-buffer variant of every per-bind layout. A twin the translation refuses leaves the state without one:
// its draws of such a stride are refused (RenderEncoder::drawnWordsRefusal). The twin is made as its record was, and the
// bound draw kept is the record's.
CompileOutcome Direct::strideTwin(MTLRenderPipelineDescriptor *descriptor, ProbeTextures textures,
                                  const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                                  std::string &why, bool split)
{
    if (!bindStrideStreams(out.pipeline))
        return CompileOutcome::Made;
    textures.bindStride = kStreamStrideField + 1;
    auto twin = std::make_shared<RuntimeRecord>();
    std::string twinWhy;
    const bool barrier = (out.pipeline.flags & PipelineBarrierForm) || !storageRefusal_.empty();
    const BoundDraw bound = bound_;
    twinRegisters_.clear();
    const CompileOutcome outcome = form(descriptor, barrier, textures, key, attributes, *twin, twinWhy,
                                        keepBound_ ? &twinRegisters_ : nullptr, split);
    bound_ = bound;
    if (outcome == CompileOutcome::Failed) {
        why = "its stride twin: " + twinWhy;
        return outcome;
    }
    if (outcome != CompileOutcome::Made || bindStrideStreams(twin->pipeline))
        return CompileOutcome::Made;
    dropUnreadFetches(*twin);
    twin->pipeline.flags |= out.pipeline.flags & PipelineBarrierForm;
    twin->pipeline.keyHash = packKeyHash(strideTwinKey(key));
    out.pipeline.strideTwin = twin->pipeline.keyHash;
    out.strideTwin = std::move(twin);
    return CompileOutcome::Made;
}

}  // namespace gm20b

namespace gm20b {

KernelDispatch::KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode)
{
    memcpy(dispatch_.threads, threads, sizeof dispatch_.threads);
    dispatch_.regionMode = regionMode;
    for (const auto &[index, length] : memory)
        if (index < 31)
            dispatch_.memory[index] = length;
}

void KernelDispatch::texture(uint32_t slot, MTLTextureDescriptor *d, bool planes)
{
    textures_.push_back({slot, planes ? 1u : 0u, d.pixelFormat, d.textureType, d.usage, d.mipmapLevelCount,
                         d.arrayLength, d.sampleCount});
}

void KernelDispatch::sampler(uint32_t slot, MTLSamplerDescriptor *d)
{
    samplers_.push_back(
        {slot,
         {d.minFilter, d.magFilter, d.mipFilter, d.sAddressMode, d.tAddressMode, d.rAddressMode, d.borderColor,
          d.normalizedCoordinates, d.maxAnisotropy, d.compareFunction, d.lodAverage, d.lodMinClamp, d.lodMaxClamp}});
}

const NmbDirectDispatch &KernelDispatch::dispatch()
{
    dispatch_.textures = textures_.data();
    dispatch_.textureCount = uint32_t(textures_.size());
    dispatch_.samplers = samplers_.data();
    dispatch_.samplerCount = uint32_t(samplers_.size());
    return dispatch_;
}

// `textures` with the address map compute key `key`'s texture items state for each (compute_key.h: an item without
// ":am" states 0) in its `planes` (direct_abi.h NMB_DIRECT_BOUND_MAP_*); false, and none stated, where the key names
// no readable item at a bound texture's slot
static bool stateKeyMaps(const std::string &key, std::vector<NmbDirectBoundTexture> &textures)
{
    const size_t tex = key.rfind(" | tex=");
    if (tex == std::string::npos)
        return false;
    const size_t from = tex + 7, to = key.find(" | ", from);
    const std::string items = key.substr(from, to == std::string::npos ? std::string::npos : to - from);
    std::map<uint32_t, uint8_t> maps;
    for (size_t at = 0; at < items.size();) {
        const size_t comma = std::min(items.find(',', at), items.size());
        BoundTexture item;
        if (!parseComputeTexture(items.substr(at, comma - at), item))
            return false;
        maps[item.slot] = item.addressMap;
        at = comma + 1;
    }
    for (const NmbDirectBoundTexture &t : textures)
        if (!maps.count(t.slot))
            return false;
    for (NmbDirectBoundTexture &t : textures)
        t.planes |= NMB_DIRECT_BOUND_MAP_STATED | uint32_t(maps[t.slot]) << NMB_DIRECT_BOUND_MAP_SHIFT;
    return true;
}

CompileOutcome Direct::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                              const std::string &key, ComputeRecord &out, std::string &why)
{
    if (!translator_.compute || !StageCompile::abiMatches()) {
        why = translator_.compute ? "the stage compile's structs are not direct_nvk.h's" : "no translator of kernels";
        return CompileOutcome::Failed;
    }
    std::vector<NmbDirectBoundTexture> textures(dispatch.textures, dispatch.textures + dispatch.textureCount);
    const bool mapsStated = stateKeyMaps(key, textures);
    NmbDirectDispatch stated = dispatch;
    stated.textures = textures.data();
    AskedKernel a = {compile_, key, out, {}};
    const char *refused = nullptr;
    if (!translator_.compute(descriptor, mapsStated ? &stated : &dispatch, madeKernel, &a, &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        // "fails: ", a dispatch Metal runs that m2v fails too (NativeMetalDirectComputeBinds.rs mapped_samplers): its
        // command buffer fails, not a route to m2v
        const bool fails = refused && (!strncmp(refused, "retry: ", 7) || !strncmp(refused, "fails: ", 7));
        return fails ? CompileOutcome::Failed : CompileOutcome::Refused;
    }
    why = a.why;
    if (a.outcome == CompileOutcome::Made)
        out.kernel.mapsStated = mapsStated;
    return a.outcome;
}

}  // namespace gm20b
