// SPDX-License-Identifier: MIT
#pragma once
#include "frame_cpu_counts.h"
#include "lock.h"
#include "word_wait.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>
#include "SwitchFrameTimeline.h"
#include "gpu.h"
#include "TegraGM20BMetalPredicates.h"
#include "finish_state.h"
#include "handler_order.h"
#include "predicate_records.h"
#include "submit_queue.h"
#include "event_timeline.h"
#include "retirement.h"
#include "variant.h"
#include "library_source.h"
#include "metallib.h"
#include "metallib_functions.h"
#include "render_variant.h"
#include "obtained.h"
#include "pipeline_fields.h"

namespace gm20b {
template <typename T> class ComputeKernelJob;
template <typename T> class ComputeKernelSet;
struct ComputeState;
}

@class SwitchMetalDevice, SwitchMetalCommandBuffer, SwitchMetalCounterSampleBuffer;
struct SurfaceWriteCount;
struct SwitchMetalPassCache;

struct SwitchMetalPassCacheDelete {
    void operator()(SwitchMetalPassCache *cache) const;
};

using SwitchMetalPassCachePtr = std::unique_ptr<SwitchMetalPassCache, SwitchMetalPassCacheDelete>;

// ---- the common base: the selectors this facade does not answer itself ----
inline uint64_t gm20bNewIdentity()
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

@interface SwitchMetalObject : NSObject {
@public
    // the device, which gRM holds for the life of the process (metal_device_open: one per process): no weak
    // reference, whose side-table lock every object's creation, dealloc and encoder begin took
    __unsafe_unretained id _device;
    // a texture's or buffer's number, never another object's in this process (gm20bNewIdentity); 0 otherwise
    uint64_t _identity;
}
@property (nonatomic, copy) NSString *label;
@end

@interface SwitchMetalDevice : NSObject  // the methods the device class gets (metal_device_open); no state of its own
- (void)gm20bCommit:(SwitchMetalCommandBuffer *)cb;
- (id<MTLTexture>)gm20bNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled;
@end

// ---- functions and libraries: the name and constants text the pack keys by ----
// The libraries the pack's states come from are frameworks' default.metallib (QuartzCore's, RenderBox's): a state of their
// functions is looked up by its key, which names the framework but QuartzCore's (metal_pipeline_key).
@interface SwitchMetalLibrary : SwitchMetalObject {
@public
    std::string _framework;  // "QuartzCore", "RenderBox"...; empty: not a framework's default library (never looked up)
    std::shared_ptr<gm20b::LibrarySource> _source;
    gm20b::MetallibFunctions _functions;
    std::map<std::string, gm20b::AirHash> _airHashes;  // each listed function's HASH tag, read with the library
}
@end
@interface SwitchMetalFunction : SwitchMetalObject {
@public
    std::string _constants;
    std::string _framework;
    uint8_t _argumentBuffers;  // whether it takes an argument buffer at a buffer index: 0 not asked yet, 1 no, 2 yes
    std::shared_ptr<gm20b::LibrarySource> _source;
    bool _specialized;             // made with constant values (even none)
    std::string _libraryName;      // the library function it was made from (its name unless specialized under another)
    SwitchMetalLibrary *_library;  // the library it was made of
    MTLFunctionConstantValues *_values;  // what it was specialized with
    uint32_t _type;                      // MTLFunctionType from the metallib; 0: none listed (a Core Image extern)
    MTLFunctionOptions _options;
    std::optional<gm20b::AirHash> _air;  // its library function's HASH tag; none where its library lists none
    uint64_t _stageHash;                 // its part of a state's fast key (TegraGM20BMetalPipelineKey.h stageHash)
    uint8_t _patchType;                  // its MTLPatchType, its TESS tag's
}
@property (nonatomic, copy) NSString *name;
@end

// ---- resources ----
// A heap of the device (TegraGM20BMetalHeap.mm): an arena its resources are placed in, or with none (a size or storage mode no arena
// takes) every resource made as the device makes it, counted against its size
@interface SwitchMetalHeap : SwitchMetalObject {
@public
    MTLHeapDescriptor *_descriptor;
    std::atomic<uint64_t> _used;  // what its resources made outside its arena hold
    uint32_t _arena;              // its arena + 1; 0: none
    std::atomic<int> _purgeable;  // the MTLPurgeableState asked last (0: never asked)
    // what a command buffer that uses it names (heap_members.h): its arena, and the resources it made outside it
    std::shared_ptr<gm20b::HeapMembers> _members;
}
@end

@interface SwitchMetalBuffer : SwitchMetalObject {
@public
    gm20b::Buffer _raw;
    MTLResourceOptions _options;
    std::atomic<bool> _gpuDirty;  // the GPU wrote it since the CPU last read it
    void (^_deallocator)(void *, NSUInteger);
    SwitchMetalHeap *_heap;
    uint64_t _heapBytes;  // what it counts against its heap outside the arena
    bool _aliasable;
    MTLPurgeableState _purgeable;
}
@end
@interface SwitchMetalBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row;
@end

typedef NS_ENUM(uint8_t, SwitchMetalTextureKind) {
    SwitchMetalTexturePool,
    SwitchMetalTextureSurface,
    SwitchMetalTextureBuffer,
    SwitchMetalTextureView,
    SwitchMetalTextureMemoryless
};

enum class SwitchMetalShareable : uint8_t {
    Unasked,
    No,
    Yes
};

