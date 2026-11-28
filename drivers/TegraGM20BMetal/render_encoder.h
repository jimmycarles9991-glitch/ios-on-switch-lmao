// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>
#include "alpha_control.h"
#include "attachment_order.h"
#include "attachment_view.h"
#include "band_rates.h"
#include "barrier_forms.h"
#include "bounds.h"
#include "client_errors.h"
#include "counters.h"
#include "depth_bias.h"
#include "depth_stencil_state.h"
#include "descriptors.h"
#include "draw_bands.h"
#include "draw_refusal.h"
#include "draw_times.h"
#include "fetch_order.h"
#include "fetch_route.h"
#include "geometry.h"
#include "icb.h"
#include "mixed_samples.h"
#include "mme.h"
#include "pack.h"
#include "pass_desc.h"
#include "pass_hazard.h"
#include "pipeline.h"
#include "push.h"
#include "region.h"
#include "resources.h"
#include "rotation.h"
#include "set_entries.h"
#include "slot_mask.h"
#include "span_marks.h"
#include "spill_table.h"
#include "split_cost.h"
#include "stage_textures.h"
#include "state_segment.h"
#include "stats.h"
#include "storage_form.h"
#include "viewports.h"
#include "visibility.h"
#include "zcull.h"
#include "zeta.h"

namespace gm20b {

class CommandBuffer;

// Persistent tables and heap-backed containers kept across passes by RenderEncoder::remake().
struct RenderEncoderKept {
    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;  // bytes from va (setBytes: its length)
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
        const uint8_t *cpu = nullptr;  // the bytes at va, where the CPU maps them
        uint64_t resourceVa = 0;
        // setBytes: the memory holding it was named, read, for the command buffer's jobs when it was bound (its
        // allocation, or the upload ring commit names), so no draw that only reads it names it again
        bool named = false;
    };

    using Body = PushBody;

    struct Clear {
        uint32_t target, x0, y0, x1, y1;
        const float *value;
        bool own = false;  // cleared by a pass of its own before this one (a Clear load reaching past the render area)
    };

    struct PassView {
        uint32_t ownTic;
        AttachmentView view;
        uint32_t tic;
    };

    struct Touched {
        uint32_t buffers[3] = {};
        StageTextureMask textures[2] = {};
        uint16_t samplers[2] = {};
        uint32_t strides = 0;
    };

    Bound bufs_[3][31]{};
    const Texture *textures_[2][kStageTextures]{};  // vertex, fragment (handleStage)
    const Sampler *samplers_[2][kStageSamplers]{};
    Touched touched_;
    uint32_t vertexStrides_[31] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX};
    PreparedDescriptors descriptors_;
    SpillUpload spillUploads_[2];  // each stage's argument table as the encoder uploaded it last (spill_table.h)
    WideDescriptors wideDescriptors_;
    DescriptorBinding descriptorBinding_;
    ViewportArray views_;
    VisibilityQueries visibility_;
    WrittenRegion written_;
    GeometryCache geometryCache_;
    PassUses planned_;
    std::array<PassView, 8> passViews_{};
    std::array<counters::Stamp, 8> startStamps_{};
    std::vector<Body> bodies_;
    SplitPassPlan splitPlan_;
    std::vector<std::array<uint32_t, 4>> rects_;
    std::vector<Clear> initialClears_;
    std::vector<bool> tiles_;
};

// Trivially-destructible per-pass state reset in one placement-new by RenderEncoder::remake().
struct RenderEncoderPass {
    static constexpr uint32_t kAmplificationMaps = SWITCH_GM20B_AMP_MAX;

    struct Tessellation {
        RenderEncoderKept::Bound buffers[TessBufferCount];
        uint32_t instanceStride = 0, scaleBits = 0x3f800000, indexed = 0;
        bool scaleValid = true;
    };

    struct MovedBindings {
        uint32_t buffers[3] = {};
        StageTextureMask textures[2] = {};  // vertex, fragment (handleStage)
        uint16_t samplers[2] = {};
        bool all = true;
    };

