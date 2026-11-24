// SPDX-License-Identifier: MIT
// A raw range of a command buffer's tape onto the driver in three steps: open (the raw command buffer, the range's first
// contents and resources, the planar images its draws sample), step (its commands up to a position, as many times as it
// is given more) and seal (its encoders ended, the tail, the kernel's job). Only the thread that owns the device's
// encoding (the submit worker) runs it.
#pragma once
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalCounters.h"
#import "TegraGM20BMetalFresh.h"
#import "TegraGM20BMetalStandIns.h"
#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <set>
#include <vector>
#include "compute.h"

class PlanDraws;
#include "kernel_shadow.h"
#include "pass_fusion.h"
#include "reach.h"
#include "turned_pass.h"
#include "xr_parts.h"

class SwitchMetalRangeEncode {
public:
    // `whole`: [from, to) is the whole range, its resources and first contents taken at once, after its commit. Else it is
    // encoded ahead, before the commit, as its caller records more: each encoder's resources and first contents are taken
    // as it is stepped into, nothing reads what the CPU may write until the commit (buffer contents, a surface's seed),
    // and an encoder whose encode would read such a thing stops it (vetoed()). False: *why; nothing of it was submitted.
    bool open(SwitchMetalCommandBuffer *cb, size_t from, size_t to, bool whole, const char **why);
    // The range's commands up to `until`; false: failed (*why) or, encoded ahead, stopped (vetoed()). Encoded ahead, it
    // looks one encoder past `until` (within the ended tape) and halts before the first encoder it cannot take, the end
    // before that encoder encoded as the range's end (halted()).
    bool step(size_t until);

    // Encoded ahead: the range ends at `stop` at the latest (the recording's stop, SIZE_MAX: none), and the tape before
    // `ended` holds only ended encoders
    void limit(size_t stop, size_t ended)
    {
        to_ = std::min(to_, stop);
        ended_ = ended;
    }

    // Encoded ahead: the first command of the encoder it halted before (0: none)
    size_t halted() const
    {
        return halt_;
    }

    // Encoded ahead: its last encoder ended with nothing left open, so the range may end at position()
    bool betweenEncoders() const
    {
        return at_ > from_ && !re_ && !be_ && !ce_ && !continuing_;
    }

    // An encode opened ahead goes on after its command buffer's commit, up to `to` where its range ends; false: as open()
    bool resume(size_t to, const char **why);
    // False: *why
    bool seal(size_t to);
    // What open() took that no seal() submitted goes back (the raw command buffer abandoned where it is still open)
    void reset();

    size_t position() const
    {
        return at_;
    }

    // Why an encode ahead stopped (SwitchMetalPipelineVeto), 0 where it did not
    uint32_t vetoed() const
    {
        return veto_;
    }

    // open() or resume() stopped on an event producer still pending on a channel made again since: the range waits for its
    // outcome, which wakes its queue (event_timeline.h consumers)
    bool producerPending() const
    {
        return producerPending_;
    }

    // The channel it began on (gm20b::Device::channelGeneration)
    uint64_t generation() const
    {
        return generation_;
    }

private:
    struct Resolve {
        uint32_t attachment;
        __unsafe_unretained SwitchMetalTexture *into;
        bool keepSamples;  // MTLStoreActionStoreAndMultisampleResolve
        uint32_t level, slice;
    };

    // A depth attachment's resolve (samples.h depthResolveState): a pass of the depth resolve state of its format and
    // filter into the resolve texture, writing each pixel's depth whatever it held
    struct DepthResolve {
        __unsafe_unretained SwitchMetalTexture *into = nil;
        uint32_t filter = 0;
        uint32_t level = 0, slice = 0;
    };

    struct StencilResolve {  // CommandBuffer::stencilResolve's pass (stencil_bits.h)
        __unsafe_unretained SwitchMetalTexture *into = nil;
        gm20b::StencilSample pick = gm20b::StencilSample::Zero;
        uint32_t level = 0, slice = 0;
    };

