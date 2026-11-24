// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "band_rates.h"
#include "barrier_forms.h"
#include "batch.h"
#include "blit_encoder.h"
#include "buffer_writes.h"
#include "counters.h"
#include "dirty_ranges.h"
#include "draw_refusal.h"
#include "draw_times.h"
#include "event_release.h"
#include "geometry.h"
#include "heap.h"
#include "heap_members.h"
#include "kernel.h"
#include "loop_copies.h"
#include "pass_desc.h"
#include "pass_hazard.h"
#include "pipeline.h"
#include "predication.h"
#include "probe_forms.h"
#include "push.h"
#include "reach.h"
#include "render_encoder.h"
#include "resource_set.h"
#include "resources.h"
#include "shared_events.h"
#include "split_batch.h"
#include "split_capture.h"
#include "split_reuse.h"
#include "state_segment.h"
#include "stats.h"
#include "stencil_bits.h"
#include "surfaces.h"
#include "tiled_cache.h"
#include "turn_copy.h"
#include "use_memo.h"
#include "zcull_planes.h"
#include "zeta.h"
#if SWITCH_METAL_DIAGNOSTICS
#include "glass_mem.h"
#endif

namespace gm20b {

class CommandBuffer {
public:
    Refusal refusal;  // the last refused draw's predicate and its values
    // null: a pass Metal's validation stops or no memory for its backings, `*why` the rule or error that stops it
    RenderEncoder *render(const PassDesc &pass, const char **why = nullptr);
    Refusal skipped;  // the last draw skipped as Apple's driver draws nothing for it
    uint32_t skips = 0;
    // turn_copy.h: each texel of `dst` from `src`'s at `map`, both of one texel size or depth format (a pass of the pack's
    // turn copy state; `src` given a texture header where it has none), over `dst`'s first width x height (0: all of an axis);
    // false: *why
    bool turnCopy(const Texture &dst, Texture &src, const TurnMap &map, const char **why,
                  const TurnMask *mask = nullptr, uint32_t width = 0, uint32_t height = 0);
    // The source's resource-owned header was prepared at admission or with its scratch backing.
    bool turnCopy(const Texture &dst, const Texture &src, const TurnMap &map, const char **why,
                  const TurnMask *mask = nullptr, uint32_t width = 0, uint32_t height = 0);
    // `dst`'s depth from `src`'s of another depth format of one extent (a Depth16Unorm's and a Depth32Float_Stencil8's), each
    // texel the float the texture unit reads of the source written as the destination's depth by the pack's turn depth
    // state of its format; its stencil kept. The source already has its resource-owned selected-level header. False: *why
    bool depthConvert(const Texture &dst, const Texture &src, const char **why);
    // reach.h: the tiles of `pass`'s render area (reachArea: within `width` x `height`, 0 where the pass names none) that
    // its draws reach, drawn again from `draws`; false: *why, out.memory where there was no memory for the reach target
    bool reach(const PassDesc &pass, uint32_t width, uint32_t height, DrawReplay &draws, PassReach &out,
               const char **why);
    // A block-linear image of `width` x `height` texels of `format` at `samples` samples, `layers` layers one layer's whole
    // tiles apart (Texture::layerBytes), the device's backing of tag `tag` for this command buffer: what a turn copy moves
    // texels through, a format stand-in's target (format_standin.h); null where none can be had
    Texture *turnScratch(uint32_t format, uint32_t width, uint32_t height, uint32_t tag, uint32_t samples = 1,
                         uint32_t layers = 1);
    // stencil_bits.h: `src`'s stencil drawn into `e`'s zeta of `format` (its stencil cleared to 0 at the pass's start, or
    // `zeroFirst`: zeroed by a draw where `map`'s source rectangle reaches, the rest kept), each pixel's from `src`'s texel at
    // `map` or its `pick` sample; `view` the stencil header's texture, read until e's end()
    bool stencilBits(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format, const TurnMap &map,
                     StencilSample pick, const char **why, bool zeroFirst = false);
    // `src`'s stencil bound for a stencil program into a zeta of `format`: its stencil header's texture `view` at 0, or with
    // `depth` `src` itself at 0 (its depth, read as its float's bits) and `view` at 1; `view` read until e's end()
    bool bindStencilSource(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format, bool depth,
                           const char **why);
    // mip_passes.cpp: levels first + 1 .. last of the selected layers of chain `t` drawn by mip state `mip`, a pass each
    bool mipPasses(Texture &t, Pipeline &mip, uint32_t first, uint32_t last, uint32_t firstLayer, uint32_t layers,
                   uint32_t format, const char **why);
    // A multisampled zeta's stencil resolve into one-sample `into` of its format, over (0, 0, width, height): a pass of its own
    bool stencilResolve(Texture &into, const Texture &from, StencilSample pick, uint32_t width, uint32_t height,
                        const char **why);
    // zeta_colour.h: whether one draw of the colour ROP writes `into`'s stencil from `src` (its `pick` sample), and that
    // draw's pass: each texel from `src`'s at `map`, or its pick sample's stencil, over (0, 0, width, height) (0: all)
    bool zetaColourTakes(const Texture &into, const Texture &src, StencilSample pick);
    bool zetaColour(const Texture &into, const Texture &src, StencilSample pick, const TurnMap &map, uint32_t width,
                    uint32_t height, const char **why);
    BlitEncoder *blit();
    // `bytes` of GPU memory this command buffer's work writes and reads until it completes (the upload ring's, on 256
    // bytes); 0: none, the ring past what it can grow to, whose commit is then refused
    uint64_t transient(uint32_t bytes);