    struct Raster {
        uint8_t cull = 0, winding = 0, fill = 0;
        bool bias = true;                                         // polygon offset on: a triangle topology's
        uint32_t biasConstant = 0, biasSlope = 0, biasClamp = 0;  // float bits
        enum class Units : uint8_t {
            Unknown,
            Format,
            Unorm24
        } units = Units::Unknown;
    };

    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    PassDesc pass_;
    Push p_;
    uint32_t reachStep_ = 0, reachW_ = 0, reachH_ = 0;
    uint32_t reachScissor_[4] = {};  // a reach draw's Metal scissor and viewport, each bounding the other
    double reachViewport_[4] = {};
    const char *reachWhy_ = nullptr;
    const Pipeline *reachTwin_ = nullptr;  // the twin takeReachTwin set
    bool reachCover_ = false;
    const Pipeline *extrasAtBegin_ = nullptr;  // set back by the pass's prologue, before its clears
    uint32_t *bodyStart_ = nullptr;
    uint8_t startStampCount_ = 0;
    bool indirectGeometry_ = false;
    mme::DrawParams params_;  // what the channel holds: the context's zeros at a pass's start, put back at its end
    Tessellation tess_;
    bool tessWordsDirty_ = true;
    uint32_t amplifications_ = 1, amplification_ = 0;
    uint32_t amplificationViewports_[kAmplificationMaps] = {}, amplificationLayers_[kAmplificationMaps] = {};
    bool amplifying_ = false, amplificationDirty_ = true;
    bool amplificationScratchDirty_ = true;
    mme::AmplificationWords heldAmplification_ = {~0u};
    const Pipeline *prepared_ = nullptr;  // what the last prepare() drew with, and its set 0's address
    uint64_t preparedSet_ = 0;
    uint32_t passDraws_ = 0;
    DrawTiming candidate_;  // the pass's first draw, measured at the pass's end when it stays the only one
    Pipeline *pipeline_ = nullptr, *emitted_ = nullptr;
    const Pipeline *compatible_ = nullptr;
    uint32_t compatibleSampleMasks_[4]{}, compatibleRasterMask_ = kSampleMaskAll;
    bool skipDraw_ = false;
    bool residualsPushed_ = false;  // the last draw's push words held SlotBindOffset residuals (RenderEncoder::prepare)
    bool spillDirty_ = true;
    uint64_t bindingRevision_ = 0;
    MovedBindings moved_;
    uint64_t streamVa_[kPackVertexStreams] = {}, streamLimit_[kPackVertexStreams] = {};
    uint32_t streamFormat_[kPackVertexStreams] = {};
    uint32_t streamFormatKnown_ = 0;
    uint64_t ibVa_ = 0, ibEnd_ = 0;
    uint32_t ibSize_ = 0;
    uint64_t rootTableVa_[2] = {};
    uint32_t rootTableSlots_[2] = {}, rootBufDirty_[2] = {~0u, ~0u};
    bool rootTableWrites_[2] = {};
    const Pipeline *factsPipeline_ = nullptr;
    uint64_t factsRevision_ = 0;
    uint32_t facts_ = 0;
    const Pipeline *lastVariantInput_ = nullptr;
    const VariantTable *lastVariantTable_ = nullptr;
    uint32_t scissor_[2] = {}, viewport_[8] = {};
    float depthRange_[2] = {0, 1};
    bool scissorDirty_ = true, viewportDirty_ = true;
    DepthStencilDesc ds_;
    const DepthStencilState *dsState_ = &DepthStencilState::metalDefault();
    uint32_t stencilRef_[2] = {};
    bool dsDirty_ = true;
    ZcullPass zcull_;  // zcull.h
    const Pipeline *zcullProgram_ = nullptr;  // the state zcull_ last saw
    Texture *shaderDepth_ = nullptr, *shaderStencil_ = nullptr, *shaderStencilTemp_ = nullptr;
    uint8_t shaderTestsActive_ = 0;
    bool shadowStencil_ = false;  // the draws are a shader-side stencil's (shader_test_push.h shadowStencil)
    SeparateZeta separate_;
    Texture *separateTemp_ = nullptr;
    const Texture *zetaShadowOf_ = nullptr;
#if SWITCH_METAL_DIAGNOSTICS
    bool zetaShadowCompressed_ = false;  // Device::probeZetaShadow's: zetaShadowOf_ is copied without its decompress
#endif
    DepthBiasFactors depthBias_;
    Raster raster_, rasterHeld_;
    bool depthClamp_ = false;
    bool clipDirty_ = true;
    uint32_t clipHeld_ = 0;  // the channel's SET_VIEWPORT_CLIP_CONTROL since the encoder's first bind
    uint32_t blendColor_[4] = {};  // Metal's default, (0, 0, 0, 0), until the encoder sets one
    bool blendColorDirty_ = true;
    uint32_t topology_ = ~0u, restart_ = ~0u;
    uint32_t alphaWanted_ = 0;  // the bound Metal state's alpha to coverage and alpha to one (alpha_control.h)
    uint32_t samplePasses_ = 1;
    uint32_t tirControlHeld_ = ~0u;  // SET_TIR_CONTROL since the pass began (mixed_samples.h), ~0: not written
    AttachmentWrites attachmentWrites_;
    bool passShaderWrites_ = false;
    bool passWritesMemory_ = false;
    bool memoryless_[8] = {};
    bool hiddenZeta_ = false;  // pass_.depthStencil.texture is PassDesc::hiddenZeta's backing: the pass declares none
    const Texture *shadowOf_[8] =
        {};  // a shadow target's texture, where pass_ holds its pool image (CommandBuffer::render)
    bool called_ = false, drew_ = false, whole_ = false;
    bool mayFetch_ = true;
    bool trackRects_ = false;
    bool passHasVolatileColor_ = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint8_t probeFetchRung_ = 0;
    uint8_t fetchBarrierMode_ = 0;
#endif
#if SWITCH_METAL_DIAGNOSTICS
    uint32_t probeNvn_ = 0;
    uint32_t probeZcullKeep_ = 0;
#endif
    uint32_t readBits_ = 0, areaW_ = 0, areaH_ = 0;
    std::array<uint8_t, 8> ropLayers_ = {};
    bool narrowed_ = false;
    uint64_t drawPixels_ = 0;
    PixelBox drawBox_;
    uint8_t samples_ = 1;        // its attachments' samples a pixel
    uint8_t rasterSamples_ = 1;  // a mixed pass's (mixed_samples.h): its zeta's samples, which it rasterises at
#if SWITCH_METAL_DIAGNOSTICS
    uint32_t tirProbe_ = 0;  // a mixed pass's probe row (Device::probeTir), taken at its begin
#if SWITCH_METAL_PROF
    uint32_t prologueMark_ = SpanMarks::kNone;  // span_marks.h: its prologue's mark, taken ahead of its draws'
#endif
    uint32_t betweenTime_ = kNoMark, prologueTime_ = kNoMark;
    uint16_t timedPass_ = kNoPass;
#endif
    double vp_[4] = {};
    uint32_t sc_[4] = {};
    PixelBox reached_{UINT32_MAX, UINT32_MAX, 0, 0};  // the box of rects_
    uint8_t covered_ = 0;
    bool vertexSamples_ = false;  // a texture bound to the vertex stage
    const uint8_t *drawIndices_ = nullptr;
    bool drawIndexCpuSafe_ = false;
    uint32_t drawIndexSize_ = 0, drawIndexCount_ = 0;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool anyWritten_ = false;
    uint8_t ropWritten_ = 0, storageWritten_ = 0;
#endif
    bool clearsSealed_ = false;
    uint8_t passViewCount_ = 0;
    Rotation
        rot_;  // the pass's turned plane (rotation.h): its frame is what areaW_, areaH_ and the rectangles count in
};

