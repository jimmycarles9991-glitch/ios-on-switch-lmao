// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_execution_plan.h"
#include "raw_object_pool.h"
#include "raw_parallel.h"
#include "raw_compute.h"
#include "raw_scheduled_wait.h"
#include "raw_take_pool.h"

// ---- the device's submission machinery ----

// A finished command buffer's recording storage, kept for the next one (its capacity a frame's): Core Animation's steady frames
// allocate none of it. As many are kept as were live at once (an app that commits a batch before it waits records each into
// kept storage), up to kKeptTotal bytes and kKeptMost recordings; gRawRelease gives back all but kKeptIdle. Taken and kept
// with no lock (raw_take_pool.h): a command buffer takes one where it is made and keeps it where it is freed.
struct RMRecording {
    RMCommandPlan plan;
    std::vector<RMPass> passes;
    RMBytes bytes;
    std::deque<raw::RetirementWrites> retirements;
    std::vector<uint8_t> shadow;
    std::vector<uint64_t> extra;
    std::vector<id> owners;
    raw::PointerMap<bool> ownerKeys;
    std::vector<RMSegment> segs;
    size_t kept = 0;
};

static constexpr size_t kKeptTotal = 16 << 20, kKeptIdle = 4;
static constexpr uint32_t kKeptMost = 64;
static raw::TakePool<RMRecording, kKeptMost> &gRecordings = *new raw::TakePool<RMRecording, kKeptMost>;
static std::atomic<size_t> gRecordingBytes{0};

static void swapRecording(RMCommandBuffer *b, RMRecording &r)
{
    std::swap(b->_plan, r.plan);
    b->_passes.swap(r.passes);
    std::swap(b->_bytes, r.bytes);
    b->_retirements.swap(r.retirements);
    b->_shadow.swap(r.shadow);
    b->_extra.swap(r.extra);
    b->_owners.swap(r.owners);
    std::swap(b->_ownerKeys, r.ownerKeys);
    b->_segs.swap(r.segs);
}

