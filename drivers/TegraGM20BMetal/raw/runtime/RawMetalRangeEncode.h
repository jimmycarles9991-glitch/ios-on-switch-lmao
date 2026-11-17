// SPDX-License-Identifier: MIT
// A raw range of a command buffer's tape onto the raw path in three steps: open (the raw command buffer, the range's first
// contents and resources, the planar images its draws sample), step (its commands up to a position, as many times as it
// is given more) and seal (its encoders ended, the tail, the kernel's job). Only the thread that owns the device's
// encoding (the submit worker) runs it.
#pragma once
#import "RawMetalInternal.h"
#import "RawMetalCounters.h"
#import "RawMetalFresh.h"
#include <array>
#include <deque>
#include <optional>
#include <set>
#include <vector>
#include "raw_compute.h"
#include "raw_kernel_windows.h"
#include "raw_pass_fusion.h"
#include "raw_reach.h"
#include "raw_turned_pass.h"
#include "raw_xr_parts.h"

class RMRangeEncode {
public:
    // `whole`: [from, to) is the whole range, its resources and first contents taken at once, after its commit. Else it is
    // encoded ahead, before the commit, as its caller records more: each encoder's resources and first contents are taken
    // as it is stepped into, nothing reads what the CPU may write until the commit (buffer contents, a surface's seed),
    // and an encoder whose encode would read such a thing stops it (vetoed()). False: *why (and *refusedEncoder where one
    // encoder is to blame); nothing of it was submitted.
    bool open(RMCommandBuffer *cb, size_t from, size_t to, bool whole, const char **why, size_t *refusedEncoder);
    // The range's commands up to `until`; false: failed (*why) or, encoded ahead, stopped (vetoed())
    bool step(size_t until);
    // An encode opened ahead goes on after its command buffer's commit, up to `to` where its range ends; false: as open()
    bool resume(size_t to, const char **why, size_t *refusedEncoder);
    // False: *why, or `stale` (a native surface changed: encode it again)
    bool seal(size_t to, bool &stale);
    // What open() took that no seal() submitted goes back (the raw command buffer abandoned where it is still open)
    void reset();

    size_t position() const
    {
        return at_;
    }

    // Why an encode ahead stopped (RMPipelineVeto), 0 where it did not
    uint32_t vetoed() const
    {
        return veto_;
    }

    // The channel it began on (raw::Device::channelGeneration)
    uint64_t generation() const
    {
        return generation_;
    }

private:
    // tRawEncoding (RawMetalPrivateComp.mm) while a step of it runs
    struct Encoding {
        Encoding()
        {
            tRawEncoding = true;
        }

        ~Encoding()
        {
            tRawEncoding = false;
        }
    };

    struct Resolve {
        uint32_t attachment;
        __unsafe_unretained RMTexture *into;
        bool keepSamples;  // MTLStoreActionStoreAndMultisampleResolve
        uint32_t level, slice;
    };

    // A depth attachment's resolve (raw_samples.h depthResolveState): a pass of the depth resolve state of its format and
    // filter into the resolve texture, writing each pixel's depth whatever it held
    struct DepthResolve {
        __unsafe_unretained RMTexture *into = nil;
        uint32_t filter = 0;
    };

    struct StencilResolve {  // CommandBuffer::stencilResolve's pass (raw_stencil_bits.h)
        __unsafe_unretained RMTexture *into = nil;
        raw::StencilSample pick = raw::StencilSample::Zero;
    };

    struct Conversion {
        __unsafe_unretained RMTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    bool fail(RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0, const char *detail = nullptr);
    bool passRefused(uint64_t a);
    bool refused();
    bool stop(uint32_t veto);
    uint32_t ineligible(size_t from, size_t until);
    bool loadsAStoredDepth(size_t x);
    bool enter(size_t begin);
    bool resources(size_t from, size_t to);
    bool borderHeaders(size_t from, size_t to);
    bool conversions();
    int standInPass(RMTexture *t, bool store);
    void standInBackings();
    bool storeStandIns();
    bool loadPlanes(raw::PassDesc &p);
    bool formatStandIns(const RMPass &pass, raw::PassDesc &p);
    bool storeFormatTargets();
    void separateZeta(const RMPass &pass, RMTexture *depth, RMTexture *stencil, raw::PassDesc &p);
    bool depth16Zeta(const RMPass &pass, raw::PassDesc &p);
    bool depth16Back();
    bool imagePlanes(size_t pass, raw::PassDesc &p);
    bool writtenBack();
    bool resolvePasses();
    void refresh(size_t pass);
    bool standIns(size_t pass);
    void localShadows(std::vector<raw::KernelUse> &list);
    void shadowCopies(const std::vector<const void *> &back, const std::vector<const void *> &fill);
    void beginKernelShadows(size_t encoderAt);
    void endKernelShadows();
    bool command(size_t at);
    bool beginPass(size_t at);
    bool endEncoder(size_t at);
    bool endEncoders();
    bool closeEncoders();