class RenderEncoder : private RenderEncoderKept, private RenderEncoderPass {
public:
    RenderEncoder() = default;
    // Made again where it is for a new pass: resets only RenderEncoderPass and touched/active slots in RenderEncoderKept.
    void remake();

    void setPipeline(Pipeline *p);

    // The bound Metal state's alpha to coverage and alpha to one (alpha_control.h alphaControl), after setPipeline
    void setAlphaControl(uint32_t control)
    {
        alphaWanted_ = control;
    }

    void setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length);  // stage 1 vertex, 2 fragment
    // the same for bytes already written at `va` (256-byte aligned, zero to the next 256 bytes; `cpu` the CPU's view)
    void setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu,
                    const Buffer *allocation = nullptr);

    void setNamedBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu,
                         uint64_t resourceVa)
    {
        if (slot >= 31 || stage < 1 || stage > 2)
            return;
        Bound &old = bufs_[stage][slot];
        if (old.va == va && old.size == length && !old.buffer && old.cpu == cpu && old.resourceVa == resourceVa)
            return;
        old = {va, length, nullptr, 0, cpu, resourceVa, true};
        bufferMoved(stage, slot);
    }

    void setBuffer(int stage, uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(int stage, uint32_t slot, uint64_t offset);

    // A vertex buffer slot's bind stride (UINT32_MAX: none, MTLAttributeStrideStatic), which a state of a dynamic stride
    // reads (amplification.cpp)
    void setVertexStride(uint32_t slot, uint32_t stride)
    {
        if (slot < 31 && (stride != UINT32_MAX || (touched_.strides >> slot & 1))) {
            vertexStrides_[slot] = stride;
            touched_.strides |= 1u << slot;
        }
    }

    // setVertexAmplificationCount:viewMappings: (amplification.cpp): each draw after draws `count` amplifications, one
    // after another, amplification a through its mapping's viewport and render target offsets (0 past the mappings)
    using RenderEncoderPass::kAmplificationMaps;
    void setAmplification(uint32_t count, const uint32_t (&viewports)[kAmplificationMaps],
                          const uint32_t (&layers)[kAmplificationMaps]);

    // stage 1 vertex, 2 fragment; the one-argument forms bind the fragment stage
    void setTexture(int stage, uint32_t slot, const Texture *t);

    // what TegraGM20BMetalEncode found this pass reads in its recording, before end()
    void plan(const PassUses &uses)
    {
        planned_.copyFrom(uses);
    }

    bool mayFetch() const
    {
        return mayFetch_;
    }

    void setTexture(uint32_t slot, const Texture *t)
    {
        setTexture(2, slot, t);
    }

    // what the pass renders color `j` into (a memoryless attachment: its backing) and its render area, kept after end()
    const Texture *target(uint32_t j) const
    {
        return j < 8 ? pass_.color[j].texture : nullptr;
    }

    const Texture *depthTarget() const
    {
        return pass_.depthStencil.texture;
    }

    bool turned() const
    {
        return rot_.turned();
    }

    uint32_t areaWidth() const
    {
        return areaW_;
    }

    uint32_t areaHeight() const
    {
        return areaH_;
    }

    void setSampler(int stage, uint32_t slot, const Sampler *s)
    {
        const uint32_t at = stage == 1 ? 0 : 1;
        if (slot < kStageSamplers && samplers_[at][slot] != s) {
            samplers_[at][slot] = s;
            ++bindingRevision_;
            moved_.samplers[at] |= uint16_t(1u << slot);
            touched_.samplers[at] |= uint16_t(1u << slot);
        }
    }

    void setSampler(uint32_t slot, const Sampler *s)
    {
        setSampler(2, slot, s);
    }

    // Materializing a bound texture or sampler header must invalidate this encoder's descriptors.
    void invalidateDescriptors()
    {
        ++bindingRevision_;
        moved_.all = true;
        rootTableVa_[0] = rootTableVa_[1] = 0;
        rootBufDirty_[0] = rootBufDirty_[1] = ~0u;
    }

    void setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void setBlendColor(const float rgba[4]);
    void setDepthStencil(const DepthStencilState *s);  // null: Metal's default state
    void setStencilReference(uint32_t front, uint32_t back);

    // MTLCullMode, MTLWinding and MTLTriangleFillMode, written as NVK's rasterizer registers at the next draw
    void setCullMode(uint8_t mode)
    {
        raster_.cull = mode;
    }

    void setWinding(uint8_t winding)
    {
        raster_.winding = winding;
    }

    void setFillMode(uint8_t mode)
    {
        raster_.fill = mode;
    }

    // MTLDepthClipModeClamp: the state's clip control as NVK sets depth clamp without depth clip, from the next draw
    void setDepthClamp(bool clamp)
    {
        if (depthClamp_ != clamp) {
            depthClamp_ = clamp;
            clipDirty_ = true;
        }
    }

    // setDepthBias:slopeScale:clamp:, infinite and NaN factors as Metal's results (depth_bias.h)
    void setDepthBias(float constant, float slope, float clamp);

    void setViewport(double x, double y, double w, double h, double znear, double zfar);
    // setViewports:count: and setScissorRects:count: (x, y, w, h, znear, zfar each; x, y, w, h each), `count` up to
    // kMaxViewports: index 0 as setViewport and setScissor take it, the others at the next draw
    void setViewports(const double (*viewports)[6], uint32_t count);
    void setScissors(const uint32_t (*rects)[4], uint32_t count);
    void barrier();  // memoryBarrier...: everything before is written and visible to what follows
    // false: this draw cannot be drawn exactly as Metal draws it (*why says what): its command buffer fails
    // (TegraGM20BMetalDrawErrors.mm)
    bool draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
              const char **why);
    // drawIndexedPrimitives: `count` indices of `indexSize` bytes (2 or 4) at `offset` in `ib`, each `baseVertex` on
    bool drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib, uint64_t offset,
                     uint32_t instances, int32_t baseVertex, uint32_t baseInstance, const char **why);
    bool drawIndirect(uint32_t primitive, const Buffer *arguments, uint64_t offset, const char **why);
    bool drawIndexedIndirect(uint32_t primitive, uint32_t indexSize, const Buffer *indices, uint64_t indexOffset,
                             const Buffer *arguments, uint64_t argumentOffset, const char **why);
    // A tessellation state's draws (tessellation.cpp): the encoder's factor buffer (null: none, whose patches are
    // culled) and scale, and patches of `controlPoints` each, from the draw's patch and control-point index buffers (null:
    // none)
    void setTessellationFactors(const Buffer *buffer, uint64_t offset, uint64_t instanceStride);
    void setTessellationFactorScale(float scale);
    bool drawPatches(uint32_t controlPoints, uint32_t start, uint32_t count, uint32_t instances, uint32_t baseInstance,
                     const Buffer *patchIndices, uint64_t patchIndexOffset, const Buffer *pointIndices,
                     uint64_t pointIndexOffset, const char **why);
    bool drawPatchesIndirect(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                             const Buffer *pointIndices, uint64_t pointIndexOffset, const Buffer *arguments,
                             uint64_t offset, const char **why);

    // An indirect command buffer's commands (icb.h) [first, first + count) of `slots`, `slotWords` each, drawn with the
    // state they inherit (`primitive` one of theirs: icb::kPatchPrimitive where they are patches; `indexed`: some draw
    // indices), or with `range` the commands of the MTLIndirectCommandBufferExecutionRange the GPU reads there (`count` the
    // buffer's), once per amplification of the encoder's. `buffers`: what the commands read (index buffers, patch commands'
    // factors and indices). `written`: the GPU wrote some of the words, which the channel fetches after those writes land
    struct IcbExecute {
        const Buffer *slots;
        uint32_t first, count, slotWords;
        const Buffer *range;
        uint64_t rangeOffset;
        uint32_t primitive;
        bool indexed, written;
        const Buffer *const *buffers;
        size_t bufferCount;
    };

    bool executeSlots(const IcbExecute &x, const char **why);

    // An indirect command of patches as the CPU encoded it (Metal's drawPatches: and drawIndexedPatches: of a command,
    // `scale` its factor scale, 1 where the call names none), and its words (icb.h icb::patches) as tessellation.cpp
    // binds a draw's: false where Metal's validation stops it (*why)
    struct IcbPatches {
        uint32_t first, count, instances, baseInstance;
        const Buffer *factors;
        uint64_t factorOffset, factorStride;
        float scale;
        const Buffer *patchIndices;
        uint64_t patchIndexOffset;
        const Buffer *pointIndices;
        uint64_t pointIndexOffset;
    };

    static bool icbPatchDraw(const Device &dev, const IcbPatches &in, icb::PatchDraw &out, const char **why);
    // A counter sample where the pass starts: in its prologue, after the barrier to the work before
    void timestampAtStart(uint64_t va, counters::Point point);
    void end();