static void takeRecording(RMCommandBuffer *b)
{
    uint32_t node;
    RMRecording *r = gRecordings.take(node);
    if (!r) {
        b->_owners.reserve(256);
        return;
    }
    swapRecording(b, *r);
    gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
    r->kept = 0;
    gRecordings.giveBack(node);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// facadecost's: setBytes chunks made for a command buffer the pool had none for, and made ahead, and the bytes of every
// setBytes chunk alive (kept, in a recording or in flight)
static std::atomic<uint64_t> gChunkMisses{0}, gChunksMadeAhead{0}, gChunkBytesLive{0};
#define RM_CHUNK_COUNT(counter, value) counter.fetch_add(uint64_t(value), std::memory_order_relaxed)
#else
#define RM_CHUNK_COUNT(counter, value)
#endif

static void dropBytes(RMBytes &s)
{
    for (raw::Buffer &k : s.chunks)
        if (k.va) {
            RM_CHUNK_COUNT(gChunkBytesLive, -k.length);
            gRM->raw->destroyBuffer(k);
        } else
            free(k.cpu);
    s = {};
}

// A recording that outgrew its first chunk keeps one chunk of their total: the next command buffer of its shape names one
// chunk to its job, not one for each 64 KB of its setBytes
static void coalesceBytes(RMBytes &s)
{
    if (s.chunks.size() < 2)
        return;
    uint64_t length = 0;
    for (const raw::Buffer &k : s.chunks) {
        if (!k.va)
            return;
        length += k.length;
    }
    raw::Buffer one;
    if (!gRM->raw->createControlBuffer(length, one))
        return;
    RM_CHUNK_COUNT(gChunkBytesLive, one.length);
    dropBytes(s);
    s.chunks.push_back(one);
}

// Finished command buffers' setBytes chunks for the next to fill, given back at completion rather than with the recording
// at dealloc: command buffers an app keeps after they complete hold none of the GPU pool's control memory. As many are kept
// as were in flight at once, up to kChunksKept and kChunkBytesKept bytes, each up to kChunkMost (a command buffer of more
// setBytes frees its own, as its recording); gRawRelease gives back all but kChunksIdle.
static constexpr uint32_t kChunksKept = 64, kChunksIdle = 4;
static constexpr uint64_t kChunkMost = 1 << 20, kChunkBytesKept = 16 << 20;
static raw::TakePool<raw::Buffer, kChunksKept> &gBytesChunks = *new raw::TakePool<raw::Buffer, kChunksKept>;
static std::atomic<uint64_t> gBytesChunkBytes{0};

static std::atomic<bool> gBytesGrowing{false};

// The length of the chunk taken last, up to kChunkMost: what a chunk made for the pool, or for a command buffer it had none
// for, is made at. A recording that coalesced past 64 KB (coalesceBytes) given a 64 KB one outgrows it and coalesces
// again at its completion, making and freeing control buffers on the completion thread.
static std::atomic<uint64_t> gBytesChunkLength{kRMBytesChunk};

static void keepBytesChunk(const raw::Buffer &chunk)
{
    uint32_t node;
    const bool fits =
        chunk.va && chunk.length <= kChunkMost &&
        gBytesChunkBytes.fetch_add(chunk.length, std::memory_order_relaxed) + chunk.length <= kChunkBytesKept;
    raw::Buffer *slot = fits ? gBytesChunks.room(node) : nullptr;
    if (!slot) {
        if (chunk.va && chunk.length <= kChunkMost)
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
        RMBytes one;
        one.chunks.push_back(chunk);
        dropBytes(one);
        return;
    }
    *slot = chunk;
    gBytesChunks.keep(node);
}

static void growBytesChunks(void *)
{
    raw::Buffer chunk;
    if (gRM->raw->createControlBuffer(gBytesChunkLength.load(std::memory_order_relaxed), chunk)) {
        RM_CHUNK_COUNT(gChunksMadeAhead, 1);
        RM_CHUNK_COUNT(gChunkBytesLive, chunk.length);
        keepBytesChunk(chunk);
    }
    gBytesGrowing.store(false, std::memory_order_release);
}

// The pool is grown by a chunk once it is down to its last (or had none), on the utility queue: a command buffer recorded
// after a burst takes the one made meanwhile instead of making its own on its recording thread
static void growBytesChunksAhead()
{
    if (gBytesChunks.kept() <= 1 && !gBytesGrowing.exchange(true, std::memory_order_acq_rel))
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), nullptr, growBytesChunks);
}

void rmBytesDone(RMCommandBuffer *cb)
{
    RMBytes &s = cb->_bytes;
    coalesceBytes(s);
    for (const raw::Buffer &k : s.chunks)
        keepBytesChunk(k);
    s.chunks.clear();  // its capacity goes on with the recording to the next command buffer
    s.at = 0;
    s.used = 0;
}

bool rmTakeBytesChunk(uint64_t length, raw::Buffer &chunk)
{
    uint32_t node;
    if (raw::Buffer *kept = gBytesChunks.take(node)) {
        if (kept->length >= length) {
            chunk = *kept;
            *kept = {};
            gBytesChunks.giveBack(node);
            gBytesChunkBytes.fetch_sub(chunk.length, std::memory_order_relaxed);
            gBytesChunkLength.store(std::min(chunk.length, kChunkMost), std::memory_order_relaxed);
            growBytesChunksAhead();
            return true;
        }
        gBytesChunks.keep(node);
    }
    growBytesChunksAhead();
    if (!gRM->raw->createControlBuffer(std::max(length, gBytesChunkLength.load(std::memory_order_relaxed)), chunk))
        return false;
    RM_CHUNK_COUNT(gChunkMisses, 1);
    RM_CHUNK_COUNT(gChunkBytesLive, chunk.length);
    return true;
}