    // Metal's predication unit ended (predication.h)
    void endPredicationUnit()
    {
        predication_.endUnit();
        loops_ = {};
    }

    // `multisampled` resolved into `into` by the 2D engine (rule resolve.twod, resolve_rule.h), all of it, or
    // [0, width) x [0, height) of it, the render area, where those are given; false: not a sample grid the engine reads
    // at 2:1, nothing recorded
    bool resolveTwod(const Texture &multisampled, const Texture &into, bool exact = false, bool keepSamples = false,
                     uint32_t width = 0, uint32_t height = 0);
    // `resolved`, a one-sample image of `multisampled`'s format, into each sample of it over the render area by the 2D
    // engine (BlitEncoder::spreadTwod); false: a format the 2D engine takes none of, nothing recorded
    bool spreadSamples(const Texture &resolved, const Texture &multisampled, uint32_t width, uint32_t height);
    // Counter samples behind every encoder ended so far, each on its own engine, in a segment of their own
    void timestamps(const counters::Stamp *stamps, size_t count);

    // CPU lines the GPU reads in this submission (CPU-written cached memory): cleaned to memory before it is submitted.
    // `invalidate`: also dropped from the CPU's caches (memory the GPU writes, so no dirty line lands on it later).
    void clean(const void *cpu, uint64_t bytes, bool invalidate = false)
    {
        dirty_.add(cpu, bytes, invalidate);
    }

    // Bounded legal draw/dispatch batches, in channel order, without CPU waits between their Execs.
    // false with `partial`: the kernel refused a later Exec after earlier ones were submitted; those completed before it
    // returned, and the next command buffer gets a new channel (the GPU's state after them is not what the device tracks).
    bool commit(uint32_t &seqno);

    // releases: one per signal, the payload releaseEvent put in the pushes for it (0: its job's end signals it)
    void sharedEvents(std::vector<TegraGM20BAbiEventUse> waits, std::vector<TegraGM20BAbiEventUse> signals,
                      std::vector<uint32_t> releases = {})
    {
        sharedWaits_ = std::move(waits);
        sharedSignals_ = std::move(signals);
        sharedReleases_ = std::move(releases);
        sharedReleases_.resize(sharedSignals_.size());
    }

