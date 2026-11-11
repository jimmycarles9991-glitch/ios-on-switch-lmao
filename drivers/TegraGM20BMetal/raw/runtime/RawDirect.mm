// SPDX-License-Identifier: MIT
// See RawDirect.h.
#import "RawDirect.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "raw_direct_compute.h"
#include "bridge/NativeMetalDirect.h"
#include "raw_pack.h"

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
    std::string why;
    Compiler::Outcome outcome = Compiler::Outcome::Failed;
};

bool made(const NmbDirectState *state, void *ctx)
{
    Asked &a = *static_cast<Asked *>(ctx);
    a.outcome = directRecord(*state, a.nvk, a.rules, a.key, a.attributes, a.out, a.why, a.registers, a.split);
    return true;
}

struct AskedKernel {
    const DirectNvk &nvk;
    const std::string &key;
    ComputeRecord &out;
    std::string why;
    Compiler::Outcome outcome = Compiler::Outcome::Failed;
};

bool madeKernel(const NmbDirectKernel *kernel, void *ctx)
{
    AskedKernel &a = *static_cast<AskedKernel *>(ctx);
    a.outcome = directKernel(*kernel, a.nvk, a.key, a.out, a.why);
    return true;
}

}  // namespace

Direct::Direct(const Pack &pack) : Direct(PackRules::of(pack))
{}

Direct::Direct(PackRules rules)
    : rules_(std::move(rules)),
      nvk_(native_bridge_metal_nvk_symbol ? DirectNvk::find(native_bridge_metal_nvk_symbol) : DirectNvk{})
{}

Compiler::Outcome Direct::form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                               const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                               std::string &why, std::map<uint32_t, uint32_t> *registers, bool split)
{
    Asked a = {nvk_, rules_, key, attributes, out, registers, split, {}};
    std::vector<NmbDirectCubicSampler> cubic;
    std::vector<NmbDirectTextureFormat> formats;
    for (const CubicBinds::Sampler &c : textures.cubic.samplers)
        cubic.push_back({c.slot, c.s, c.t, c.r, c.border, c.normalized});
    for (const CubicBinds::Texture &t : textures.cubic.textures)
        formats.push_back({t.slot, t.format});
    const NmbDirectBinds binds = {cubic.data(), uint32_t(cubic.size()), formats.data(), uint32_t(formats.size())};
    const char *refused = nullptr;
    if (!native_bridge_metal_render_direct(descriptor, barrier, textures.format, textures.samples,
                                           textures.cubic.empty() ? nullptr : &binds, made, &a, &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? Compiler::Outcome::Failed : Compiler::Outcome::Refused;
    }
    why = a.why;
    return a.outcome;
}

Compiler::Outcome Direct::make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                               std::string &why, ProbeTextures textures, std::map<uint32_t, uint32_t> *registers,
                               bool split)
{
    if (!native_bridge_metal_render_direct || !nvk_.complete()) {
        why = nvk_.compileStage && !nvk_.abiMatches ? "NVK's switch_nvk_compile structs are not raw_direct.cpp's"
                                                    : "the frontend or NVK has no direct compile";
        return Compiler::Outcome::Failed;
    }
    std::vector<uint32_t> attributes;
    for (MTLVertexAttribute *a in descriptor.vertexFunction.vertexAttributes)
        if (a.isActive)
            attributes.push_back(uint32_t(a.attributeIndex));
    Compiler::Outcome outcome = form(descriptor, false, textures, key, attributes, out, why, registers, split);
    if (outcome != Compiler::Outcome::Refused || why.rfind("storage form", 0) != 0)
        return outcome;
    outcome = form(descriptor, true, textures, key, attributes, out, why, registers, split);
    if (outcome == Compiler::Outcome::Made)
        out.pipeline.flags |= PipelineBarrierForm;
    return outcome;
}

}  // namespace raw

namespace raw {

KernelDispatch::KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory)
{
    memcpy(dispatch_.threads, threads, sizeof dispatch_.threads);
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

Compiler::Outcome Direct::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                                 const std::string &key, ComputeRecord &out, std::string &why)
{
    if (!native_bridge_metal_compute_direct || !nvk_.kernelComplete()) {
        why = "the frontend or NVK has no direct compile of kernels";
        return Compiler::Outcome::Failed;
    }
    AskedKernel a = {nvk_, key, out, {}};
    const char *refused = nullptr;
    if (!native_bridge_metal_compute_direct(descriptor, &dispatch, madeKernel, &a, &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? Compiler::Outcome::Failed : Compiler::Outcome::Refused;
    }
    why = a.why;
    return a.outcome;
}

}  // namespace raw