static void giveRecording(RMCommandBuffer *b)
{
    constexpr size_t kKeptBytes = 1 << 20;  // (a command buffer of a few megabytes of setBytes: its own, freed)
    uint64_t bytes = 0;
    for (const raw::Buffer &k : b->_bytes.chunks)
        bytes += k.va ? k.length : kKeptBytes;  // (heap chunks are not kept)
    bytes += b->_plan.storageBytes() + b->_extra.capacity() * 8 + b->_shadow.capacity() +
             b->_segs.capacity() * sizeof(RMSegment);
    if (bytes > kKeptBytes) {
        dropBytes(b->_bytes);
        return;
    }
    uint32_t node;
    RMRecording *r = gRecordingBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes <= kKeptTotal
                         ? gRecordings.room(node)
                         : nullptr;
    if (!r) {
        gRecordingBytes.fetch_sub(bytes, std::memory_order_relaxed);
        dropBytes(b->_bytes);
        return;
    }
    swapRecording(b, *r);
    r->kept = bytes;
    r->plan.clear();
    r->passes.clear();
    r->bytes.at = 0;
    r->bytes.used = 0;
    r->shadow.clear();
    r->extra.clear();
    r->owners.clear();
    r->ownerKeys.clear();
    r->segs.clear();
    coalesceBytes(r->bytes);
    gRecordings.keep(node);
}

void releaseRecordings()
{
    while (gBytesChunks.kept() > kChunksIdle) {
        uint32_t node;
        raw::Buffer *kept = gBytesChunks.take(node);
        if (!kept)
            break;
        gBytesChunkBytes.fetch_sub(kept->length, std::memory_order_relaxed);
        RMBytes one;
        one.chunks.push_back(*kept);
        dropBytes(one);
        *kept = {};
        gBytesChunks.giveBack(node);
    }
    while (gRecordings.kept() > kKeptIdle) {
        uint32_t node;
        RMRecording *r = gRecordings.take(node);
        if (!r)
            break;
        gRecordingBytes.fetch_sub(r->kept, std::memory_order_relaxed);
        dropBytes(r->bytes);
        *r = RMRecording();
        gRecordings.giveBack(node);
    }
}

void rmChanged(RMCommandBuffer *cb)
{
    cb->_changes.changed();
}

NSArray *rmTakeHandlers(RMCommandBuffer *cb, NSMutableArray *__strong &list, std::atomic<bool> &added)
{
    if (!added.load())
        return nil;
    std::lock_guard<raw::Mutex> l(cb->_lock);
    NSArray *taken = list;
    list = nil;
    return taken;
}

template <typename Done> static void waitFor(RMCommandBuffer *cb, Done &&done)
{
    cb->_changes.waitFor(done);
}

static void commandBufferDropped(void *object)
{
    RMCommandBuffer *__unsafe_unretained b = (__bridge RMCommandBuffer *)object;
    giveRecording(b);
}

// Command buffers on the storage their deallocs gave back, as the encoders (raw_object_pool.h): -dealloc is the pool's,
// which gives the recording back first. A sub-encoder's (RMSubRecorder) allocates and frees as NSObject's do, and gives
// its recording back the same.
void rmPoolCommandBuffers()
{
    if (!rawPoolInstances([RMCommandBuffer class], commandBufferDropped))
        os_log_error(rawLog(), "raw: command buffers are not pooled");
}

@implementation RMCommandBuffer

- (void)retain:(id)object
{
    if (!object || !_ownerKeys.insert((__bridge void *)object, true).second)
        return;
    _owners.push_back(object);
    RMResourceFacts facts;
    facts.cpuBuffer = (__bridge void *)cpuBufferOfResource(object);
    if ([object isKindOfClass:[RMBuffer class]])
        facts.kind = RMResourceKind::Buffer;
    else if ([object isKindOfClass:[RMTexture class]]) {
        facts.kind = RMResourceKind::Texture;
        RMTexture *texture = (RMTexture *)object;
        facts.roles = uint8_t((texture->_raw.converted() ? RMResourceFacts::Converted : 0) |
                              (texture->_raw.planarStandIn ? RMResourceFacts::PlanarTarget : 0));
        __unsafe_unretained RMTexture *root = texture;
        while (root->_kind == RMTextureView && root->_parent)
            root = root->_parent;
        facts.surfaceWrites = root->_surfaceWrites;
    } else if ([object isKindOfClass:[RMSampler class]])
        facts.kind = RMResourceKind::Sampler;
    else if ([object isKindOfClass:[RMHeap class]])
        facts.kind = RMResourceKind::Heap;
    _plan.registerResource((__bridge void *)object, facts);
}