@interface SwitchMetalTexture : SwitchMetalObject {
@public
    gm20b::Texture _raw;
    bool _gm20bOk;  // the driver can draw with it (else a command buffer that blits it fails)
    SwitchMetalTextureKind _kind;
    std::atomic<bool> _gpuWrites;   // its surface was declared GPU-written (surfaceGpuWrites)
    std::atomic<bool> _readAsData;  // a blit copied out of it or a kernel took it, as recorded (resolve_rule.h)
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _surface;
    SurfaceWriteCount *_surfaceWrites;  // its surface's writes (TegraGM20BMetalSurfaceWrites.mm), held while it lives
    NSUInteger _plane;
    // A plane whose offset or rows are off 16: Metal's GPU reads it from both rounded down to 16 (_raw's layout), its CPU
    // from the plane's own (m2v linear_plan.rs, measured), here; null: the CPU's rows are _raw's
    uint8_t *_cpuRows;
    size_t _cpuPitch;
    // a planar texture's image (4:2:0 converted, 4:4:4 codes) holds its surface at this seed (_imageCurrent): converted
    // again only once the surface's seed moves (r8a: a YouTube page's thumbnails converted again at every command buffer)
    uint32_t _imageSeed;
    uint64_t _imageNativeGeneration;
    uint64_t _imageWrites;  // and its surface's writes the seed does not show (surfaceWrites)
    bool _imageCurrent;
    SwitchMetalTexture *_parent;  // a view's
    // a view of part of its parent (newTextureViewWithPixelFormat:textureType:levels:slices:): those of the parent; the
    // view's type is its descriptor's
    NSRange _viewLevels, _viewSlices;
    bool _viewPart;
    // The parent chain keeps this owner alive; absolute subresources are prepared when a view is made.
    SwitchMetalTexture *__unsafe_unretained _levelsOwner;
    uint32_t _levelsBase, _slicesBase;
    SwitchMetalBuffer *_buffer;  // a texture over a buffer's
    NSUInteger _offset, _rowBytes;
    // A decoded format's texture that keeps its blocks (decoded.h keepsBlocks): the copy its uploads write and its
    // readbacks read, and its views that hold a form of their own (_ownForm: an image of their own of the blocks' other
    // decode, made from them), which every upload to the texture or a view of it decodes into in its own job. Only the
    // submit worker's moves change _forms (TegraGM20BMetalDecodedFamily.mm), and the encodes that read it run between them.
    SwitchMetalBuffer *_keptBlocks;
    std::vector<SwitchMetalTexture * __unsafe_unretained> _forms;
    bool _ownForm;
    std::atomic<bool> _gpuDirty;
    MTLPurgeableState _purgeable;
    uint8_t _pred;  // why the driver does not keep it (SwitchMetalPred; 0: it does)
    SwitchMetalHeap *_heap;
    uint64_t _heapBytes;  // as SwitchMetalBuffer's
    bool _aliasable;
    std::atomic<bool>
        _idNamed;  // its gpuResourceID was asked: the device's handle tables name it (TegraGM20BMetalResourceIds.mm)
    std::atomic<SwitchMetalShareable> _shareable;  // its surface's creation properties name a Metal device (read once)
    // Made plain for want of compressed memory (private_comp.h): the submit worker may move its memory
    // (compressedFromPlain), so allocatedSize answers this, its plain allocation's bytes; 0: it wants none
    // (gm20bWantCompressed registers it)
    uint64_t _movableBytes;
    __weak SwitchMetalTexture *_wanting;  // itself: the worker's look holds it only while it is not being deallocated
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::atomic<bool> _freshUnwritten;  // a fresh texture no command wrote yet (TegraGM20BMetalFresh.mm traceFreshUses)
#endif
#if SWITCH_METAL_DIAGNOSTICS
    std::atomic<uint32_t> _replaceCount;
#endif
}
@end

@interface SwitchMetalTexture (CpuCopies)
// Metal's CPU copies as they move this texture's own texels (TegraGM20BMetalTextureCopies.mm): what replaceRegion and getBytes do
// past an RGB10A8_2P_XR10 texture's colour words (cpuXrWords)
- (void)gm20bReplaceRegion:(MTLRegion)r
               mipmapLevel:(NSUInteger)level
                     slice:(NSUInteger)slice
                 withBytes:(const void *)bytes
               bytesPerRow:(NSUInteger)row
             bytesPerImage:(NSUInteger)image;
