// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "SwitchFrameTimeline.h"
#include "raw_gpu.h"
#include "RawMetalPredicates.h"
#include "RawResidency.h"

@class RMDevice, RMCommandBuffer;

// An m2v twin made outside its object's lock, kept if no other thread kept one first (theirs is returned, this one dropped). The
// object's lock is never held across a call into m2v: a thread inside m2v holding one of its locks (its handlers run Core
// Animation's, which may ask for a twin) and a thread here holding the object's lock asking m2v for one of its own would wait for
// each other for good (r7p8's freeze: a wait with no bound).
static inline id twinPublish(id object, __strong id &slot, id made)
{
    @synchronized(object) {
        if (!slot)
            slot = made;
        return slot;
    }
}

// ---- the common base: an m2v twin and the selectors this facade does not answer itself ----
@interface RMObject : NSObject {
@public
    __weak id _device;
    id _twin;  // the m2v object over the same memory (made on first need)
}
@property (nonatomic, copy) NSString *label;
- (id)rmTwin;  // made now if needed; nil if it cannot be
- (Class)rmTwinClass;
@end

@interface RMDevice : NSObject  // the methods the device class gets (raw_metal_device_open); no state of its own
- (void)rmCommit:(RMCommandBuffer *)cb;
- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled;
@end

// ---- functions and libraries: the m2v library and functions, with the name and constants text the pack keys by ----
// The libraries the pack's states come from are frameworks' default.metallib (QuartzCore's, RenderBox's): a state of their
// functions is looked up by its key, which names the framework but QuartzCore's (raw_metal_pipeline_key)
@interface RMLibrary : RMObject {
@public
    std::string _framework;  // "QuartzCore", "RenderBox"...; empty: not a framework's default library (never looked up)
}
@end
@interface RMFunction : RMObject {
@public
    std::string _constants;
    std::string _framework;
}
@property (nonatomic, copy) NSString *name;
@end

// ---- resources ----
// A heap of the device (RawMetalHeap.mm): an arena its resources are placed in, or with none (a size or storage mode no arena
// takes) every resource made as the device makes it, counted against its size
@interface RMHeap : RMObject {
@public
    MTLHeapDescriptor *_descriptor;
    std::atomic<uint64_t> _used;  // what its resources made outside its arena hold
    uint32_t _arena;              // its arena + 1; 0: none
    std::atomic<int> _purgeable;  // the MTLPurgeableState asked last (0: never asked)
}
@end

@interface RMBuffer : RMObject {
@public
    raw::Buffer _raw;
    MTLResourceOptions _options;
    std::atomic<bool> _gpuDirty;  // the GPU wrote it since the CPU last read it
    void (^_deallocator)(void *, NSUInteger);
    RMHeap *_heap;
    uint64_t _heapBytes;  // what it counts against its heap outside the arena
    bool _aliasable;
    MTLPurgeableState _purgeable;
}
@end
@interface RMBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row;
@end

typedef NS_ENUM(uint8_t, RMTextureKind) {
    RMTexturePool,
    RMTextureSurface,
    RMTextureBuffer,
    RMTextureView,
    RMTextureMemoryless,
    RMTextureTwinOnly
};

@interface RMTexture : RMObject {
@public
    raw::Texture _raw;
    bool _rawOk;  // the raw path can draw with it (else every command buffer using it goes to m2v)
    RMTextureKind _kind;
    std::atomic<bool> _gpuWrites;  // its surface was declared GPU-written (surfaceGpuWrites)
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _surface;
    NSUInteger _plane;
    // a planar texture's image (4:2:0 converted, 4:4:4 codes) holds its surface at this seed (_imageCurrent): converted
    // again only once the surface's seed moves (r8a: a YouTube page's thumbnails converted again at every command buffer)
    uint32_t _imageSeed;
    uint64_t _imageWrites;  // and its surface's writes the seed does not show (surfaceWrites)
    bool _imageCurrent;
    RMTexture *_parent;  // a view's
    // a view of part of its parent (newTextureViewWithPixelFormat:textureType:levels:slices:): those of the parent, which its
    // twin views too; the view's type is its descriptor's
    NSRange _viewLevels, _viewSlices;
    bool _viewPart;
    RMBuffer *_buffer;  // a texture over a buffer's
    NSUInteger _offset, _rowBytes;
    std::atomic<bool> _gpuDirty;
    MTLPurgeableState _purgeable;
    uint8_t _pred;  // why the raw path does not keep it (RMPred; 0: it does)
    RMHeap *_heap;
    uint64_t _heapBytes;  // as RMBuffer's
    bool _aliasable;
}
@end