- (void)record:(const Cmd &)cmd
{
    if (_commandLog) {
        RMSubCommand s{cmd, {}};
        if (const size_t words = raw::extraWords(cmd))
            s.extra.assign(_extra.begin() + ptrdiff_t(cmd.x), _extra.begin() + ptrdiff_t(cmd.x + words));
        _commandLog->push_back(s);
    }
    if (cmd.op == OpPipeline && _recordedPass < _passes.size() && _passes[_recordedPass].compressed)
        rmNoteStorageTargets(self, _passes[_recordedPass], cmd);
    if (cmd.op == OpCompute)
        _predicates.computeEncoder(cmd.a);
    else if (cmd.op == OpRender || cmd.op == OpBlit || cmd.op == OpSignalEvent || cmd.op == OpWaitEvent)
        _predicates.otherWork();
    if (_plan.record(cmd, _extra)) {
        std::vector<raw::ResourceUse> &uses = _plan.recordUses();
        if (isPlanDraw(cmd.op)) {
            const uint32_t index = _plan.draw(_plan.size() - 1).state;
            const auto &state = _plan.renderStates[index];
            if (!state.usesFinal) {
                const auto &raster = _plan.raster[state.raster];
                RMPipeline *pipeline = (__bridge RMPipeline *)raster.pipeline;
                const raw::Pipeline *native = static_cast<const raw::Pipeline *>(raster.nativePipeline);
                if (!native && [(id)pipeline isKindOfClass:[RMPipeline class]])
                    native = pipeline->_raw;
                if (native && native->rec) {
                    _plan.resolveRenderUses(index, native, [&](std::vector<raw::ResourceUse> &bound) {
                        const auto &record = *native->rec;
                        for (uint32_t i = 0; i < record.entries; ++i) {
                            const auto &entry = record.entry[i];
                            if (entry.kind == raw::EntryUbo && entry.stage >= 1 && entry.stage <= 2 &&
                                entry.slot < 31) {
                                const auto &binding =
                                    _plan.buffers[entry.stage - 1][state.buffers[entry.stage - 1]][entry.slot];
                                if (binding.source == raw::BufferBinding::Source::Buffer)
                                    bound.push_back(
                                        {binding.object,
                                         uint8_t(raw::ResourceUse::Read |
                                                 (raw::bufferStorage(entry) ? raw::ResourceUse::Write : 0))});
                            } else if ((entry.kind == raw::EntryTexture || entry.kind == raw::EntryTexture3d) &&
                                       entry.slot < raw::kStageTextures) {
                                const uint32_t at = raw::handleStage(entry);
                                const bool written = entry.stage == 2 && entry.slot < raw::kWrittenSlots &&
                                                     (native->writtenSlots >> entry.slot & 1);
                                bound.push_back({_plan.textures[at][state.textures[at]].objects[entry.slot],
                                                 uint8_t(raw::ResourceUse::Read | raw::ResourceUse::Sample |
                                                         (written ? raw::ResourceUse::Write : 0))});
                            }
                            if ((entry.kind == raw::EntryTexture || entry.kind == raw::EntryTexture3d ||
                                 entry.kind == raw::EntrySamplerOnly) &&
                                entry.samplerKind == raw::SamplerSlot && entry.sampler < raw::kStageSamplers) {
                                const uint32_t at = raw::handleStage(entry);
                                bound.push_back({_plan.samplers[at][state.samplers[at]].objects[entry.sampler],
                                                 raw::ResourceUse::Read});
                            }
                        }
                        if (record.vertexSlot < 31 && record.vertexStride) {
                            const auto &binding = _plan.buffers[0][state.buffers[0]][record.vertexSlot];
                            if (binding.source == raw::BufferBinding::Source::Buffer)
                                bound.push_back({binding.object, raw::ResourceUse::Read});
                        }
                        // the buffer slots a stage reaches through its root table, which may be written there too
                        for (uint32_t stage = 0; stage < 2; ++stage)
                            for (uint32_t slots = record.rootSlots[stage]; slots; slots &= slots - 1) {
                                const auto &binding = _plan.buffers[stage][state.buffers[stage]][__builtin_ctz(slots)];
                                if (binding.source == raw::BufferBinding::Source::Buffer)
                                    bound.push_back(
                                        {binding.object, uint8_t(raw::ResourceUse::Read | raw::ResourceUse::Write)});
                            }
                    });
                }
            }
        } else if (cmd.op == OpDispatch && cmd.obj2) {
            const char *why = nullptr;
            if (!admitComputeDispatch(self, _plan.size() - 1,
                                      static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(cmd.obj2), &why))
                [self takeReason:WhyCompute why:[NSString stringWithUTF8String:why]];
        } else if (cmd.op == OpRender) {
            _recordedPass = uint32_t(cmd.x);
            const RMPass &pass = _passes[cmd.x];
            for (const RMPass::Color &a : pass.color)
                if (a.texture)
                    uses.push_back({(__bridge void *)a.texture,
                                    uint8_t(raw::ResourceUse::Write |
                                            (a.load == MTLLoadActionLoad ? raw::ResourceUse::Read : 0))});
            if (pass.depth)
                uses.push_back({(__bridge void *)pass.depth,
                                uint8_t(raw::ResourceUse::Write |
                                        (pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad
                                             ? raw::ResourceUse::Read
                                             : 0))});
            for (const RMPass::Color &a : pass.color)
                if (a.resolve)
                    uses.push_back({(__bridge void *)a.resolve, raw::ResourceUse::Write});
            if (pass.depthResolve)
                uses.push_back({(__bridge void *)pass.depthResolve, raw::ResourceUse::Write});
            if (pass.stencilResolve)
                uses.push_back({(__bridge void *)pass.stencilResolve, raw::ResourceUse::Write});
        } else if (cmd.op == OpVisibility && cmd.obj) {
            uses.push_back({cmd.obj, raw::ResourceUse::Write});
        } else if (cmd.op == OpPredicate && cmd.obj) {
            uses.push_back({cmd.obj, raw::ResourceUse::Read});
        } else if (cmd.op == OpInvocation) {
            raw::forEachObject((__bridge NSInvocation *)cmd.obj, [&](id object) {
                uses.push_back({(__bridge void *)object, raw::ResourceUse::Read | raw::ResourceUse::Write});
                [self retain:object];
            });
        }
        if (!uses.empty())
            _plan.addUses(_plan.size() - 1, uses);
        if (cmd.op == OpEnd && _encOpen)
            _plan.prepareRange(_encAt, _plan.size());
    }
    const size_t consumed = raw::extraWords(cmd);
    if (consumed && cmd.x + consumed == _extra.size())
        _extra.resize(cmd.x);
}