    struct Conversion {
        __unsafe_unretained SwitchMetalTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    bool fail(SwitchMetalPred p, const char *w, uint64_t a = 0, uint64_t b = 0, const char *detail = nullptr);
    bool passRefused(uint64_t a);
    bool refused();
    bool stop(uint32_t veto);
    uint32_t ineligible(size_t from, size_t until, size_t &stopAt);
    bool loadsAStoredDepth(size_t x);
    bool enter(size_t begin);
    bool resources(size_t from, size_t to);
    bool borderHeaders(size_t from, size_t to);
    bool conversions();
    bool remakeImages(size_t end);
    bool makeImages(const std::vector<__unsafe_unretained SwitchMetalTexture *> &planar);
    int standInPass(SwitchMetalTexture *t, uint32_t slot, bool store);
    void standInBackings();
    bool storeStandIns();
    bool loadPlanes(gm20b::PassDesc &p);
    bool formatStandIns(const SwitchMetalPass &pass, gm20b::PassDesc &p, const PlanDraws &draws, bool continued);
    bool storeFormatTargets();
    // The open pass's RGB10A8_2P_XR10 targets held in planes that overlap (xr_pools.h xrPlanesOverlap) and plain ones
    // over rows that overlap (plain_rows.h): rendered in their pools, filled first where loaded, and stored into their
    // rows after the pass
    void overlappingPlanes(const SwitchMetalPass &pass, gm20b::PassDesc &p);
    bool storeOverlappingPlanes();
    // The open pass's targets that are texel buffers held in a pool of a block-linear surface's rows (texel_rows.h):
    // rendered in their pools, filled first, copied back after the pass where it stores them (writtenBack)
    void texelRowsTargets(const SwitchMetalPass &pass);
    // A blit command over such a texel buffer reads and writes its pool (blitImage): filled before the command (`before`),
    // copied back after one that writes it
    void blitTexelRows(const Cmd &c, bool before);
    // Colour j's planes `plane` .. + `layers` of `t`, laid out several planes a block (gm20b::planeInBlocks), as a
    // one-plane-a-block image the copy engine fills from them where `filled` and drains into them after the pass where
    // `stored` (planesBack); null: no memory for it (*why_)
    gm20b::Texture *planeImage(SwitchMetalTexture *t, uint32_t j, uint32_t plane, uint32_t layers, bool filled,
                               bool stored);
    void planesBack();
    void separateZeta(const SwitchMetalPass &pass, SwitchMetalTexture *depth, SwitchMetalTexture *stencil,
                      gm20b::PassDesc &p);
    bool depth16Zeta(const SwitchMetalPass &pass, gm20b::PassDesc &p);
    bool depth16Tested(const SwitchMetalPass &pass, gm20b::PassDesc &p);
    bool depth16Back();
    bool imagePlanes(size_t pass, gm20b::PassDesc &p);
    bool writtenBack();
    bool resolvePasses();
    void refresh(size_t pass);
    bool standIns(size_t pass);
    bool drawStandIns(const std::vector<__unsafe_unretained SwitchMetalTexture *> &stale);
    bool standInsGoStale(size_t encoderAt);
    void localShadows(std::vector<gm20b::KernelUse> &list);
    void shadowCopies(const std::vector<const void *> &back, const std::vector<const void *> &fill);
    void beginKernelShadows(size_t encoderAt);
    void endKernelShadows();
    bool command(size_t at);
    bool beginPass(size_t at);
    bool endEncoder(size_t at);
    bool endEncoders();
    bool closeEncoders();
    bool producersOrdered();

    __unsafe_unretained SwitchMetalCommandBuffer *cb_ = nil;
    gm20b::Device *d_ = nullptr;
    gm20b::CommandBuffer *rc_ = nullptr;
    const char **why_ = nullptr;
    size_t from_ = 0, to_ = 0, at_ = 0;
    size_t ended_ = 0, halt_ = 0;  // encoded ahead: limit()'s ended tape, and where it halted
    bool whole_ = true;
    bool ahead_ = false;  // before its commit: the recording goes on
    bool abandoned_ = false, sealed_ = false, producerPending_ = false;
    uint32_t veto_ = 0;
    bool sampledShadows_ = false, standIns_ = false;  // the command buffer's, as of its commit (ahead: none)
    uint64_t generation_ = 0;
    std::optional<FreshWrites> firstContents_;
    std::optional<SwitchMetalCounterEmit> counters_;

    gm20b::RenderEncoder *re_ = nullptr;
    gm20b::BlitEncoder *be_ = nullptr;
    std::optional<gm20b::ComputeEncoder> ce_;
    bool serialCompute_ = false, computeUnit_ = false;
    // a predication Start or EndDoWhile naming no buffer withholds the open compute unit's dispatches
    bool unitWithheld_ = false;
    gm20b::KernelShadows kernelShadows_;  // the open compute encoder's (kernel_shadow.h)
    std::vector<gm20b::KernelUse> dispatchUses_, encoderUses_;
    bool encoderShadows_ = false;
    bool encoderTracked_ = false;  // its dispatches' uses are tracked: shadowed images, or stand-ins writes reach
    gm20b::StateCursor stateCursor_;
    gm20b::FusionPass fused_;
    size_t fusedAt_ = SIZE_MAX;
    gm20b::PassUses planned_;
    size_t renderBegin_ = SIZE_MAX;
    uint32_t passDraws_ = 0;  // the raw command buffer's draws as the open pass began (resolvePasses: whether it drew)
    bool passDrew_ = false;   // the pass closing drew, read before the driver's own draws after it (closeEncoders)
    bool continuing_ = false;
    size_t encoder_ = SIZE_MAX;  // the encoder being encoded (SIZE_MAX: none)
    size_t freshAt_ = SIZE_MAX;  // encoded ahead: the encoder whose first uses of fresh textures were claimed last