@interface RMSampler : RMObject {
@public
    raw::Sampler _raw;
    MTLSamplerDescriptor *_descriptor;
    NSMutableDictionary<NSValue *, id<MTLSamplerState>> *_clamped;  // its descriptor's with a bind's LOD clamps
}
@end

@interface RMPipeline : RMObject {
@public
    // null: not in the pack (every command buffer using it goes to m2v), or not yet made by the runtime (completeLater: set once,
    // released, from its queue)
    raw::Pipeline *_raw;
    bool _packable;     // its functions are one framework's default library's (looked up in the pack)
    bool _completable;  // both its functions are this device's (the runtime makes its record if the pack has none)
    bool _caTransform;
    bool _asked;                             // its record asked of the runtime (completeLater)
    std::string _key;                        // its pack key (completable states')
    MTLRenderPipelineDescriptor *_madeFrom;  // completable states': what its cubic variants are made of
    // its cubic variants (raw_cubic.h) by their binds' name: null where the runtime refused one; `_variantsAsked`: being made
    std::mutex _variantLock;
    std::map<std::string, raw::Pipeline *> _variants;
    std::set<std::string> _variantsAsked;
}
@end

@interface RMTwinObject : RMObject  // compute states, and any m2v object the facade only carries
@end

@interface RMDepthStencil : RMTwinObject {
@public
    raw::DepthStencilDesc _raw;
}
@end

// ---- the tape ----
enum Op : uint8_t {
    OpRender,
    OpBlit,
    OpCompute,
    OpEnd,
    OpPipeline,
    OpBytes,
    OpBuffer,
    OpBufferOffset,
    OpTexture,
    OpSampler,
    OpScissor,
    OpViewport,
    OpDraw,
    OpCull,
    OpWinding,
    OpDepthStencil,
    OpFillMode,
    OpBlendColor,
    OpStencilRef,
    OpDepthBias,
    OpBarrier,
    OpUse,
    OpUpdateFence,
    OpWaitFence,
    OpCopyTexture,
    OpCopyBuffer,
    OpCopyBufferToTexture,
    OpCopyTextureToBuffer,
    OpMipmaps,
    OpInvocation,
    OpSignalEvent,
    OpWaitEvent,
    OpDrawIndexed,
    OpVisibility,
    OpFillBuffer,
    // a compute encoder's (RawMetalCompute.mm)
    OpKernelState,
    OpKernelBytes,
    OpKernelBuffer,
    OpKernelBufferOffset,
    OpKernelTexture,
    OpKernelSampler,
    OpKernelMemory,
    OpKernelUse,
    OpKernelBarrier,
    OpDispatch,
};

// one record: `stage` 1 vertex, 2 fragment; `obj` unretained (the command buffer keeps it); `x` into the extras
struct Cmd {
    Op op;
    uint8_t stage;
    uint16_t index;
    uint32_t a, b;
    uint64_t c, x;
    void *obj, *obj2;
};

inline Cmd cmd(Op op, uint8_t stage = 0, uint16_t index = 0)
{
    Cmd c = {};
    c.op = op;
    c.stage = stage;
    c.index = index;
    return c;
}

@interface RMQueue : RMObject {
@public
    dispatch_queue_t _completionQueue;
    uint32_t _gpuPriority, _backgroundGpuPriority;  // what it asked for (RawMetalQueuePriority.mm)
    raw::ResidencySets _residency;
}
@end

// What the raw encode reads of a render pass, taken from the descriptor's copy as its encoder is made (the copy itself is what
// m2v's replay encodes): no Metal getter, no attachment object Metal makes on a first read, per encode. Objects unretained: the
// command buffer's owners hold them.
struct RMPass {
    struct Color {
        __unsafe_unretained id texture;
        NSUInteger level;
        MTLLoadAction load;
        MTLStoreAction store;
        MTLClearColor clear;
    } color[8];

    __unsafe_unretained id depth;
    MTLLoadAction depthLoad, stencilLoad;
    double clearDepth;
    uint32_t clearStencil;
    NSUInteger width, height;
};

// A command buffer's setBytes data, written once where the GPU reads it: write-combined pool chunks the raw encode binds in place
// (m2v's replay reads them back), each copy 256-byte aligned and zero to the next 256 bytes as an upload chunk is. The chunks go
// with the recording (giveRecording), so they outlive every submission that reads them.
struct RMBytes {
    std::vector<raw::Buffer> chunks;  // va 0: heap memory (the GPU pool had none; the command buffer is m2v's)
    size_t at = 0;                    // the chunk being filled
    uint32_t used = 0;                // its bytes taken
};