    __unsafe_unretained RMCommandBuffer *cb_ = nil;
    raw::Device *d_ = nullptr;
    raw::CommandBuffer *rc_ = nullptr;
    const char **why_ = nullptr;
    size_t *refusedEncoder_ = nullptr;
    size_t from_ = 0, to_ = 0, at_ = 0;
    bool whole_ = true;
    bool ahead_ = false;  // before its commit: the recording goes on
    bool abandoned_ = false, sealed_ = false;
    uint32_t veto_ = 0;
    bool sampledShadows_ = false, standIns_ = false;  // the command buffer's, as of its commit (ahead: none)
    uint64_t generation_ = 0;
    std::optional<FreshWrites> firstContents_;
    std::optional<RMCounterEmit> counters_;

    raw::RenderEncoder *re_ = nullptr;
    raw::BlitEncoder *be_ = nullptr;
    std::optional<raw::ComputeEncoder> ce_;
    bool serialCompute_ = false, computeUnit_ = false;
    raw::KernelShadows kernelShadows_;  // the open compute encoder's (raw_kernel_shadow.h)
    raw::KernelWindows kernelWindows_;  // the command buffer's buffer windows (raw_kernel_windows.h)
    std::vector<raw::KernelUse> dispatchUses_, encoderUses_;
    bool encoderShadows_ = false;
    raw::StateCursor stateCursor_;
    raw::FusionPass fused_;
    size_t fusedAt_ = SIZE_MAX;
    raw::PassUses planned_;
    size_t renderBegin_ = SIZE_MAX;
    bool continuing_ = false;
    size_t encoder_ = SIZE_MAX;  // the encoder being encoded, to blame for a refusal (SIZE_MAX: none)
    size_t freshAt_ = SIZE_MAX;  // encoded ahead: the encoder whose first uses of fresh textures were claimed last

    // A planar render target's stand-in passes (raw::Texture::planarStandIn)
    std::vector<__unsafe_unretained RMTexture *> standInLoads_, planesWritten_;
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> standInStores_;  // and their attachment slots
    std::array<const raw::Texture *, 8> backings_{};
    // The open pass's loaded two-plane YCbCr targets and their slots, the tiles its draws reach (raw_ycbcr_load.h) and the
    // targets whose stores write those tiles alone
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> planeLoads_;
    raw::PassReach loadedReach_;
    uint8_t loadedTargets_ = 0;
    // Targets of a format the GM20B renders none of (raw_format_standin.h): the open pass's and their slots, then each
    // stored attachment's image and its stand-in, put back after the pass
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> formatTargets_;
    std::vector<std::pair<raw::Texture *, raw::Texture *>> formatStores_;
    std::vector<Resolve> resolves_;
    std::deque<raw::Texture> resolveTargets_;  // the encoder reads them until its end()
    raw::TurnedPass turned_;                   // the open pass's turn copies (raw_turned_pass.h)
    DepthResolve depthResolve_;
    StencilResolve stencilResolve_;
    std::vector<__unsafe_unretained RMSampler *> bordered_;
    std::vector<Conversion> converted_;  // planar images converted here, at their surfaces' seeds and write counts
    std::set<RMTexture *> fresh_;
    raw::Texture levelTargets_[8];  // the open pass's level targets: the encoder reads them until its end()
    raw::Texture zetaPart_;         // and its depth/stencil attachment's layer
    raw::Texture separatePart_[2];  // a separate depth and stencil texture's layers from their slices
    // a separate depth or stencil texture a resolve reads: the pass stores the aspect back into it, whose own resolve state
    // reads it
    const raw::Texture *separateResolved_[2] = {};
    // a stored Depth16Unorm beside a Stencil8, rendered through a Depth32Float_Stencil8 scratch: converted back at the end
    raw::Texture *depth16Scratch_ = nullptr;
    // Textures the open pass's draws store into: through a sampled shadow, copied back after the pass; under a stand-in,
    // drawn again before later work samples it; two-plane RGB10A8_2P_XR10 ones through an image of their texels, merged
    // from the planes before the pass and split back after it (RawMetalXrPlanes.mm)
    std::vector<__unsafe_unretained RMTexture *> shadowsWritten_, standInsWritten_;
    std::vector<std::pair<__unsafe_unretained RMTexture *, raw::Texture *>> planesImaged_;
    std::set<RMTexture *> standInFresh_;
    std::deque<raw::Texture> codes_;  // the XR conversions' sources: the codes' headers (read until the encoder's end)
    std::deque<raw::XrPart> codeParts_;  // a layered, mipmapped, 3D or multisampled one's 2D images (raw_xr_parts.h)
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t levelPasses_ = 0;
#endif
};

// The submit worker's one range encode
RMRangeEncode &rmRangeEncode();
// The refusal predicate of an encode-time refusal or skip (raw_gpu.h RefusalId)
RMPred refusalPred(uint8_t id);