#if SWITCH_METAL_DIAGNOSTICS
    // nvn_probes.cpp
    void nvnPassEnd();
    void nvnKnob(Push &p);
    void nvnZcullFormats(Push &p, const DepthStencilAttachment &zs, uint32_t &regionFormat, uint32_t &zFormat);
#endif

    void continuePass(uint8_t stores, uint8_t discards = 0);
    // A pass whose draws bind colours `storage` as storage images, `arrayed` of them as 2D arrays, fused into this one:
    // false where this pass's begin laid those colours out otherwise (a colour bound as storage takes an uncompressed
    // backing where a storage form cannot view compressed memory, one bound as a 2D array a block-linear target), else
    // they become this pass's storage colours, which passUses reads at its end
    bool takeStorage(uint8_t storage, uint8_t arrayed);
    // setVisibilityResultMode:offset: (visibility.h) in a pass that resets or, `accumulate`, adds to its results:
    // false where the driver cannot take it
    bool setVisibility(uint32_t mode, const Buffer *results, uint64_t offset, bool accumulate);
    // the Metal encoder's results to their offsets, at its end (a fused pass's too)
    void finishVisibility();
    // reach.h: the pass a reach target's, at `step` frame pixels a texel, of a frame `width` x `height`: viewports and
    // scissors in the target's units, no depth/stencil state; and the state set last, its reach twin for the draws after
    void beginReach(uint32_t step, uint32_t width, uint32_t height);
    void takeReachTwin();

    // the state's vertex program writes memory, so its draws are not drawn again: the reach pass covers their viewports
    bool reachCovers() const
    {
        return reachCover_;
    }

    const char *reachRefusal() const
    {
        return reachWhy_;
    }

    void beginSplitPlan()
    {
        splitPlan_.clear();
    }

    void addSplitCandidates(Pipeline *pipeline, uint32_t draws, uint64_t pixels)
    {
        if (pipeline)
            if (const auto *source = __atomic_load_n(&pipeline->splitSource, __ATOMIC_ACQUIRE))
                splitPlan_.add(pipeline, source->rec, draws, pixels);
    }

    void finishSplitPlan()
    {
        splitPlan_.finish();
    }

