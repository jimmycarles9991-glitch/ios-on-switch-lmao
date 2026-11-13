// SPDX-License-Identifier: MIT
// See RawDirect.h.
#import "RawDirect.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "raw_direct_compute.h"
#include "bridge/NativeMetalDirect.h"
#include "raw_pack.h"
#include "raw_record_parts.h"

// Weak, so the host benchmark links without the frontend
extern "C" bool native_bridge_metal_render_direct(id descriptor, bool barrier, NSUInteger textureFormat,
                                                  NSUInteger textureSamples, const struct NmbDirectBinds *binds,
                                                  bool (*each)(const struct NmbDirectState *, void *), void *ctx,
                                                  const char **why) __attribute__((weak_import));
extern "C" bool native_bridge_metal_compute_direct(id descriptor, const struct NmbDirectDispatch *dispatch,
                                                   bool (*each)(const struct NmbDirectKernel *, void *), void *ctx,
                                                   const char **why) __attribute__((weak_import));
extern "C" void *native_bridge_metal_nvk_symbol(const char *name) __attribute__((weak_import));

namespace raw {
namespace {

struct Asked {
    const DirectNvk &nvk;
    const PackRules &rules;
    const std::string &key;
    const std::vector<uint32_t> &attributes;
    RuntimeRecord &out;
    std::map<uint32_t, uint32_t> *registers;
    bool split;
    bool variants;
    BoundDraw *bound;
    uint16_t xrSlots;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
};

bool made(const NmbDirectState *state, void *ctx)
{
    Asked &a = *static_cast<Asked *>(ctx);
    a.outcome = directRecord(*state, a.nvk, a.rules, a.key, a.attributes, a.out, a.why, a.registers, a.split,
                             a.variants, a.xrSlots);
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
    const DirectNvk &nvk;
    const std::string &key;
    ComputeRecord &out;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
};

bool madeKernel(const NmbDirectKernel *kernel, void *ctx)
{
    AskedKernel &a = *static_cast<AskedKernel *>(ctx);
    a.outcome = directKernel(*kernel, a.nvk, a.key, a.out, a.why);
    return true;
}

}  // namespace

DirectTranslator DirectTranslator::frontend()
{
    DirectTranslator t;
    t.render = native_bridge_metal_render_direct;
    t.compute = native_bridge_metal_compute_direct;
    t.nvkSymbol = native_bridge_metal_nvk_symbol;
    return t;
}

Direct::Direct(const Pack &pack) : Direct(PackRules::of(pack))
{}

Direct::Direct(PackRules rules, bool rankArtifacts, DirectTranslator translator)
    : rules_(std::move(rules)), translator_(translator),
      nvk_(translator.nvkSymbol ? DirectNvk::find(translator.nvkSymbol) : DirectNvk{})
{
    if (rankArtifacts)
        nvk_.nakCompileStage = translator.nvkSymbol ? translator.nvkSymbol("switch_nak_compile_ranked_stage") : nullptr;
}

CompileOutcome Direct::form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                            const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                            std::string &why, std::map<uint32_t, uint32_t> *registers, bool split)
{
    Asked a = {nvk_,
               rules_,
               key,
               attributes,
               out,
               registers,
               split,
               variants_,
               keepBound_ ? &bound_ : nullptr,
               textures.xrSlots,
               {}};
    if (keepBound_)
        bound_ = {};
    std::vector<NmbDirectCubicSampler> cubic;
    std::vector<NmbDirectTextureFormat> formats;
    for (const CubicBinds::Sampler &c : textures.cubic.samplers)
        cubic.push_back({c.slot, c.s, c.t, c.r, c.border, c.normalized});
    for (const CubicBinds::Texture &t : textures.cubic.textures)
        formats.push_back({t.slot, t.format});
    for (const WrittenTexture &t : textures.written)
        formats.push_back({t.slot, t.format});
    const NmbDirectBinds binds = {
        cubic.data(),           uint32_t(cubic.size()), formats.data(),   uint32_t(formats.size()),
        textures.fragmentTests, textures.bindStride,    textures.twoPlane};
    const bool plain =
        formats.empty() && cubic.empty() && !textures.fragmentTests && !textures.bindStride && !textures.twoPlane;
    const char *refused = nullptr;
    if (!translator_.render(descriptor, barrier, textures.format, textures.samples, plain ? nullptr : &binds, made, &a,
                            &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? CompileOutcome::Failed : CompileOutcome::Refused;
    }
    why = a.why;
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
    if (!translator_.render || !nvk_.complete()) {
        why = nvk_.nakCompileStage && !nvk_.abiMatches ? "NVK's switch_nvk_compile structs are not raw_direct.cpp's"
                                                       : "the frontend or NVK has no direct compile";
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
    const CompileOutcome outcome = form(descriptor, barrier, textures, key, attributes, *twin, twinWhy, nullptr, split);
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
    out.strideTwin = std::move(twin);
    return CompileOutcome::Made;
}

}  // namespace raw

namespace raw {

KernelDispatch::KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode)
{
    memcpy(dispatch_.threads, threads, sizeof dispatch_.threads);
    dispatch_.regionMode = regionMode;
    for (const auto &[index, length] : memory)
        if (index < 31)
            dispatch_.memory[index] = length;
}

void KernelDispatch::texture(uint32_t slot, MTLTextureDescriptor *d)
{
    textures_.push_back(
        {slot, d.pixelFormat, d.textureType, d.usage, d.mipmapLevelCount, d.arrayLength, d.sampleCount});
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

CompileOutcome Direct::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                              const std::string &key, ComputeRecord &out, std::string &why)
{
    if (!translator_.compute || !nvk_.complete()) {
        why = "the frontend or NVK has no direct compile of kernels";
        return CompileOutcome::Failed;
    }
    AskedKernel a = {nvk_, key, out, {}};
    const char *refused = nullptr;
    if (!translator_.compute(descriptor, &dispatch, madeKernel, &a, &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? CompileOutcome::Failed : CompileOutcome::Refused;
    }
    why = a.why;
    return a.outcome;
}

}  // namespace raw