    // A kernel event's signal here, after the work encoded so far: `payload` into its word at `va` (event_release.h)
    void releaseEvent(uint64_t va, uint32_t payload);
    bool partial = false;
    bool admissionBlocked = false;
    // the commit's last job is quiet too (completion_source.h quietEnd); every job before it always is
    bool quietEnd = false;
    // the writer promises its last job keeps (Device::promise), and whether a job of the commit carried them
    std::vector<uint32_t> promises;
    bool promisesKept = false;
    int nativeError = 0;
    // A commit that submitted nothing more: the kernel's answer to the job it refused (0: none), else why none was sent
    int kernelResult = 0;
    const char *notSubmitted = nullptr;
    // Why the last commit submitted nothing more, as its error line and its command buffer's error give it
    void describeRefusal(char *line, size_t size) const;
    // Why a job was refused when it was the resource set: its generation ended and the kernel refused to make it again
    // (SetView::refused). The command buffer fails with it.
    std::string setRefused;
    std::shared_ptr<ResourceReadyCompletion> resourceReadiness(std::function<void()> ready, int &result);
    const Texture *readTexture(const Texture *texture);
    void requirePitch(const Import *source, bool writes);
    // An import required pitch after a texture over it was read through its native image
    bool pitchAfterNativeRead() const;
    bool nativeReadsCurrent() const;
    void invalidateNativeReads();
    void releaseNativeReads();