private:
    friend class CommandBuffer;
#ifdef SWITCH_METAL_HARNESS_MARKS
    friend struct EncoderHarness;
    friend struct DeviceHarness;
#endif

    void clearTouchedBindings();
    bool indirect(uint32_t primitive, bool indexed, const Buffer *arguments, uint64_t offset, const char **why);

    // A draw under vertex amplification is one amplify macro call (amplification.cpp), but where runs of its
    // primitives are drawn apart with barriers between: that draw is drawn once per amplification
    bool amplifies() const
    {
        return amplifications_ > 1 && !amplifying_;
    }

    // Each SlotGuestBase hole of `pl` (Pipeline::guestBases) given its slot's bound address in `want`; the push words it
    // wrote
    uint32_t guestBaseWords(const Pipeline &pl, uint32_t (&want)[32]);
    // Whether a hole's slot was bound again since the last draw's push words (moved_)
    bool guestSlotMoved(const Pipeline &pl) const;

    // How a draw of `pl` keeps a mixed pass's sample rule (mixed_samples.h), whole where HW Test's probe row asks
    // (kTirProbeWhole)
    MixedDraw mixedForm(const Pipeline &pl) const
    {
        const MixedDraw form =
            mixedDraw(pl, ds_, pass_.depthStencil, samples_, rasterSamples_, (alphaWanted_ & kAlphaToCoverage) != 0);
#if SWITCH_METAL_DIAGNOSTICS
        if (splitsSamples(form) && (tirProbe_ & kTirProbeWhole))
            return MixedDraw::Whole;
#endif
        return form;
    }

    bool runsPerAmplification(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed);
    void amplificationScratch(Push &p, const Pipeline &pl);
    void amplifyCall(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                     uint32_t root, uint32_t baseInstance);
    // the root words an amplify call leaves: the last amplification's id and offsets, the count
    void heldAmplifyWords(const Pipeline &pl);

    // `draw` once per amplification, its id and offsets in the state's words
    template <typename Draw> bool amplified(const Draw &draw)
    {
        amplifying_ = true;
        bool drawn = true;
        for (uint32_t a = 0; drawn && a < amplifications_; a++) {
            amplification_ = a;
            amplificationDirty_ = true;
            drawn = draw();
        }
        amplifying_ = false;
        amplification_ = 0;
        amplificationDirty_ = true;
        return drawn;
    }

    // What of a draw's amplification and bind strides the state does not take (null: it takes them), its refusal in
    // `refusal`
    const char *drawnWordsRefusal(const PackPipeline &r, Refusal &refusal) const;
    // A vertex stream of the state's takes its bind's stride, and that stride is past the stream's 12-bit field
    bool bindStridePastStream(const PackPipeline &r) const;
    void drawnWords(const Pipeline &pl, uint32_t (&want)[32]);
    // The state's vertex streams at their binds (vertex_streams.cpp): a stream's location and limit where its bind
    // moved, its format where its stride is its bind's, and its vertices [vFirst, vFirst + count) cleaned
    void bindStreams(Push &p, const Pipeline &pl, bool indexed, uint32_t vFirst, uint32_t count);
    void bindStream(Push &p, uint32_t stream, uint8_t slot, uint32_t stride, bool bindStride, bool indexed,
                    uint32_t vFirst, uint32_t count);

    uint32_t vertexStride(uint32_t slot) const
    {
        return slot < 31 && vertexStrides_[slot] != UINT32_MAX ? vertexStrides_[slot] : 0;
    }

    static Bound boundFrom(const Buffer *buffer, uint64_t offset);
    bool bindPatches(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                     const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why);
    Bound tessBuffer(uint8_t which) const;
    void controlWords(const PackPipeline &r, uint32_t (&want)[32]) const;
    // An execute of patch commands: the state prepare() drew with as their layout's scratch words; after them, the root
    // table's control words set back to what the encoder holds and set 0 built again, as the commands loaded both
    bool icbPatchLayout(const char **why);
    void icbPatchesDone();
    // The amplification `a`'s id into the root table of the state prepare() drew with, before each of an execute's passes
    void icbAmplification(uint32_t a);
    void reserve();
    void finishDraw(uint32_t vertices, uint32_t instances, bool indirect = false);
    void cutBatch();
    // the prepared draw's words, whole or in scissor bands (draw_bands.h)
    void emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                  uint32_t baseInstance);
    uint64_t drawUnits(uint32_t instances) const;
    BandGrid drawBands(uint32_t vertices, uint32_t instances, uint64_t units) const;
    void noteCandidate(uint64_t units, uint32_t bands);