// A part of a command buffer submitted on its own (RMCommandBuffer::_segs): tape [from, to), on the raw path or m2v
struct RMSegment {
    uint32_t from, to;
    bool raw;
};

@interface RMCommandBuffer : RMObject {
@public
    uint64_t _protection;  // setProtectionOptions: (kept, no effect)
    NSMutableDictionary *_userDictionary;
    uint64_t _traceId;
    raw::ResidencySets _residency;
    // Its encoders: each one's first command (OpRender, OpBlit, OpCompute) carries index 1 where it goes to m2v (a refusal taken
    // while it was the active encoder: _encRefused, written into it as the next encoder begins or at commit, _encAt the
    // command's place). A refusal outside any encoder keeps the command buffer whole (_whole). One whose encoders are some raw's
    // and some m2v's is submitted in segments (_segs, planSegments): each run of encoders of one driver on its own, in order on
    // the GPU as two command buffers of the drivers are (the kernel's Order), so a refused pass costs m2v that pass, not the
    // frame. The command buffer completes with its last segment.
    size_t _encAt;
    bool _encOpen, _encRefused, _whole;
    std::vector<RMSegment> _segs;
    size_t _segNext;
    NSError *_segError;  // an m2v segment's error, reported at completion
    bool
        _sampledShadows;  // it binds a texture sampled through a pool image (raw::Texture::sampled): encodeRaw refreshes them
    bool _standIns;  // it binds an extended-range texture (raw::Texture::standIn): encodeRaw draws its stand-in
    std::vector<Cmd> _tape;
    std::vector<RMPass> _passes;  // OpRender's (Cmd::x)
    RMBytes _bytes;               // OpBytes': chunk Cmd::c, offset Cmd::a, length Cmd::b
    std::vector<uint64_t> _extra;
    std::vector<id> _owners;  // everything the tape names, retained until completion
    RMReason _reason;
    NSString *_why;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    MTLCommandBufferStatus _status;
    NSError *_error;
    RMQueue *_queue;
    bool _committed, _scheduledDone, _completedDone, _m2v;
    // its event signals are in m2v work (replayed whole, or in its last segment): finish() makes none
    bool _signalsOnM2v;
    bool
        _partial;  // the kernel refused its raw submission after part of it ran (raw::CommandBuffer::partial): never replayed
    // scheduled() and finish() ran (their handlers taken): a handler added after goes at once, and scheduled() runs once (a
    // command buffer held for a shared event is scheduled at its commit, eventHeld)
    bool _scheduledSent, _completedSent;
    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _seqno;
    uint32_t _stamp;  // its raw submission's GPU time stamps (raw::CommandBuffer::stamp), 0 for any other
    double _kernelStart, _kernelEnd, _gpuStart, _gpuEnd;
    __weak id _active;        // the encoder being recorded (weak: it holds the command buffer, its caller holds it)
    uint64_t _preds;          // every refusal predicate it hit (1 << RMPred)
    uint64_t _predA, _predB;  // the first one's values
    uint8_t _pred0;           // the first one
#if SWITCH_FRAME_TIMELINE
    switch_frame::Stamps _frame;
#endif
}
- (void)takeReason:(RMReason)reason why:(NSString *)why;
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why;
- (void)closeEncoder;
- (void)record:(const Cmd &)cmd;
- (void)retain:(id)object;
@end

// A refusal predicate hit while recording (or encoding) `cb`: into its mask; the first one's values kept; the process's first hit
// of `p` logged with them
static inline void hit(RMCommandBuffer *cb, RMPred p, uint64_t a = 0, uint64_t b = 0)
{
    cb->_preds |= 1ull << p;
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    if (!predLogged(p))
        logPred(p, a, b, nil);
}

// ---- encoders: they record ----
@interface RMEncoder : RMObject {
@public
    RMCommandBuffer *_cb;
    bool _ended;
    uint64_t _traceId;
}
@end

// The next of the device's trace object IDs, which its command buffers and encoders take at their first globalTraceObjectID
// (RawMetalEncoder.mm)
uint64_t nextTraceId();