    void internalCompletion()
    {
        visibility_ = JobVisibility::Internal;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    void forceWholePrograms(bool force)
    {
        forceWholePrograms_ = force;
    }

    void splitProbe(SplitProbe probe)
    {
        splitProbe_ = probe;
    }

    // HW Test's bench: every draw the split plan would take if large enough is taken, without a plan
    void forceSplit(bool force)
    {
        splitForced_ = force;
    }

    // The split capture's records of this command buffer's draws (split_capture.h), handed over once
    std::vector<SplitCaptureRecord> takeSplitCaptures()
    {
        return std::move(splitCaptures_);
    }

    // The batch records of this command buffer's split draws (split_batch.h), one a prologue launch, until its commit
    const std::vector<SplitBatchRecord> &splitRecords() const
    {
        return splitRecords_;
    }

    // HW Test's bench: each prologue launch's frontier (its address, its bytes), until the next begin()
    std::vector<std::pair<uint64_t, uint32_t>> splitFrontiers() const
    {
        std::vector<std::pair<uint64_t, uint32_t>> out;
        for (const SplitLaunch &l : splits_)
            if (l.batchRecord < splitRecords_.size())
                out.emplace_back(splitRecords_[l.batchRecord].frontier, l.split->rec.frontierBytes);
        return out;
    }

    // hw_test_app's measurement of state segments: every state bound as the inline copy it was before them
    void inlineStates(bool inline_)
    {
        inlineStates_ = inline_;
    }

    const ProbeForms &probeForms() const
    {
        return forms_;
    }

    const ProbeTaken &probeTaken() const
    {
        return taken_;
    }

    // a multisampled attachment's resolve by the 2D engine (probe_ops.h); false: none for its format or grid
    bool probeResolve(const Texture &multisampled, const Texture &into);
#endif
    // Nothing of it is on the GPU any more: the rings get back what it took, the root table's shadow what it was.
    void abandon();

    // A host import the submission reads or writes (a surface's, a no-copy buffer's), for the kernel's order with other
    // processes' GPU work on it (kernel.h exec)
    void use(const Import *i, bool write)
    {
        if (i)
            surfaces_.add(i->bo.handle, write);
    }

    // Actual GPU addresses resolve through this connection's binding table in the addressed job ABI.
    void useAddress(uint64_t va, uint64_t bytes, bool write);

    // An MTLHeap an encoder used (useHeap): named once per command buffer, its jobs name what it holds at their commit
    void useHeap(const HeapMembers &heap)
    {
        if (std::find(heaps_.begin(), heaps_.end(), &heap) == heaps_.end())
            heaps_.push_back(&heap);
    }

    bool useMember(uint64_t va, uint64_t bytes, bool write, bool hostWritten);
    void useStorage(uint64_t va, uint64_t bytes, bool write, bool hostWritten, const ArenaStorage *arena);
    // `noted`: a write the command buffer's later reads must see (bufferWrites_); false where the GPU may write the
    // memory but no program of the draw does
    void use(const Buffer &buffer, bool write, bool noted = true);
    void use(const Texture &texture, bool write);

    uint32_t draws = 0, passes = 0, blits = 0, bands = 0;
    uint32_t stamp = 0;               // its GPU time's slot (stamps.h), for Device::gpuNanoseconds once it completed
    uint64_t serial = 0;              // its begin()'s, unique in the device's life
    std::vector<DrawTiming> timings;  // its measured draws, which its commit hands the device (Device::learnCompleted)
    bool loopsLaid = false;           // a predicated loop's copies: the GPU writes the push ring

private:
    friend class Device;
    friend struct DeviceHarness;
    std::vector<TegraGM20BAbiEventUse> sharedWaits_, sharedSignals_;
    std::vector<uint32_t> sharedReleases_;
    friend class RenderEncoder;
    friend class BlitEncoder;
    friend class ComputeEncoder;
    Device *dev_ = nullptr;
    Predication predication_;
    LoopCopies loops_;  // a unit's loops, laid across its compute encoders
    PushBatches segments_;
    BatchWork batchWork_;
    uint64_t batchLimit() const;
    DirtyRanges dirty_;
    std::vector<Texture *> backings_;                   // memoryless backings it uses
    std::vector<const HeapMembers *> heaps_;            // useHeap's, each kept alive by the facade's command buffer
    void nameHeaps(std::unique_lock<gm20b::Mutex> &l);  // lock_ held
    SurfaceList surfaces_;
    ZcullPlanes zcullPlanes_;  // the depths' regions this command buffer's passes stored (zcull_planes.h)
    std::vector<SetUse> memberUses_;
    uint32_t memberRemovals_ = 0;
    bool memberHostWritten_ = false;
    bool arenaKindInvalidate_ = false;
    bool pendingKindSwitch_ = false;
    uint32_t kindSwitchGen_ = 0;
    uint32_t encodersOpened_ = 0;
    BlitEncoder *activeBlit_ = nullptr;
    bool inRender_ = false;
    bool passBufferWrites_ = false;
    bool prevBufferWrites_ = false;
    bool tiledCacheDirty_ = false;
    std::map<uint64_t, KindRanges> cbArenaKinds_;
    std::map<uint64_t, uint64_t> cbCrossSeen_;
    bool hasEncodedWork() const;
    void requestKindSwitch();

    bool takeKindSwitch()
    {
        const bool pending = pendingKindSwitch_;
        pendingKindSwitch_ = false;
        return pending;
    }

    void noteArenaKind(const ArenaStorage *arena);
    UseMemo named_;  // (use_memo.h)
    JobVisibility visibility_ = JobVisibility::Legacy;
    bool externalWrites_ = false;
    bool volatileWrites_ = false;
    EventPublication publication_;
    BarrierEngine lastEngine_ = BarrierEngine::None;  // the engine of the work encoded last (barrier_forms.h)
    PassChain chain_;                                 // the passes since the last drain (pass_hazard.h)

    // the forms a probe forced (probe_forms.h), else the rules'
    BarrierForms barrierForms() const
    {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        return forms_;
#else
        return {};
#endif
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool forceWholePrograms_ = false, inlineStates_ = false;
    SplitProbe splitProbe_ = SplitProbe::Twin;
    bool splitForced_ = false;
    bool splitCapturing_ = false;  // the device's capture was armed at begin(): its draws may take a record
    std::vector<SplitCaptureRecord> splitCaptures_;

    // the encode's per-draw and per-bind counts, kept plainly and added to the device's at the next begin(): an atomic
    // add per draw for each was a measurable share of a debug build's draw
    struct DrawCounts {
        uint64_t uploadBytes = 0, segmentBinds = 0, segmentWords = 0, segmentSkips = 0, segmentCopies = 0,
                 blendKillBinds = 0, interlockDraws = 0, interlocked = 0, interlockedBlends = 0, twinRule = 0,
                 rasterOrderedDraws = 0;
        uint64_t splitTwins = 0, splitWhole[size_t(SplitWhole::Count)] = {};
    } counts_;

    void foldCounts();
    ProbeForms forms_;
    ProbeTaken taken_;
    // passes it opened with PIXEL_SHADER_BARRIER, added to Stats::passPixelOpenings at its commit (an abandoned encode
    // ahead counts none)
    uint64_t pixelOpenings_ = 0;
#endif
#if SWITCH_METAL_DIAGNOSTICS
    Texture glassMemImages_[kGlassMemItems];               // the image each of the probe's rows is read from
    const Texture *glassMemSources_[kGlassMemItems] = {};  // and the texture a draw bound (alive until completion)
    int glassMemSlot_ = -1;                                // its ring slot once its reads are encoded
    void glassMemTail();
    TimedBuffer *drawTimes_ = nullptr;  // its slot in a frame-weights capture (draw_capture.h); null: none
#endif
    std::map<const Texture *, const Texture *> nativeReads_;
    std::vector<NativeSurface *> nativeSurfaces_;
    std::vector<const Import *> pitchImports_;
    GeometryBudget geometryBudget_;
    GeometrySnapshotPool geometryPool_;
    RenderEncoder render_;
    BlitEncoder blit_;
    uint64_t pushStart_ = 0, uploadStart_ = 0;
    uint32_t rootPush_[32] = {};
    uint32_t tiledCacheSize_ = kTiledCacheDefaultSize;
    uint32_t restart_ = 0;                  // SET_DA_PRIMITIVE_RESTART_INDEX as the GPU will hold it
    uint32_t rtLayer_ = ~0u;                // and SET_RT_LAYER (~0: not known)
    uint32_t alphaHeld_ = ~0u;              // and SET_ANTI_ALIAS_ALPHA_CONTROL (~0: not known)
    uint32_t sampleMaskHeld_ = ~0u;         // and SET_SAMPLE_MASK_X0_Y0..X1_Y1 (~0: not known)
    DepthStencilRegs ds_;                   // and its depth/stencil registers
    const Pipeline *extrasLive_ = nullptr;  // and the state whose extras it holds (Pipeline::restore)
    HeldSegment heldSegment_;               // and the state segment its registers hold
    uint32_t segmentBinds_ = 0;             // its own (segmentBindBudget)
    bool drawn_ = false;  // a draw was encoded (NVK resets the restart index at a command buffer's first)
    // the prologue's INVALIDATE_SHADER_CACHES_NO_WFI data word, which commit gives the INSTRUCTION bit when programs were placed
    // in the heap since the last submission that invalidated the instruction cache
    uint32_t *shaderInvalidate_ = nullptr;
    uint32_t prologueDwords_ = 0;
    // the split draws' prologues (RenderEncoder::launchSplit), launched in one compute segment after the submission prologue
    std::vector<SplitLaunch> splits_;
    std::vector<SplitBatchRecord> splitRecords_;
    SplitBatchPlanner splitBatches_;
    SplitReuse splitReuse_;
    // GPU work encoded before may have written a buffer: a prologue, run ahead of it, reads only what the CPU wrote
    BufferWrites bufferWrites_;
    void segment(uint32_t *from, uint32_t *to);
    // An encoder's words from `start` as segments of the unit's open loops (loop_copies.h), or plain ones
    LoopStream loopStream(Push &p, uint32_t *&start);
    void launchPrologues();
};

}  // namespace gm20b