#if SWITCH_METAL_DIAGNOSTICS
    void notePrice(uint64_t units, BandGrid grid, uint32_t instances) const;
#endif
    bool measurePass(bool drained);
    // whether the prepared draw of `pl` is drawn whole, never in bands: it amplifies, or may choose a viewport past 0 and so
    // its scissor, which bands do not narrow; and its words so, an amplify call where it amplifies
    bool drawnWhole(const Pipeline &pl) const;
    void wholeDraw(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                   uint32_t baseVertex, uint32_t baseInstance);
    void bindState(Push &p, const Pipeline &pl);
    bool measuringInline() const;
    bool bindsBySegment() const;

    bool skippedDraw()
    {
        return std::exchange(skipDraw_, false);
    }

    // `pl`'s argument tables whose entries moved (`moved`, bit k entry k; every one where `all`)
    void fillSpills(const Pipeline &pl, uint32_t moved, bool all, SetResiduals &prepared, uint32_t *set);
    bool writeSetEntry(Pipeline *pl, const PackEntry &e, uint32_t *set, SetResiduals &prepared, const char **why);
    bool anyEntryMoved(const SetEntries &entries) const;
    bool entryMoved(const PackEntry &e) const;

    void bufferMoved(int stage, uint32_t slot)
    {
        ++bindingRevision_;
        moved_.buffers[stage] |= 1u << slot;
        touched_.buffers[stage] |= 1u << slot;
        if (stage == 1 || stage == 2)
            rootBufDirty_[stage - 1] |= 1u << slot;
    }

    void textureMoved(uint32_t at, uint32_t slot)
    {
        ++bindingRevision_;
        addSlot(moved_.textures[at], slot);
        addSlot(touched_.textures[at], slot);
        vertexSamples_ |= at == 0 && textures_[at][slot];
    }

    uint32_t movedEntries(const SetEntries &entries) const;

    const Texture *boundTexture(const PackEntry &e) const
    {
        return e.slot < kStageTextures ? textures_[handleStage(e)][e.slot] : nullptr;
    }

    const Sampler *boundSampler(const PackEntry &e) const
    {
        return samplers_[handleStage(e)][e.sampler];
    }

    // the state a draw needs but its draw parameters, which its macro call writes (vertices [vFirst, vFirst + vCount) of
    // the stream are CPU-cleaned): the first vertex a draw of vertices starts at, or the base vertex an indexed one adds to
    // each index
    bool prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount, bool indexed,
                 const char **why);
    // `pl`'s split twin where the draw is large enough for it to save time and its prologue can read what the draw binds
    Pipeline *splitTwin(Pipeline *pl, WrittenRegion::Analysis &analysis) const;
    // the facts of a draw of `pl` its bindings decide, worked out where the bindings moved since the last draw, never as
    // one is bound
    uint32_t bindingFacts(const Pipeline &pl, const VariantTable *variants);
    // the twin's draw: its frontier bound and its prologue's launch recorded (`set`: the draw's set 0)
    void launchSplit(Push &p, const Pipeline &twin, const uint32_t *set);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    void captureSplit(const Pipeline &pl, WrittenRegion::Analysis &analysis, uint32_t primitive, uint32_t first,
                      uint32_t count);