// The encoder recorded last: whether it goes to m2v, into its first command
- (void)closeEncoder
{
    if (_encOpen && _encAt < _plan.size())
        _plan.setRefused(_encAt, _encRefused);
    _encOpen = false;
}

- (void)takeReason:(RMReason)reason why:(NSString *)why
{
    if (_active)
        _encRefused = true;  // this encoder goes to m2v
    else
        _whole = true;
    if (_reason != WhyRaw)
        return;
    _reason = reason;
    _why = why;
}

// A refusal of the command buffer's own (an event m2v must see, a call on the command buffer): the whole of it goes to m2v
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why
{
    _whole = true;
    [self takeReason:reason why:why];
}

// Answered here, not recorded as invocations (each one sent the whole command buffer to m2v)
- (NSMutableDictionary *)userDictionary
{
    std::lock_guard<raw::Mutex> l(_lock);
    if (!_userDictionary)
        _userDictionary = [NSMutableDictionary dictionary];
    return _userDictionary;
}

- (uint64_t)globalTraceObjectID
{
    std::lock_guard<raw::Mutex> l(_lock);
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

- (void)useResidencySet:(id)set
{
    _residency.add(&set, 1);
}

// (QuartzCore 27, per command buffer: the kernel charges GPU time to the task that submits it, this one)
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (id)rmTwin
{
    return nil;
}

- (MTLCommandBufferStatus)status
{
    return _status.load();
}

- (NSError *)error
{
    return _error;
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}

- (id<MTLLogContainer>)logs
{
    return nil;
}

- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}