    // A planar render target's stand-in passes (planar_target.h), with their attachment slots, and the stand-in each slot
    // took of the command buffer's for a texture that holds none
    std::vector<std::pair<__unsafe_unretained SwitchMetalTexture *, uint32_t>> standInLoads_, standInStores_;
    std::array<gm20b::Texture *, 8> planarScratch_{};
    // overlapStores_ the open pass's targets held in planes or rows that overlap, overlapLoaded_ those of them it loads
    std::vector<__unsafe_unretained SwitchMetalTexture *> planesWritten_, overlapStores_, overlapLoaded_;
    std::array<const gm20b::Texture *, 8> backings_{};
    // The open pass's loaded two-plane YCbCr targets and their slots, the tiles its draws reach (ycbcr_load.h) and the
    // targets whose stores write those tiles alone
    std::vector<std::pair<__unsafe_unretained SwitchMetalTexture *, uint32_t>> planeLoads_;
    gm20b::PassReach loadedReach_;
    uint8_t loadedTargets_ = 0;
    // Targets of a format the GM20B renders none of (format_standin.h): the open pass's and their slots, then each
    // stored attachment's image and its stand-in, put back after the pass
    std::vector<std::pair<__unsafe_unretained SwitchMetalTexture *, uint32_t>> formatTargets_;

    struct FormatStore {
        gm20b::ColorAttachment attachment;
        uint32_t layers;
        gm20b::Texture *standIn;
        bool filled;  // from the texels (storeFormatStandIn's keep)
    };

    std::vector<FormatStore> formatStores_;

    // The open pass's plane images its stores drain into their 3D textures (planeImage)
    struct PlaneImage {
        __unsafe_unretained SwitchMetalTexture *t;
        gm20b::Texture *image;
        uint32_t plane, layers;
    };

    std::vector<PlaneImage> planeImages_;
    std::vector<Resolve> resolves_;
    std::deque<gm20b::Texture> resolveTargets_;  // the encoder reads them until its end()
    gm20b::TurnedPass turned_;                   // the open pass's turn copies (turned_pass.h)
    DepthResolve depthResolve_;
    StencilResolve stencilResolve_;
    std::vector<__unsafe_unretained SwitchMetalSampler *> bordered_;
    std::vector<Conversion> converted_;  // planar images converted here, at their surfaces' seeds and write counts
    // the converted textures the range samples whose surface it also writes: drawn again after each encoder that writes
    // it, ahead of the next work that samples them (remakeImages)
    std::vector<__unsafe_unretained SwitchMetalTexture *> rewritten_;
    std::set<SwitchMetalTexture *> fresh_;
    gm20b::Texture levelTargets_[8];  // the open pass's level targets: the encoder reads them until its end()
    gm20b::Texture zetaPart_;         // and its depth/stencil attachment's layer
    gm20b::Texture separatePart_[2];  // a separate depth and stencil texture's layers from their slices
    // a separate depth or stencil texture a resolve reads: the pass stores the aspect back into it, whose own resolve state
    // reads it
    const gm20b::Texture *separateResolved_[2] = {};
    // a stored Depth16Unorm beside a Stencil8, rendered through a Depth32Float_Stencil8 scratch: converted back at the end
    gm20b::Texture *depth16Scratch_ = nullptr;
    // Textures the open pass's draws store into: through a sampled shadow, copied back after the pass; under a stand-in,
    // drawn again before later work samples it; two-plane RGB10A8_2P_XR10 ones through an image of their texels, merged
    // from the planes before the pass and split back after it (TegraGM20BMetalXrPlanes.mm)
    std::vector<__unsafe_unretained SwitchMetalTexture *> shadowsWritten_, standInsWritten_;
    std::vector<std::pair<__unsafe_unretained SwitchMetalTexture *, gm20b::Texture *>> planesImaged_;
    std::vector<gm20b::PassDesc::WrittenPlanes> morePlanes_;  // past gm20b::PassDesc's inline ones (its morePlanes)
    SwitchMetalStandInFresh standInFresh_;
    std::deque<gm20b::Texture>
        codes_;  // the XR conversions' sources: the codes' headers (read until the encoder's end)
    std::deque<gm20b::XrPart> codeParts_;  // a layered, mipmapped, 3D or multisampled one's 2D images (xr_parts.h)
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t levelPasses_ = 0;
#endif
};

// The submit worker's one range encode
SwitchMetalRangeEncode &gm20bRangeEncode();
// A texture a kernel or a draw reaches through an image of its own (kernel_shadow.h): that image filled from the
// texels where `fill`, else copied back into them. Two-plane RGB10A8_2P_XR10 planes' pools (xr_pools.h), a texel
// buffer's pool of its surface's rows (texel_rows.h), or Texture::sampled
void gm20bMoveShadowImage(gm20b::BlitEncoder &b, const gm20b::Texture &t, bool fill);
// The refusal predicate of an encode-time refusal or skip (draw_refusal.h RefusalId)
SwitchMetalPred refusalPred(uint8_t id);