#endif
    void emitViewports(Push &p);
    bool prepareShaderTests(const Pipeline &pipeline, const char **why);
    uint64_t rootTable(const PackPipeline &r, uint32_t k, bool writes);
    void copyShaderTests(uint8_t flags, bool toAttachment);
    void endShaderTests();
    void copySeparateZeta(Push &p, uint8_t aspects, bool toAttachment);
    void copyZetaShadow(Push &p, bool toAttachment);
    void writeRaster(Push &p, const Raster &want);
    // after the state's registers, which may set SET_DEPTH_BIAS_CONTROL
    void writeBiasUnits(Push &p, const Raster &want);
    // false: a depth clamp on a state whose registers set no clip control
    bool writeClipControl(Push &p, const Pipeline &pl, const Raster &raster, const char **why);
    void writeClipDepthRange(Push &p, const uint32_t range[2]);
    void unclampDepthRange(Push &p);
    Raster rasterFor(uint32_t primitive) const;
    // The hidden zeta: over the plane the colors' backings cover (once they are taken), of the pass's samples
    Texture hiddenZetaShape(const PassDesc &pass) const;
#if SWITCH_METAL_DIAGNOSTICS
#if SWITCH_METAL_PROF
    void markSpan(Push &p, uint32_t index, SpanKind kind);