- (CFTimeInterval)kernelStartTime
{
    return _kernelStart;
}

- (CFTimeInterval)kernelEndTime
{
    return _kernelEnd;
}

// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}

- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    _hasScheduledHandlers.store(true);
    {
        std::lock_guard<raw::Mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        if (!_scheduledSent) {
            if (!_scheduledHandlers)
                _scheduledHandlers = [NSMutableArray array];
            [_scheduledHandlers addObject:[block copy]];
        } else {
            _handlersQueued = true;
            dispatch_async(_queue->_handlers, ^{
                block((id<MTLCommandBuffer>)self);
            });
        }
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    _hasCompletedHandlers.store(true);
    {
        std::lock_guard<raw::Mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now) {
            if (!_completedHandlers)
                _completedHandlers = [NSMutableArray array];
            [_completedHandlers addObject:[block copy]];
        }
    }
    if (now)
        block((id<MTLCommandBuffer>)self);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAtTime:t];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    _presents = true;
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAfterMinimumDuration:d];
    }];
}

- (void)enqueue
{
    enqueueBuffer(self);
}

- (void)commit
{
    [self enqueue];
    if (_committed.exchange(true))
        return;
    _status = MTLCommandBufferStatusCommitted;
    _kernelStart = hostSeconds();
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const raw::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed), gTwinsMade.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(RMEncoder *)_active endEncoding];
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    glassProbeCommitted(self);
#endif
    [(RMDevice *)_device rmCommit:self];
#if SWITCH_FRAME_TIMELINE
    _frame.mark(switch_frame::Back);
#endif
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}

- (void)waitUntilScheduled
{
    auto returns = [&] {
        return raw::scheduledWaitReturns(_scheduledDone, _held, _segError.set());
    };
    if (_committed && !returns()) {
#if SWITCH_RAW_DIAGNOSTICS
        const BlockedWait blocked(gRM->stats.scheduledWaits, gRM->stats.scheduledWaitUs);
#endif
        waitFor(self, returns);
    }
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t none = 0;
    if (_held && !_scheduledDone && _heldReturnUs.compare_exchange_strong(none, nowUs()))
        gRM->stats.heldReturns++;
#endif
}

