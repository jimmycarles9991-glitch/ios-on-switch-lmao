// SPDX-License-Identifier: MIT
// The raw device's direct compile of a state the pack lacks (raw_direct.h), in process: the state's record made from m2v's
// translation of its functions and NVK's stage compile, at its creation, as the pack generator makes the pack's records. A state
// whose fragment function reads its color attachments uses its qualified ordered form when the native views support it.
#pragma once
#import <Metal/Metal.h>
#include <map>
#include <string>
#include <vector>
#include "bridge/NativeMetalDirect.h"
#include "raw_compute_record.h"
#include "raw_cubic.h"
#include "raw_direct.h"
#include "raw_written_textures.h"

namespace raw {

class Pack;

// A dispatch's shape and what it binds besides its buffers, as the direct compile of its kernel takes them
class KernelDispatch {
public:
    KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode = false);
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
    uint8_t fragmentTests = 0;
    uint32_t bindStride = 0;  // the attributeStride every vertex buffer is bound at (0: none)
    bool twoPlane = false;    // its RGB10A8_2P_XR10 colours in their two planes (raw_xr_pass.h: its " | xr2p" variant)
    uint16_t xrSlots = 0;     // fragment slots it samples two-plane RGB10A8_2P_XR10 at (its " | xrs=" variant)
    // the formats of the textures it writes at these slots (raw_written_textures.h: its " | wtex=" variant)
    std::vector<WrittenTexture> written;
};

// What makes a state's or a kernel's translation and finds NVK's exports: the frontend over its m2v runtime device, or
// the pack generator's offline compile (pack/offline_metal.h), whose descriptors hold its own functions. False with `why`:
// the translator refused it.
struct DirectTranslator {
    bool (*render)(id descriptor, bool barrier, NSUInteger textureFormat, NSUInteger textureSamples,
                   const NmbDirectBinds *binds, bool (*each)(const NmbDirectState *, void *), void *ctx,
                   const char **why) = nullptr;
    bool (*compute)(id descriptor, const NmbDirectDispatch *dispatch, bool (*each)(const NmbDirectKernel *, void *),
                    void *ctx, const char **why) = nullptr;
    void *(*nvkSymbol)(const char *name) = nullptr;

    static DirectTranslator frontend();
};

// m2v's own instantiation of a state's probe draw that binds every buffer slot at its probe offset (pack/probe_bind.h,
// NmbDirectState draws[3]): its root push words and each stage's argument table, what a host check (raw/test/boundcheck.py)
// compares the words the raw path writes for that draw with
struct BoundDraw {
    uint32_t pushMask = 0;
    uint32_t push[32] = {};
    std::vector<uint32_t> spill[2];
};

class Direct {
public:
    explicit Direct(const Pack &pack);
    // The pack generator's: records under `rules` alone (pack/directpack.mm)
    explicit Direct(PackRules rules, bool rankArtifacts = false,
                    DirectTranslator translator = DirectTranslator::frontend());

    // The record of the state `descriptor` (of m2v's functions) describes, whose pack key is `key`; `registers`: as
    // directRecord's; `split`: with its fragment program's split (directRecord). A state of bind-stride vertex streams
    // gets its stride twin (RuntimeRecord::strideTwin) with it.
    CompileOutcome make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                        std::string &why, ProbeTextures textures = {},
                        std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false,
                        PackForm requested = PackForm::Default);

    // The pack generator's: records also hold the program variants directRecord makes (`variants`)
    void makeVariants(bool on)
    {
        variants_ = on;
    }

    // Why the last make's storage form was refused where it made the barrier form instead; empty where it made none
    const std::string &storageRefusal() const
    {
        return storageRefusal_;
    }

    // The pack generator's: each make keeps m2v's bound probe draw of the state it made (boundDraw)
    void keepBoundDraws(bool on)
    {
        keepBound_ = on;
    }

    const BoundDraw &boundDraw() const
    {
        return bound_;
    }

    // The record of `descriptor`'s kernel at a dispatch of `dispatch`'s shape and bindings, whose compute key is `key`
    // (raw_direct_compute.h)
    CompileOutcome kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                          const std::string &key, ComputeRecord &out, std::string &why);

private:
    PackRules rules_;
    DirectTranslator translator_;
    DirectNvk nvk_;
    bool variants_ = false;
    std::string storageRefusal_;
    bool keepBound_ = false;
    BoundDraw bound_;

    CompileOutcome form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                        const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                        std::string &why, std::map<uint32_t, uint32_t> *registers, bool split);
    CompileOutcome strideTwin(MTLRenderPipelineDescriptor *descriptor, ProbeTextures textures, const std::string &key,
                              const std::vector<uint32_t> &attributes, RuntimeRecord &out, std::string &why,
                              bool split);
};

}  // namespace raw