#endif
    void timePassBegin();
    void timeDraw(uint32_t vertices, uint32_t instances, bool indirect);
    TimedState timedState(const Pipeline &drawn) const;
#endif
    const PassUses &passUses();
    PassForm passRule(BarrierEngine before, const PassUses &uses) const;

    // `pl`, its fetch-free form where its rung's rule holds, else its barrier twin where an attachment of this pass cannot
    // be viewed as one of its storage images or rule fetch.twin (twin_rule.h) takes the draw's shape
    Pipeline *formFor(Pipeline *pl, const FetchDraw &shape);
    Pipeline *fetchFreeFor(const Pipeline &pl, StorageRoute *route = nullptr);
    bool matrixConstants(const Pipeline &pl, const char **why);
    uint32_t colorTargets() const;
    StorageFallback storageFallback(const Pipeline &pl, const Pipeline *barrierTwin) const;
    bool orderedFetch(const Pipeline &pl, const FetchDraw &shape) const;
    void fetchDraw(uint32_t primitive, bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                   uint32_t baseInstance);
    void refuseFetch(const Pipeline &pl, FetchRefusal refusal, uint32_t primitive, uint32_t count, uint32_t instances,
                     const char **why);
    DrawGeometry geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count, const PixelBox &limit);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    void countRules(const Pipeline &pl);
#endif
    void fetchBarrier(Push &p);

    // the pass's clears into `out`; the tiles its draws reach in tiles_ (both keep the last pass's capacity)
    void clears(std::vector<Clear> &out, bool fused = false);
    bool drawnClear(uint32_t j, const float *value, std::vector<Clear> &out) const;

    uint32_t passViewTic(const Texture &t, uint32_t word0);
    uint32_t passViewTic(const Texture &owner, const AttachmentView &v, const char **why);
    // The header a draw's fetch of attachment `a` binds: the storage form's view of word 0 `storage`, a 2D array of the
    // pass's layers for a record of the layered form (`layeredRecord`), or (storage 0) the input-attachment form's, which
    // NVK fetches at (x, y, Layer) (nvk_nir_lower_descriptors.c lower_load_input_attachment_coord); 0: none (`why`)
    uint32_t fetchTic(const ColorAttachment &a, uint32_t storage, bool layeredRecord, const char **why);

    void emitSwizzle(Push &p, uint32_t word);

    Rect planeRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const
    {
        return rot_.rect({x0, y0, x1, y1});
    }

    Rect planeArea() const
    {
        return planeRect(0, 0, areaW_, areaH_);
    }
};

}  // namespace gm20b
