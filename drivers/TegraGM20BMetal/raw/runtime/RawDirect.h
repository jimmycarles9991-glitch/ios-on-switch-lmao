// SPDX-License-Identifier: MIT
// The raw device's direct compile of a state the pack lacks (raw_direct.h), in process: the state's record made from m2v's
// translation of its functions and NVK's stage compile, at its creation, as the pack generator makes the pack's records. A state
// whose fragment function reads its color attachments under the pixel interlock gets its barrier form.
#pragma once
#import <Metal/Metal.h>
#include <map>
#include <string>
#include <vector>
#include "bridge/NativeMetalDirect.h"
#include "raw_compiler.h"
#include "raw_compute_record.h"
#include "raw_cubic.h"
#include "raw_direct.h"

namespace raw {

class Pack;

// A dispatch's shape and what it binds besides its buffers, as the direct compile of its kernel takes them
class KernelDispatch {
public:
    KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory);
    KernelDispatch(const KernelDispatch &) = delete;
    KernelDispatch &operator=(const KernelDispatch &) = delete;

    void texture(uint32_t slot, MTLTextureDescriptor *d);
    void sampler(uint32_t slot, MTLSamplerDescriptor *d);
    const NmbDirectDispatch &dispatch();

private:
    NmbDirectDispatch dispatch_ = {};
    std::vector<NmbDirectBoundTexture> textures_;
    std::vector<NmbDirectBoundSampler> samplers_;
};

// The textures a state is planned for, as the probe binds them (pack/probe.mm: BGRA8 but for the raw path's own states that
// read codes, and the multisampled ones its resolves read)
struct ProbeTextures {
    MTLPixelFormat format = MTLPixelFormatBGRA8Unorm;
    NSUInteger samples = 1;
    CubicBinds cubic;  // the cubic samplers and the textures sampled through them (a state's cubic variant)
};

class Direct {
public:
    explicit Direct(const Pack &pack);
    // The pack generator's: records under `rules` alone (pack/directpack.mm)
    explicit Direct(PackRules rules);

    // The record of the state `descriptor` (of m2v's functions) describes, whose pack key is `key`; `registers`: as
    // directRecord's; `split`: with its fragment program's split (directRecord)
    Compiler::Outcome make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                           std::string &why, ProbeTextures textures = {},
                           std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false);
    // The record of `descriptor`'s kernel at a dispatch of `dispatch`'s shape and bindings, whose compute key is `key`
    // (raw_direct_compute.h)
    Compiler::Outcome kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                             const std::string &key, ComputeRecord &out, std::string &why);

private:
    PackRules rules_;
    DirectNvk nvk_;

    Compiler::Outcome form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                           const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                           std::string &why, std::map<uint32_t, uint32_t> *registers, bool split);
};

}  // namespace raw