// A render encoder keeps what its draws need to decide, as they are recorded, whether the raw path draws them exactly as m2v
// would: the state, each bound buffer's address, the textures, the pass's formats. A draw it would not is the command buffer's
// reason then, so encodeRaw never meets it (a fallback costs no encoding).
@interface RMRenderEncoder : RMEncoder {
@public
    RMPipeline *_pipe;
    uint64_t _bufVa[3]
                   [31];  // stage 1 vertex, 2 fragment: the bound buffer's address plus offset (setBytes: 0, aligned)
    RMBuffer *_buf[3][31];
    RMTexture *_tex[16];   // fragment textures (unretained: the command buffer retains what it binds)
    RMSampler *_samp[16];  // fragment samplers, alike; `_cubicMask`: the slots of those that are cubic (raw_cubic.h)
    uint16_t _cubicMask;
    raw::Pipeline *_drawn;  // the record its draws are recorded with: the state's, or a cubic variant of it
    uint32_t _formats[8];   // the pass's color attachments' formats as the raw path holds them (0: none)
    uint32_t _count;        // its attachment count (the last attachment's index + 1)
    uint32_t _zeta;         // its depth/stencil attachment's format as the raw path holds it (0: none)
    uint32_t _samples;      // its color attachments' samples a pixel
    uint32_t _passAt;       // its pass's RMPass (the command buffer's _passes)
    raw::DepthStencilDesc _ds;
    MTLRenderPassDescriptor *_pass;  // the command buffer's copy, which the raw encode and m2v's replay read
}
@end
@interface RMBlitEncoder : RMEncoder
@end
@interface RMComputeEncoder : RMEncoder {
@public
    MTLDispatchType _type;
}
@end

// RawMetalCompute.mm: a compute state over m2v's (`twin`), with what the raw pack keys its kernels by
id rmComputeState(id device, id twin, id<MTLFunction> function, MTLComputePipelineDescriptor *descriptor);
// m2v's replay and the raw encode of a compute encoder's commands (the Op*Kernel* ones and OpDispatch): false from the raw
// encode where it refuses one (*why)
bool isKernelCommand(Op op);
// OpBytes and OpKernelBytes (`m`): the bytes into the command buffer's RMBytes, m.a/b/c set (RawMetalBindings.mm)
void recordBytes(RMCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length);
void replayKernel(id encoder, RMCommandBuffer *cb, const Cmd &c, std::set<RMBuffer *> &buffers);
bool encodeKernel(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMCommandBuffer *cb, const Cmd &c, const char **why);

static inline void setBinding(RMCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t c)
{
    if (object)
        [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = c;
    [cb record:m];
}

@interface RMEncoder ()
- (void)endEncoding;
- (void)updateFence:(id<MTLFence>)fence;
- (void)waitForFence:(id<MTLFence>)fence;
@end

// `s` with a bind's LOD clamps (RawMetalBindings.mm)
id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax);

@interface RMRenderEncoder (Bindings)
- (void)texture:(int)stage object:(id<MTLTexture>)t index:(NSUInteger)index;
- (void)sampler:(int)stage object:(id<MTLSamplerState>)s index:(NSUInteger)index;
@end

@interface RMRenderEncoder (Cubic)
- (const raw::Pipeline *)drawnRecord;  // (RawMetalCubic.mm)
@end

#if SWITCH_FRAME_TIMELINE
extern std::atomic<uint64_t> gSurfaceTextures, gTwinsMade;
extern thread_local uint64_t tTwinTicks;
void countTwin(id object, uint64_t ticks);  // the rates line's tw= (RawMetalReplay.mm)
#endif
static inline id twinOf(id object)
{
    if (!object)
        return nil;
    if (![object isKindOfClass:[RMObject class]])
        return object;  // an m2v object already (events, fences, anything the device forwarded)
#if SWITCH_FRAME_TIMELINE
    if (!((RMObject *)object)->_twin) {
        const uint64_t began = mach_absolute_time();
        id twin = [(RMObject *)object rmTwin];
        const uint64_t took = mach_absolute_time() - began;
        tTwinTicks += took;
        gTwinsMade.fetch_add(1, std::memory_order_relaxed);
        countTwin(object, took);
        return twin;
    }
#endif
    return [(RMObject *)object rmTwin];
}

static inline uint64_t extra(RMCommandBuffer *cb, uint64_t i)
{
    return cb->_extra[i];
}

static inline double extraDouble(RMCommandBuffer *cb, uint64_t i)
{
    double d;
    memcpy(&d, &cb->_extra[i], 8);
    return d;
}

static inline float extraFloat(RMCommandBuffer *cb, uint64_t i)
{
    const uint32_t u = uint32_t(cb->_extra[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