- (void)gm20bGetBytes:(void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
           fromRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice;
@end

@interface SwitchMetalSampler : SwitchMetalObject {
@public
    gm20b::Sampler _raw;
    MTLSamplerDescriptor *_descriptor;
    std::atomic<bool> _idNamed;
    gm20b::Sampler _normalized;  // a pixel-coordinate sampler's twin of normalized coordinates, what a heap holds of it
    NSMutableDictionary<NSValue *, id<MTLSamplerState>> *_clamped;  // its descriptor's with a bind's LOD clamps
}
@end

@interface SwitchMetalPipeline : SwitchMetalObject {
@public
    // null only until the runtime made it (completeLater: set once, released, from its queue): a state without one is
    // never handed out (gm20bFinishState)
    gm20b::Pipeline *_raw;
    bool _packable;  // its functions are one framework's default library's (looked up in the pack)
    // its record is the pack's, and so are its variants' (TegraGM20BMetalCubic.mm packedVariants); a record this process made is
    // not, and its variants are made as well
    bool _packed;
    bool _completable;  // its functions, fragment or none, are this device's (made at creation where the pack has none)
    bool _caTransform;
    // its alpha to coverage and alpha to one (alpha_control.h), which its key does not hold
    uint32_t _alphaControl;
    std::string _failure;  // why its creation failed (Completion::makeNow), as Metal's would
    // Completable states': what its key names (keyFieldsOf), its functions and their linked functions' segments, from which
    // its pack key's text and a descriptor its variants are made of come at their first ask (gm20bStateKey,
    // gm20bMadeFrom: TegraGM20BMetalStateKey.h)
    gm20b::PipelineFields _fields;
    SwitchMetalFunction *_vertexFunction, *_fragmentFunction;
    NSArray *_linkedLists[4];  // vertex functions, vertex private functions, fragment's, fragment private
    std::string _linked;
    std::once_flag _keyed, _described;
    std::string _key;
    MTLRenderPipelineDescriptor *_madeFrom;
    uint64_t _packKey;  // the hash the pack finds its records and reflection by (its key's, or its key and AIR's)
    gm20b::Mutex _variantLock;
    // every variant asked of it, by name, for its life: plans and encoders name each by address
    std::map<std::string, std::shared_ptr<gm20b::Variant<gm20b::Pipeline>>> _variants;
    gm20b::Obtained _obtained;  // debug builds: how its creation got its record
#if SWITCH_METAL_DIAGNOSTICS
    bool _caCatchAll;  // Core Animation's catch-all state (ca_catch_all.h), whose draws the rates line counts
#endif
}
@end

@interface SwitchMetalDepthStencil : SwitchMetalObject {
@public
    gm20b::DepthStencilState _raw;
}
@end

#include "TegraGM20BMetalPlan.h"

@interface SwitchMetalQueue : SwitchMetalObject {
@public
    dispatch_queue_t _completionQueue;
    dispatch_queue_t _handlers;  // its command buffers' scheduled handlers, in its order
    std::atomic<void *> _last;   // the command buffer enqueued last, retained until its completed handlers ran
    // the submit worker's: the kernel's bound on the writer promises of a command buffer held behind its held head
    gm20b::EventDeadline::Time _heldBound;
    uint32_t _unscheduled;
    uint32_t _gpuPriority, _backgroundGpuPriority;  // what it asked for (TegraGM20BMetalQueuePriority.mm)
}
@end

// What the raw encode reads of a render pass, taken from the descriptor's copy as its encoder is made: no Metal getter, no
// attachment object Metal makes on a first read, per encode. Objects unretained: the command buffer's owners hold them.
struct SwitchMetalPass {
    struct Color {
        __unsafe_unretained id texture;
        NSUInteger level;
        NSUInteger slice;  // its slice, a 3D texture's depth plane
        MTLLoadAction load;
        MTLStoreAction store;
        MTLClearColor clear;
        __unsafe_unretained id resolve;
        NSUInteger resolveLevel, resolveSlice;  // the level and slice (a 3D texture's depth plane) it resolves into
    } color[8];

    __unsafe_unretained id depth;
    NSUInteger depthSlice = 0;  // a 2D array's or cube's layer
    MTLLoadAction depthLoad, stencilLoad;

    // the descriptor's depth (0) and stencil (1) attachments as the encode reads them, the store as the encoder set it (a
    // resolve's, MultisampleResolve and StoreAndMultisampleResolve, as DontCare and Store)
    struct Zeta {
        __unsafe_unretained id texture;
        NSUInteger level, slice;
        MTLLoadAction load;
        MTLStoreAction store;
        bool resolve;
        uint32_t sourceTic = 0;
    } zeta[2];

    __unsafe_unretained id depthResolve;    // its depth resolve's texture, nil for none
    uint32_t depthFilter;                   // and MTLMultisampleDepthResolveFilter
    __unsafe_unretained id stencilResolve;  // its stencil resolve's texture, nil for none
    gm20b::StencilSample stencilPick;       // and the sample whose stencil it takes
    // the level and slice of the texture each of them resolves into
    NSUInteger depthResolveLevel = 0, depthResolveSlice = 0, stencilResolveLevel = 0, stencilResolveSlice = 0;
    double clearDepth;
    uint32_t clearStencil;
    NSUInteger width, height;
    uint32_t layers = 1;  // renderTargetArrayLength (0 and 1: one)
    gm20b::SamplePositions positions;
    uint8_t compressed = 0;        // extended-range colours on compressed memory at its making (private_comp.h)
    bool testsHiddenZeta = false;  // a depth/stencil state set in it tests depth or stencil (gm20b::testsHiddenZeta)
    // a draw in it tests depth in its shader (shader_tests.h): a Depth16Unorm attachment renders converted
    // (TegraGM20BMetalRangeForms.mm depth16Tested)
    bool shaderTestsDepth = false;
    bool xrPlaneBinds = false;  // it binds a two-plane RGB10A8_2P_XR10 texture, which a draw may store through
    bool continues =
        false;  // a parallel encoder's sub-encoder after its first: the pass before it goes on (parallel.h)
};

// A command buffer's setBytes data, written once where the GPU reads it: write-combined pool chunks the raw encode binds in place,
// each copy 256-byte aligned and zero to the next 256 bytes as an upload chunk is. The chunks go with the recording
// (giveRecording), so they outlive every submission that reads them.
struct SwitchMetalBytes {
    gm20b::GrowList<gm20b::Buffer> chunks;  // va 0: heap memory (the GPU pool had none; the command buffer fails)
    size_t at = 0;                          // the chunk being filled
    uint32_t used = 0;                      // its bytes taken
};

// A command buffer's first error, set under its lock; whether it has one is readable with no lock
class SwitchMetalSegError {
public:
    SwitchMetalSegError &operator=(NSError *error)
    {
        error_ = error;
        set_.store(error != nil, std::memory_order_release);
        return *this;
    }

    operator NSError *() const
    {
        return error_;
    }

    bool set() const
    {
        return set_.load(std::memory_order_acquire);
    }

private:
    NSError *error_ = nil;
    std::atomic<bool> set_{false};
};

// A command a parallel encoder's sub-encoder recorded, with its extra words (TegraGM20BMetalParallel.mm)
struct SwitchMetalSubCommand {
    Cmd cmd;
    std::vector<uint64_t> extra;
};

// A part of a command buffer submitted on its own (SwitchMetalCommandBuffer::_segs): tape [from, to), raw work or one event
struct SwitchMetalSegment {
    uint32_t from, to;
    bool event = false;
    bool gpuSignal = false;
    bool gpuWait = false;
    // The range before it laid predicated loops: it waits for the ranges before it to retire, and where one of their
    // loops would have run past its copies what follows that loop is laid down first (loop_ranges.h continuation)
    bool held = false;
};

// A counter sample a pass descriptor's attachment asks for (TegraGM20BMetalCounterEncode.mm): at the tape position of its encoder's
// first command (a start) or its OpEnd (an end); the command buffer retains the buffer
struct SwitchMetalCounterMark {
    uint32_t at;
    gm20b::counters::Point point;
    uint32_t index;
    __unsafe_unretained SwitchMetalCounterSampleBuffer *buffer;
};

struct SwitchMetalSharedEventWait {
    void *event;
    std::shared_ptr<gm20b::SharedEventLease> lease;
    uint64_t value;
    std::shared_ptr<gm20b::SharedEventCompletion> completion = {};
};

struct SwitchMetalDeferredPresent {
    enum class Kind : uint8_t {
        Immediate,
        AtTime,
        AfterMinimumDuration
    };

    id<MTLDrawable> drawable;
    Kind kind;
    CFTimeInterval value;
};

static inline void gm20bRunDeferredPresent(const SwitchMetalDeferredPresent &p)
{
    switch (p.kind) {
    case SwitchMetalDeferredPresent::Kind::Immediate:
        [p.drawable present];
        break;
    case SwitchMetalDeferredPresent::Kind::AtTime:
        [p.drawable presentAtTime:p.value];
        break;
    case SwitchMetalDeferredPresent::Kind::AfterMinimumDuration:
        [p.drawable presentAfterMinimumDuration:p.value];
        break;
    }
}

@interface SwitchMetalCommandBuffer : SwitchMetalObject {
@public
    uint64_t _protection;  // setProtectionOptions: (kept, no effect)
    NSMutableDictionary *_userDictionary;
    uint64_t _traceId;
    // The encoder being recorded: its first command's place (_encAt), and whether its recording failed the command buffer
    // (_encFailed: gm20bPassFailed), which leaves its draws unchecked and its pass unread. The command buffer is submitted in
    // segments (_segs, planSegments) and completes with its last.
    size_t _encAt;
    bool _encOpen, _encFailed;
    gm20b::PredicateRecords _predicates;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::atomic<bool> _forceWholePrograms, _inlineStates;
    // hw_test_app's probes (TegraGM20BMetalProbe.mm): its barriers' forms, where they took effect, and its 2D-engine work, each
    // at the blit encoder whose first command is at `at`
    gm20b::ProbeForms _probeForms;
    gm20b::ProbeTaken _probeTaken;

    struct ProbeOp {
        size_t at;
        gm20b::ProbeOp op;
        SwitchMetalTexture *read, *written;
    };

    std::vector<ProbeOp> _probeOps;
#endif
    std::vector<SwitchMetalSegment> _segs;
    size_t _segNext;
    SwitchMetalSegError _segError;    // its error, reported at completion
    gm20b::FinishState _finishState;  // its ranges in flight, and whether it submits no more (finish_state.h)
    uint64_t _channelGeneration, _gm20bPrefixGeneration;
    bool _gpuPrefixEligible;
    std::vector<gm20b::EventTimeline::Promise> _gpuEventWaits;
    std::vector<SwitchMetalSharedEventWait> _sharedEventWaits;
    std::vector<size_t> _sharedSignalRanges;
    // Kernel events' signals inside native ranges (TegraGM20BMetalInlineSignals.mm), by tape index, and those the range being encoded
    // released into their words (payload) or left to its job's end (0)
    std::vector<size_t> _inlineSignals;
    std::vector<std::pair<size_t, uint32_t>> _released;
    std::shared_ptr<gm20b::SharedEventWatch> _sharedEventWatch;
    std::shared_ptr<gm20b::ResourceReadyCompletion> _resourceReady;
    void *_nativeWaitEvent;
    // a drawable present was added (its completion is observed), cleared once it failed before all its work was
    // submitted (none is made, TegraGM20BMetalFinish.mm withholdPresents); the range being submitted ends quiet
    // (completion_source.h)
    std::atomic<bool> _presents;
    bool _rangeQuiet, _rangeHeld;
    // writer promises (Device::promise) made when it counted as scheduled before its work reached the kernel, under
    // _lock; taken once its last raw range carries them to the kernel or finish() cancels or withdraws them. _promised:
    // any were asked for, or its failure's unpromised writes were withdrawn (cancelPromises); read without the lock
    // by the frame path and a failed command buffer's waitUntilScheduled
    std::vector<uint32_t> _promises;
    std::atomic<bool> _promised;
    bool _promisesTaken;
    bool
        _sampledShadows;  // it binds a texture sampled through a pool image (gm20b::Texture::sampled): encodeRaw refreshes them
    bool _standIns;  // it binds an extended-range texture (gm20b::Texture::standIn): encodeRaw draws its stand-in
    // the two-plane RGB10A8_2P_XR10 textures a draw samples through the stand-in, its state having no ready " | xrs="
    // variant for them (xr_pass.h): only these of that format get their stand-in drawn
    std::set<SwitchMetalTexture *> _xrStandIns;
    SwitchMetalCommandPlan _plan;
    bool _recordingReturned;
    SwitchMetalPassCachePtr _passCache;
    gm20b::GrowList<SwitchMetalPass> _passes;  // OpRender's (Cmd::x)
    uint32_t _recordedPass;                    // the OpRender the draws being recorded belong to
    bool
        _borderSamplers;  // a sampler of an opaque border was bound: its headers for the range's textures before the encode
    std::vector<SwitchMetalCounterMark> _counterMarks;  // in tape order
    SwitchMetalBytes _bytes;                            // OpBytes': chunk Cmd::c, offset Cmd::a, length Cmd::b
    // each submitted range's written resources, for its completion (captureWrittenRange): the first _retired of them
    std::deque<gm20b::RetirementWrites> _retirements;
    size_t _retired;
    // vertex setBytes before a pass's first draw, and post-first vertex slots 0..2 (Core Animation's vertices and
    // transform), again in cached memory (Cmd::shadow: offset + 1): encode geometry never reads write-combined chunks
    gm20b::GrowList<uint8_t> _shadow;
    gm20b::GrowList<uint64_t> _extra;
    std::vector<id> _owners;
    gm20b::PointerMap<bool> _ownerKeys;  // everything the tape names, retained until completion
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    std::atomic<bool>
        _handlersQueued;  // scheduled handlers went to its queue's _handlers: its completed ones go behind them
    SwitchMetalCommandBuffer
        *_before;  // enqueued before it on its queue, until its own completed handlers were handed on
    std::vector<SwitchMetalSubCommand>
        *_commandLog;           // a sub-encoder's command buffer's log of what it records (null: none)
    NSArray *_doneHandlers;     // its completed handlers, from finish() until they ran
    bool _doneBehindScheduled;  // they run behind its scheduled handlers on the queue's handler queue
    bool _emptyPendingCompletion;
    std::vector<SwitchMetalDeferredPresent> _deferredPresents, _donePresents;
    std::atomic<bool> _hasDeferredPresents;
    bool _latePresent;
    gm20b::HandlerTurn _scheduledTurn, _turn;
    std::atomic<MTLCommandBufferStatus> _status;
    NSError *_error;
    SwitchMetalQueue *_queue;
    std::atomic<uint8_t> _enqueueState;  // 0: none, 1: commit claimed queue slot, 2: explicit enqueue pushed to worker
    gm20b::SubmitQueue<void *, SwitchMetalCommandBuffer *>::Reservation _reservation;
    bool _reserved;  // the submit worker's: its reservation made
    bool _countedUnscheduled, _earlyPromised;
    std::vector<size_t> _eventWaits;
    gm20b::EventDeadline::Time _eventStart;
    std::unordered_set<void *> _variantObserved;
    std::map<void *, std::shared_ptr<gm20b::ComputeKernelSet<gm20b::ComputeState>>> _computeJobs;
    size_t _eventArmed;            // operation position + 1; only the current wait installs a listener
    size_t _boundArmed;            // the submit worker's: the wait (_eventArmed) whose bound the kernel holds
    std::atomic<bool> _inHolders;  // in gHolders or gEventWaits (debug builds): unhold() takes their lock only then
    std::atomic<bool> _committed, _scheduledDone, _completedDone;
    // its queue's head waits on a shared event Metal's GPU would wait for: its writes are promised (scheduled_wait.h)
    std::atomic<bool> _held;
    // What the submit worker may encode before the commit (TegraGM20BMetalPipeline.mm). `_published`: where the worker may encode
    // up to (the first command of the encoder ended before the last one: an encoder's end looks at the two encoders after
    // it) << 32 | the tape's length, stored as each encoder ends; everything of the tape before that length is written for
    // good, but the end of an encoder past the first position, which a command recorded on it after its end moves at
    // the commit (TegraGM20BMetalSubmit.mm joinEndedEncoders). `_pipelineVeto`: why it is encoded at its commit instead
    // (SwitchMetalPipelineVeto), set as the reason is recorded.
    // `_endedAt`: the last ended encoder's first command; `_handedCommands`: commands since the worker was last handed it.
    std::atomic<uint64_t> _published;
    std::atomic<uint32_t> _pipelineVeto;
    // `_aheadStop`: the first command of the first encoder a reason of its own keeps from the encode ahead (0: none),
    // stored as the reason is recorded; the encoders before it are encoded ahead and its commit splits the range there
    // (gm20bPipelineSplit). `_aheadStopped`: the reasons that stopped it, the recording's and the worker's.
    std::atomic<uint32_t> _aheadStop, _aheadStopped;
    size_t _endedAt;
    uint32_t _handedCommands;
    bool _handedOnce;  // the worker was handed it (its one SwitchMetalScheduleChange::Encoded)
    // the worker's (the caller reads it): it gave up encoding this one ahead; and how many of its encoders it encoded so
    // (of how many)
    std::atomic<bool> _pipelineDeclined;
    uint32_t _pipelinedEncoders, _pipelineEncoders, _pipelineAbandons, _pipelineGivenBack;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::atomic<size_t> _aheadAt;    // where the worker's encode ahead of it reached (switchMetalWaitAhead)
    std::atomic<size_t> _aheadHalt;  // where the worker found an encoder it cannot take ahead (0: none)
#endif
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t _committedTicks;  // mach_absolute_time as its commit began
    uint64_t _kickedTicks;     // and as its first raw range's job went to the kernel (0: none yet)
    uint32_t _firstStamp;
    uint8_t _enqueueTiming;
    bool _lateWriteChecked, _lateWriteSampled;
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::atomic<uint64_t> _heldReturnUs;  // when waitUntilScheduled returned for it held, until it was submitted
    gm20b::ScheduleNotes _scheduleNotes;  // the submit worker's turns of it, for the frame rates line
#endif
    // scheduled() and finish() ran (their handlers taken): a handler added after goes at once, and scheduled() runs once (a
    // command buffer held for a shared event is scheduled at its commit, eventHeld). A handler is added under _lock after its
    // list's flag is set, and the lists are taken under _lock only where the flag says one was added: taking exchanges the
    // sent flag first, so no handler is both missed and not run (gm20bTakeHandlers).
    std::atomic<bool> _scheduledSent, _submittedAll, _completedSent, _hasScheduledHandlers, _hasCompletedHandlers;
    gm20b::ChangeWord _changes;     // a state change its waits read (gm20bChanged), with no lock (word_wait.h)
    gm20b::OwnedWord _submitOwner;  // submitLoop's thread port while committed and actively queued for submission
    std::atomic<uint32_t> _compileWaitGen;
    std::vector<std::function<bool()>> _compileWaits;  // under _lock: blocking compile waits while parked
    gm20b::Mutex _lock;
    uint32_t _seqno;
    uint32_t _stamp;  // its raw submission's GPU time stamps (gm20b::CommandBuffer::stamp), 0 for any other
    uint32_t
        _rangeLoopStamp;  // the raw range just encoded laid predicated loops: its stamp (SwitchMetalRawRetirement::loopStamp)

    // _lock: a retired range [from, to) recorded that its loop ending at `tag` would run past its copies (SwitchMetalSegment::held)
    struct {
        bool pending;
        uint32_t tag, from, to;
    } _loopContinue;

    std::atomic<double> _kernelStart, _kernelEnd, _gpuStart, _gpuEnd;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool _gpuStamped;  // GPUStartTime/GPUEndTime are its submission's GPU stamps, not the host's window
#endif
    // the encoder being recorded: its end, its drop (endDropped) and this command buffer's drop clear it
    __unsafe_unretained id _active;
    uint64_t _predA, _predB;  // the first one's values
    uint8_t _pred0;           // the first one
    uint8_t _failedBy;        // the predicate its failure named (failCommandBuffer, nameFailure), 0: none
#if SWITCH_FRAME_TIMELINE
    switch_frame::Stamps _frame;
#endif
}
- (void)closeEncoder __attribute__((objc_direct));
// direct (no message): every recorded command and every object a command names goes through them
- (void)record:(const Cmd &)cmd __attribute__((objc_direct));
- (void)retain:(__unsafe_unretained id)object __attribute__((objc_direct));
// record: of a binding or raster command no other duty of record: concerns (a buffer, bytes, texture, sampler,
// depth/stencil state or one scissor; not a render state, whose pass may note storage targets)
- (void)recordBinding:(const Cmd &)cmd __attribute__((objc_direct));
@end

// `cb` fails with predicate `p`'s error (predFailure), naming `detail` where given; the process's first hit of `p` logs
// its values (TegraGM20BMetalPredicates.mm)
void predFails(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *detail);
// A call Metal's validation stops (`rule`) as it is recorded: an object another device made, or nil where Metal takes
// none. Apple's driver dereferences what it is handed (a nil state or index buffer is EXC_BAD_ACCESS on an M4 Max with
// validation off: c38-audit/m2v-cut2/probe); here the call records nothing and the command buffer fails
// MTLCommandBufferErrorInvalidResource, its predicate named in its path, the first of each logged with `rule`
// (TegraGM20BMetalPassErrors.mm)
void gm20bCallInvalid(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule);

// A refusal predicate hit while recording (or encoding) `cb`: the first one's values kept; the process's first hit of `p`
// logged with them; a predicate of predFailure fails the command buffer
static inline void hit(__unsafe_unretained SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a = 0,
                       uint64_t b = 0, const char *detail = nullptr)
{
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    if (predFailure(p))
        return predFails(cb, p, a, b, detail);
    if (!predLogged(p))
        logPred(p, a, b, detail ? @(detail) : nil);
}

// A call Metal's validation stops that the driver runs as Apple's driver runs it (gm20bSkipped): a hit as `p`, its
// `rule` (a literal) logged once a process
static inline void skip(__unsafe_unretained SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b,
                        const char *rule, uint64_t n = 1)
{
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    predSkipped(p, a, b, rule, n);
}

// The predicate of the failure the submit worker found encoding or readying `cb`, committed and so written by no other
// thread now: kept unless the failure named one already
static inline void nameFailure(__unsafe_unretained SwitchMetalCommandBuffer *cb, SwitchMetalPred p)
{
    if (!cb->_failedBy)
        cb->_failedBy = p;
}

// Why a command buffer's tape is encoded at its commit, not as its encoders end (SwitchMetalCommandBuffer::_pipelineVeto, and the
// worker's own findings): what the encode reads that the recording may still change, or work it is not built for
enum SwitchMetalPipelineVeto : uint32_t {
    VetoCounters = 1u << 1,    // counter samples
    VetoShadows = 1u << 2,     // a texture sampled through a pool image or an extended-range stand-in
    VetoVariant = 1u << 3,     // a binding of a state variant still being made, which an encode ahead would drop
    VetoProbes = 1u << 4,      // HW Test's probes
    VetoParallel = 1u << 5,    // a parallel render encoder's sub-encoders
    VetoOps = 1u << 6,         // an event, a compute encoder, an indirect command buffer, patches, a late command
    VetoRefused = 1u << 7,     // the command buffer failed as it recorded (failCommandBuffer): nothing of it runs
    VetoPlanar = 1u << 8,      // a planar texture converted for its draws
    VetoNative = 1u << 9,      // a surface read through its native image before a later encoder wrote it
    VetoFailed = 1u << 10,     // its encode failed ahead (the commit's encode meets the failure again, as before)
    VetoOtherWork = 1u << 11,  // the worker needed the device for another command buffer
    VetoDevice = 1u << 12,     // a purge, or a channel made again since it began
    VetoZcull = 1u << 13,      // a pass loads a depth an earlier pass stored (zcull_planes.h)
    VetoChannel = 1u << 14,    // the kernel gave its begin no channel (the commit's encode asks again)
    VetoKinds = 15
};

static inline void gm20bPipelineVeto(SwitchMetalCommandBuffer *cb, uint32_t reason)
{
    if (!(cb->_pipelineVeto.load(std::memory_order_relaxed) & reason))
        cb->_pipelineVeto.fetch_or(reason, std::memory_order_release);
}

// The recording thread's: the encoder being recorded is not encoded ahead for `reason`, the encoders before it still are.
// Stored before the publish of that encoder's end, which is the earliest the worker may encode the end before it.
static inline void gm20bPipelineStop(SwitchMetalCommandBuffer *cb, uint32_t reason)
{
    if (!cb->_encAt) {
        gm20bPipelineVeto(cb, reason);
        return;
    }
    if (!cb->_aheadStop.load(std::memory_order_relaxed))
        cb->_aheadStop.store(uint32_t(cb->_encAt), std::memory_order_release);
    if (!(cb->_aheadStopped.load(std::memory_order_relaxed) & reason))
        cb->_aheadStopped.fetch_or(reason, std::memory_order_relaxed);
}

// ---- encoders: they record ----
@interface SwitchMetalEncoder : SwitchMetalObject {
@public
    // its command buffer's caller holds it while it records (Metal); a command buffer dropped first clears it
    // (commandBufferDropped)
    __unsafe_unretained SwitchMetalCommandBuffer *_cb;
    bool _ended;
    uint64_t _traceId;
    std::vector<SwitchMetalCounterMark> _counterPending;  // its descriptor's samples not yet placed on the tape
}
@end

// The next of the device's trace object IDs, which its command buffers and encoders take at their first globalTraceObjectID
// (TegraGM20BMetalEncoder.mm)
uint64_t nextTraceId();

// A variant a render encoder's prepare found for a state and its forms (TegraGM20BMetalCubic.mm preparedVariant)
struct SwitchMetalPreparedVariant {
    __unsafe_unretained SwitchMetalPipeline *pipe = nil;
    gm20b::RenderVariant forms;
    gm20b::Variant<gm20b::Pipeline> *variant = nullptr;
};

// A render encoder keeps what its draws need to decide, as they are recorded, whether the driver draws them: the state, each
// bound buffer's address, the textures, the pass's formats. A draw it refuses is decided then, so encodeRaw never meets it.
@interface SwitchMetalRenderEncoder : SwitchMetalEncoder {
@public
    // Unretained, as the bound objects below: the command buffer retains each (setBinding), so a bind stores no
    // reference count and a pooled encoder's end releases none of its 172 slots
    __unsafe_unretained SwitchMetalPipeline *_pipe;
    uint64_t _bufVa[3]
                   [31];  // stage 1 vertex, 2 fragment: the bound buffer's address plus offset (setBytes: 0, aligned)
    __unsafe_unretained SwitchMetalBuffer *_buf[3][31];
    __unsafe_unretained SwitchMetalTexture *_tex[2][gm20b::kStageTextures];  // vertex, fragment textures
    __unsafe_unretained SwitchMetalSampler *_samp[2][16];
    uint16_t _cubicMask[2];
    // `_samp` slots whose sampler the plan holds bound since `_sampEpoch` (SwitchMetalCommandPlan::stateEpoch): binding the same
    // one again records nothing
    uint16_t _sampRecorded[2];
    uint64_t _sampEpoch;
    gm20b::RenderVariant _forms;
    gm20b::Variant<gm20b::Pipeline> *_formVariant, *_sampledVariant;
    // the variants its prepares found: a pass switching among its states asks a state's map once for each forms
    std::array<SwitchMetalPreparedVariant, 8> _prepared;
    uint8_t _preparedNext;
    uint16_t _formSamplers[2];  // populated when a cubic binding first needs this pipeline's sampler slots
    gm20b::StageTextureMask _cubicTextures[2];
    std::array<gm20b::StageTextureMask, 2> _sampledSlots;
    // moves with each bind that can change what checkDraw decides (setBytes and a plain texture's or sampler's cannot);
    // checkDraw's last full check was at `_checkedEligibility`, `_checkedEpoch` and `_checkedPipeline`
    uint32_t _eligibility, _checkedEligibility;
    uint64_t _checkedEpoch;
    const gm20b::Pipeline *_checkedPipeline;
    gm20b::Pipeline *_drawn;
    gm20b::Variant<gm20b::Pipeline> *_drawnVariant;
    uint32_t _formats[8];  // the pass's color attachments' formats as the driver holds them (0: none)
    uint32_t _count;       // its attachment count (the last attachment's index + 1)
    uint32_t _zeta;        // its depth/stencil attachment's format as the driver holds it (0: none)
    uint32_t _samples;     // its color attachments' samples a pixel
    uint8_t _turn;         // its turned attachments' rotation (rotation.h; 0: none)
    uint32_t _passAt;      // its pass's SwitchMetalPass (the command buffer's _passes)
    gm20b::DepthStencilDesc _ds;
    MTLRenderPassDescriptor *_pass;  // the command buffer's copy, which the raw encode reads
    bool _passShared;                // _pass is the recording bank's immutable copy (TegraGM20BMetalPassCache.h)
    size_t _renderAt;                // its OpRender in the command buffer's plan
    __unsafe_unretained id<MTLBuffer> _visibility;  // its pass's visibility result buffer (the command buffer holds it)
    bool _drew;                                     // a draw recorded
    // setTessellationFactorBuffer:offset:instanceStride: and setTessellationFactorScale:, which its patch draws record
    // (TegraGM20BMetalTessellation.mm; unretained: each draw retains what it reads)
    SwitchMetalBuffer *_tessFactors;
    NSUInteger _tessFactorOffset, _tessFactorStride;
    float _tessScale;
    BOOL _tessScaleSet;
}
@end
@interface SwitchMetalBlitEncoder : SwitchMetalEncoder
@end
@interface SwitchMetalParallelRenderEncoder : SwitchMetalEncoder  // (TegraGM20BMetalParallel.mm)
@end
@interface SwitchMetalComputeEncoder : SwitchMetalEncoder {
@public
    MTLDispatchType _type;
    // A predication Start of this encoder named no buffer: its Ends answer NO from then on, as AGX's do (21H461
    // 0x21c90e51c); every other End answers YES, since MPSGraph aborts on a NO (CSCBSuccess, 0x1810c10a4)
    bool _predicateWithoutBuffer;
    MTLRegion _stageIn;  // setStageInRegion:, which each dispatch records (a stage-in kernel's [[stage_in]] grid)
    // setStageInRegionWithIndirectBuffer:'s buffer and offset (nil: _stageIn), which each dispatch records
    SwitchMetalBuffer *_stageInBuffer;
    NSUInteger _stageInOffset;
}
@end

// TegraGM20BMetalComputeState.mm: a compute state with what the pack keys its kernels by, answered from the pack's facts of
// it or the shader compiler's
id gm20bComputeState(id device, MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
                     MTLAutoreleasedComputePipelineReflection *reflection, NSError **error);
// A compute encoder's commands (the Op*Kernel* ones and OpDispatch), and their raw encode (encodeKernel): false where it
// refuses one (*why)
bool isKernelCommand(Op op);
// OpBytes and OpKernelBytes (`m`): the bytes into the command buffer's SwitchMetalBytes, m.a/b/c set (TegraGM20BMetalBindings.mm)
void recordBytes(__unsafe_unretained SwitchMetalCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length,
                 bool shadow = false);
// `at`: the command's tape index, a loop's tag (loop_ranges.h)
bool encodeKernel(gm20b::ComputeEncoder &e, gm20b::CommandBuffer &rc, SwitchMetalCommandBuffer *cb, const Cmd &c,
                  size_t at, const char **why);

static inline void setBinding(__unsafe_unretained SwitchMetalCommandBuffer *cb, Op op, int stage, NSUInteger index,
                              __unsafe_unretained id object, uint64_t c)
{
    if (object)
        [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = c;
    if (op == OpBuffer || op == OpBufferOffset || op == OpTexture || op == OpSampler || op == OpDepthStencil)
        [cb recordBinding:m];
    else
        [cb record:m];
}

// The encoder `e` records into this command buffer from now on, the one before it ended (TegraGM20BMetalEncoder.mm)
@interface SwitchMetalCommandBuffer (Encoders)
- (void)begin:(SwitchMetalEncoder *)e __attribute__((objc_direct));
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
@end

@interface SwitchMetalEncoder ()
- (void)endEncoding;
- (void)updateFence:(id<MTLFence>)fence;
- (void)waitForFence:(id<MTLFence>)fence;
@end

// `s` with a bind's LOD clamps (TegraGM20BMetalBindings.mm)
id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax);

@interface SwitchMetalRenderEncoder ()
// (TegraGM20BMetalRenderEncoder.mm) NO: a draw Apple's driver draws nothing defined for, which is skipped (client_errors.h)
- (BOOL)checkDraw __attribute__((objc_direct));
@end

// TegraGM20BMetalEncoder.mm: the encoder's own copy of its pass, where it shares the thread's, before it changes it
void gm20bOwnPass(SwitchMetalRenderEncoder *e);

// A texture or sampler bound at slot `index` of stage `stage` (1 vertex, 2 fragment), as its setter binds it
// (TegraGM20BMetalBindings.mm)
bool gm20bBindTexture(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage, __unsafe_unretained id<MTLTexture> t,
                      NSUInteger index);
bool gm20bBindSampler(__unsafe_unretained SwitchMetalRenderEncoder *e, int stage,
                      __unsafe_unretained id<MTLSamplerState> s, NSUInteger index);

@interface SwitchMetalRenderEncoder (Cubic)
- (const gm20b::Pipeline *)drawnRecord __attribute__((objc_direct));  // (TegraGM20BMetalCubic.mm)
- (void)prepareDrawnRecord __attribute__((objc_direct));
@end

#if SWITCH_FRAME_TIMELINE
extern std::atomic<uint64_t> gSurfaceTextures;
#endif

static inline uint64_t extra(SwitchMetalCommandBuffer *cb, uint64_t i)
{
    return cb->_extra[i];
}

static inline double extraDouble(SwitchMetalCommandBuffer *cb, uint64_t i)
{
    double d;
    memcpy(&d, &cb->_extra[i], 8);
    return d;
}

static inline float extraFloat(SwitchMetalCommandBuffer *cb, uint64_t i)
{
    const uint32_t u = uint32_t(cb->_extra[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