- (void)waitUntilCompleted
{
    if (!_committed || _completedDone)
        return;
#if SWITCH_RAW_DIAGNOSTICS
    const BlockedWait blocked(gRM->stats.completedWaits, gRM->stats.completedWaitUs);
#endif
    // a range of it in flight may have ended quiet: the completion thread waits for the newest one
    gRM->completionWaiters.fetch_add(1, std::memory_order_release);
    gRM->inflight.wake();
    waitFor(self, [&] {
        return _completedDone.load();
    });
    gRM->completionWaiters.fetch_sub(1, std::memory_order_relaxed);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
- (NSNumber *)switchRawWholePrograms
{
    std::lock_guard<raw::Mutex> lock(_lock);
    if (_committed)
        return @NO;
    _forceWholePrograms = true;
    return @YES;
}

// hw_test_app's gpu.path.state_segments: this command buffer binds every state as an inline copy (raw_gpu.h inlineStates)
- (NSNumber *)switchRawInlineStates
{
    std::lock_guard<raw::Mutex> lock(_lock);
    if (_committed)
        return @NO;
    _inlineStates = true;
    return @YES;
}

// hw_test_app's clock of GPUStartTime/GPUEndTime, final once the command buffer completed: "ns" (its submission's GPU
// stamps) or "host" (the host's window from submission to completion, which stands in where no stamp was read)
- (NSString *)switchGpuClock
{
    if (!_completedSent)
        return @"pending";
    return _gpuStamped ? @"ns" : @"host";
}

// hw_test_app's attribution, final once the command buffer completed (its completed handlers may ask): "raw",
// "m2v:<reason>:<first predicate>" or "split:<raw segments>+<m2v segments>:<reason>:<first predicate>". With no lock:
// what it reads is set before the completed mark this acquires.
- (NSString *)switchRawPath
{
    if (!_completedSent)
        return @"pending";
    raw::ExecutionAttribution route;
    for (const RMSegment &range : _segs)
        route.add(range.raw, range.event);
    if (route.mixed())
        return [NSString stringWithFormat:@"split:%zu+%zu:%s:%s", route.native, route.compatibility,
                                          kReasonNames[_reason], kPredNames[_pred0]];
    if (!route.onlyCompatibility() && !_m2v)
        return @"raw";
    return [NSString stringWithFormat:@"m2v:%s:%s", kReasonNames[_reason], kPredNames[_pred0]];
}

// facadecost's held-chunk check: the GPU addresses of its setBytes chunks, read while its job is held in flight (they go
// back at its completion)
- (NSArray<NSNumber *> *)switchRawBytesChunks
{
    NSMutableArray<NSNumber *> *vas = [NSMutableArray array];
    for (const raw::Buffer &k : _bytes.chunks)
        [vas addObject:@(k.va)];
    return vas;
}

// facadecost's: the setBytes chunks' misses and those made ahead since the process began, and their live bytes now
+ (NSDictionary<NSString *, NSNumber *> *)switchRawBytesChunkStats
{
    return @{
        @"misses" : @(gChunkMisses.load(std::memory_order_relaxed)),
        @"made_ahead" : @(gChunksMadeAhead.load(std::memory_order_relaxed)),
        @"live_bytes" : @(gChunkBytesLive.load(std::memory_order_relaxed)),
    };
}
#endif

// A selector a command buffer does not answer here: recorded for m2v (the command buffer goes there), answered zero; when m2v's
// command buffer has no method for it either, only answered zero.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = NSClassFromString(@"NBCommandBuffer");
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = NSClassFromString(@"NBCommandBuffer");
    return c && [c instancesRespondToSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    if (!twinAnswers(NSClassFromString(@"NBCommandBuffer"), inv.selector)) {
        answerZero(self, inv);
        return;
    }
    [inv retainArguments];
    if (!raw::keepPointedArguments(inv))
        SWITCH_REFUSE_DETAIL("raw", "call.pointer", uintptr_t(inv.selector),
                             "-[MTLCommandBuffer %s] recorded with a pointer argument of no known extent",
                             sel_getName(inv.selector));
    [self retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    c.stage = 0xff;  // the command buffer's own, not an encoder's
    [self record:c];
    [self takeWholeReason:WhyCall why:@"a command buffer call recorded as an invocation"];
    hit(self, PredCallCBInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.cbinvocation", uintptr_t(inv.selector), "-[MTLCommandBuffer %s] answered zero",
                         sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

@end

@implementation RMQueue

+ (Class)twinClass
{
    return NSClassFromString(@"NBQueue");
}

- (instancetype)init
{
    if ((self = [super init])) {
        queuePriorityAdd(self);
        _handlers = followingSerialQueue("com.brunostjohn.switch.raw.command.handlers");
        _urges = followingSerialQueue("com.brunostjohn.switch.raw.command.urge");
    }
    return self;
}

- (void)dealloc
{
    queuePriorityRemove(self);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [RM_M2V("a command queue for a command buffer replayed on it") newCommandQueue]);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    RMCommandBuffer *b = [RMCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_status = MTLCommandBufferStatusNotEnqueued;
    takeRecording(b);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (raw_gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

- (void)addResidencySet:(id)set
{
    _residency.add(&set, 1);
}

- (void)addResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (void)removeResidencySet:(id)set
{
    _residency.remove(&set, 1);
}

- (void)removeResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.remove(sets, count);
}

- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}

- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}

@end
