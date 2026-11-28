// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "object_pool.h"
#include "parallel.h"
#include "compute.h"
#include "scheduled_wait.h"
#import "TegraGM20BMetalPipeline.h"
#import "TegraGM20BMetalCounters.h"
#include "counter_kernel.h"

// ---- the device's submission machinery ----

void gm20bChanged(SwitchMetalCommandBuffer *cb)
{
    cb->_changes.changed();
}

void gm20bSetCompileWaits(SwitchMetalCommandBuffer *cb, std::vector<std::function<bool()>> waits)
{
    {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        cb->_compileWaits = std::move(waits);
    }
    cb->_compileWaitGen.fetch_add(1, std::memory_order_acq_rel);
    cb->_submitOwner.clearIf(gRM->submitThread.load(std::memory_order_relaxed));
    gm20bChanged(cb);
}

void gm20bClearCompileWaits(SwitchMetalCommandBuffer *cb)
{
    std::vector<std::function<bool()>> cleared;
    {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        cleared.swap(cb->_compileWaits);
    }
}

void gm20bNoteCompileWaitReady(SwitchMetalCommandBuffer *cb)
{
    cb->_compileWaitGen.fetch_add(1, std::memory_order_acq_rel);
    gm20bChanged(cb);
}

static std::vector<std::function<bool()>> copyCompileWaits(SwitchMetalCommandBuffer *cb)
{
    std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
    return cb->_compileWaits;
}

NSArray *gm20bTakeHandlers(SwitchMetalCommandBuffer *cb, NSMutableArray *__strong &list, std::atomic<bool> &added)
{
    if (!added.load())
        return nil;
    std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
    NSArray *taken = list;
    list = nil;
    return taken;
}

// Decided where its scheduled handlers are sent, not where they run: one the GPU fails after its submission still
// presents (the kernel ends that present), one that failed before presents nothing (TegraGM20BMetalFinish.mm withholdPresents)
static bool presents(id<MTLCommandBuffer> cb)
{
    return ((SwitchMetalCommandBuffer *)cb)->_presents;
}

template <typename Done> static void waitFor(SwitchMetalCommandBuffer *cb, Done &&done)
{
    uint32_t triedCompileGen = 0;
    uint32_t failedOwner = 0;
    for (;;) {
        if (done())
            return;
        const uint32_t gen = cb->_compileWaitGen.load(std::memory_order_acquire);
        if (gen != 0 && gen != triedCompileGen) {
            bool retry = false;
            for (const auto &wait : copyCompileWaits(cb)) {
                if (done())
                    return;
                if (wait && wait()) {
                    retry = true;
                    break;
                }
            }
            if (!retry)
                triedCompileGen = gen;
            continue;
        }
        const uint32_t owner = cb->_submitOwner.owner();
        const uint32_t self = gm20b::OwnedWord::currentOwner();
        if (owner && owner != failedOwner && owner != self) {
            if (cb->_submitOwner.waitOnce(done))
                continue;
            if (cb->_submitOwner.owner() == owner)
                failedOwner = owner;
            continue;
        }
        cb->_changes.waitOnce([&] {
            const uint32_t curOwner = cb->_submitOwner.owner();
            const bool usableOwner = curOwner && curOwner != failedOwner && curOwner != self;
            return done() || usableOwner || cb->_compileWaitGen.load(std::memory_order_acquire) != triedCompileGen;
        });
    }
}

// On the thread that let go of the last reference, which may not be the one that completed it. The completion's last
// write is its turn's mark (completeBuffer: its recording given back before), and its reference went after it; read
// with acquire before anything here, so this dealloc and whoever takes the block from the pool next come after that
// write through the command buffer's own atomic, not only through libobjc's reference count
static void commandBufferDropped(void *object)
{
    SwitchMetalCommandBuffer *__unsafe_unretained b = (__bridge SwitchMetalCommandBuffer *)object;
    const bool completed = b->_turn.ran();
    if (SwitchMetalEncoder *__unsafe_unretained e = (SwitchMetalEncoder *)b->_active)
        e->_cb = nil;
    b->_active = nil;
    if (!completed) {
        b->_deferredPresents.clear();
        b->_residencySets = nil;
        b->_residencySnapshots.clear();
        gm20bRecordingDone(b);
    }
}

// Command-buffer objects use the same instance pool as encoders (object_pool.h). Dealloc returns an unsubmitted
// recording before pooling its object. Sub-encoders (SwitchMetalSubRecorder) use NSObject allocation and return their recordings
// there too.
void gm20bPoolCommandBuffers()
{
    if (!gm20bPoolInstances([SwitchMetalCommandBuffer class], commandBufferDropped))
        os_log_error(gm20bLog(), "SwitchMetal: command buffers are not pooled");
}

@implementation SwitchMetalCommandBuffer

- (void)retain:(__unsafe_unretained id)object
{
    if (!object || !_ownerKeys.insert((__bridge void *)object, true).second)
        return;
    _owners.push_back(object);
    SwitchMetalResourceFacts facts;
    if (__unsafe_unretained SwitchMetalTexture *texture = gm20bAsTexture(object)) {
        facts.kind = SwitchMetalResourceKind::Texture;
        facts.roles = uint8_t(texture->_raw.converted() ? SwitchMetalResourceFacts::Converted : 0);
        __unsafe_unretained SwitchMetalTexture *root = texture->_levelsOwner ?: texture;
        facts.surfaceWrites = root->_surfaceWrites;
        facts.cpuBuffer = (__bridge void *)root->_buffer;
    } else if (gm20bAsBuffer(object)) {
        facts.kind = SwitchMetalResourceKind::Buffer;
        facts.cpuBuffer = (__bridge void *)object;
    } else if (gm20bAsSampler(object))
        facts.kind = SwitchMetalResourceKind::Sampler;
    else if (gm20bAsHeap(object))
        facts.kind = SwitchMetalResourceKind::Heap;
    _plan.registerResource((__bridge void *)object, facts);
}

// A binding's state variant still being made (OpPipeline's obj2): an encode ahead would draw its draws with no state, as
// RenderEncoder::draw drops a draw without one; the commit's submission waits for it (TegraGM20BMetalSchedule.mm variantReadiness)
static bool bindsPendingVariant(const Cmd &binding)
{
    const auto *variant = static_cast<const gm20b::Variant<gm20b::Pipeline> *>(binding.obj2);
    return variant && variant->state() == gm20b::Variant<gm20b::Pipeline>::State::Pending;
}

// The raw state a draw of this raster state runs
static const gm20b::Pipeline *recordedPipeline(const gm20b::RasterState &raster)
{
    if (const gm20b::Pipeline *native = static_cast<const gm20b::Pipeline *>(raster.nativePipeline))
        return native;
    __unsafe_unretained SwitchMetalPipeline *pipeline = gm20bAsPipeline((__bridge id)raster.pipeline);
    return pipeline ? pipeline->_raw : nullptr;
}

static void resolveDrawUses(__unsafe_unretained SwitchMetalCommandBuffer *cb, const gm20b::Pipeline *native,
                            bool captures)
{
    auto &plan = cb->_plan;
    const uint32_t index = plan.ownDraw(plan.size() - 1).state;
    const auto &state = plan.renderStates.own(index);
    if (state.usesFinal)
        return;
    if (!captures)
        native = recordedPipeline(plan.raster.own(state.raster));
    if (!native || !native->rec)
        return;
    plan.resolveRenderUses(index, native, [&](std::vector<gm20b::ResourceUse> &bound) {
        const auto &record = *native->rec;
        const gm20b::SetEntries &entries = native->entries;
        for (uint32_t i = 0; i < entries.size(); ++i) {
            const auto &entry = entries[i];
            if (entry.kind == gm20b::EntryUbo && entry.stage >= 1 && entry.stage <= 2 && entry.slot < 31) {
                const auto &binding = plan.buffers[entry.stage - 1].current()[entry.slot];
                if (binding.source == gm20b::BufferBinding::Source::Buffer)
                    bound.push_back(
                        {binding.object,
                         uint8_t(gm20b::ResourceUse::Read |
                                 (gm20b::bufferStorage(entry) ? gm20b::ResourceUse::Write : 0))});
            } else if ((entry.kind == gm20b::EntryTexture || entry.kind == gm20b::EntryTexture3d) &&
                       entry.slot < gm20b::kStageTextures) {
                const uint32_t at = gm20b::handleStage(entry);
                const bool written =
                    entry.stage == 2 && entry.slot < gm20b::kWrittenSlots && (native->writtenSlots >> entry.slot & 1);
                bound.push_back({plan.textures[at].current().objects[entry.slot],
                                 uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample |
                                         (written ? gm20b::ResourceUse::Write : 0))});
            }
            if ((entry.kind == gm20b::EntryTexture || entry.kind == gm20b::EntryTexture3d ||
                 entry.kind == gm20b::EntrySamplerOnly) &&
                entry.samplerKind == gm20b::SamplerSlot && entry.sampler < gm20b::kStageSamplers) {
                const uint32_t at = gm20b::handleStage(entry);
                bound.push_back({plan.samplers[at].current().objects[entry.sampler], gm20b::ResourceUse::Read});
            }
        }
        if (record.vertexSlot < 31 && record.vertexStride) {
            const auto &binding = plan.buffers[0].current()[record.vertexSlot];
            if (binding.source == gm20b::BufferBinding::Source::Buffer)
                bound.push_back({binding.object, gm20b::ResourceUse::Read});
        }
        // the buffer slots a stage reaches through its root table, which may be written there too
        for (uint32_t stage = 0; stage < 2; ++stage)
            for (uint32_t slots = record.rootSlots[stage]; slots; slots &= slots - 1) {
                const auto &binding = plan.buffers[stage].current()[__builtin_ctz(slots)];
                if (binding.source == gm20b::BufferBinding::Source::Buffer)
                    bound.push_back({binding.object, uint8_t(gm20b::ResourceUse::Read | gm20b::ResourceUse::Write)});
            }
    });
}

void gm20bRecordDraw(__unsafe_unretained SwitchMetalCommandBuffer *cb, uint8_t primitive, uint32_t start,
                     uint32_t count, uint32_t instances, uint32_t base, const gm20b::Pipeline *native)
{
    const bool captures = cb->_plan.capturesRender();
    cb->_plan.recordDraw(primitive, start, count, instances, base, captures && native && native->rec);
    resolveDrawUses(cb, native, captures);
}

void gm20bBeginKeptRender(__unsafe_unretained SwitchMetalCommandBuffer *cb,
                          __unsafe_unretained MTLRenderPassDescriptor *pass, uint32_t passAt)
{
    cb->_predicates.otherWork();
    cb->_plan.recordRenderBegin((__bridge void *)pass, passAt);
    cb->_recordedPass = passAt;
    std::vector<gm20b::ResourceUse> &uses = cb->_plan.recordUses();
    const SwitchMetalPass &facts = cb->_passes.own(passAt);
    for (uint8_t mask = facts.colorMask; mask; mask &= uint8_t(mask - 1)) {
        const SwitchMetalPass::Color &a = facts.color[__builtin_ctz(mask)];
        uses.push_back({(__bridge void *)a.texture,
                        uint8_t(gm20b::ResourceUse::Write | (a.load == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
        if (a.resolve)
            uses.push_back({(__bridge void *)a.resolve, gm20b::ResourceUse::Write});
    }
    if (facts.depth)
        uses.push_back({(__bridge void *)facts.depth,
                        uint8_t(gm20b::ResourceUse::Write |
                                (facts.depthLoad == MTLLoadActionLoad || facts.stencilLoad == MTLLoadActionLoad
                                     ? gm20b::ResourceUse::Read
                                     : 0))});
    if (id stencil = facts.zeta[1].texture; stencil && stencil != facts.depth)
        uses.push_back({(__bridge void *)stencil,
                        uint8_t(gm20b::ResourceUse::Write |
                                (facts.stencilLoad == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
    if (facts.depthResolve)
        uses.push_back({(__bridge void *)facts.depthResolve, gm20b::ResourceUse::Write});
    if (facts.stencilResolve)
        uses.push_back({(__bridge void *)facts.stencilResolve, gm20b::ResourceUse::Write});
    if (!uses.empty())
        cb->_plan.addUses(cb->_plan.size() - 1, uses);
}

void gm20bEndEncoder(__unsafe_unretained SwitchMetalCommandBuffer *cb)
{
    cb->_plan.recordEnd();
    if (cb->_encOpen) {
        if (gm20bAheadReads(cb))
            gm20bPrepareEncoder(cb, cb->_encAt, cb->_plan.size());
        cb->_encOpen = false;
        gm20bPublishEncoder(cb);
    }
}

- (void)record:(const Cmd &)cmd
{
    if (_commandLog) {
        SwitchMetalSubCommand s{cmd, {}};
        if (const size_t words = gm20b::extraWords(cmd))
            s.extra.assign(_extra.ownData() + cmd.x, _extra.ownData() + cmd.x + words);
        _commandLog->push_back(s);
    }
    if (cmd.op == OpPipeline && bindsPendingVariant(cmd))
        gm20bPipelineStop(self, VetoVariant);
    if (cmd.op == OpCompute)
        _predicates.computeEncoder(cmd.a);
    else if (cmd.op == OpRender || cmd.op == OpBlit || cmd.op == OpSignalEvent || cmd.op == OpWaitEvent)
        _predicates.otherWork();
    const bool draws = isPlanDraw(cmd.op), captures = draws && _plan.capturesRender();
    const gm20b::Pipeline *native = captures ? recordedPipeline(_plan.raster.current()) : nullptr;
    if (_plan.record(cmd, _extra.ownData(), _extra.size(), native && native->rec)) {
        std::vector<gm20b::ResourceUse> &uses = _plan.recordUses();
        if (draws) {
            resolveDrawUses(self, native, captures);
        } else if (cmd.op == OpDispatch && cmd.obj2) {
            admitComputeDispatch(self, _plan.size() - 1,
                                 static_cast<const gm20b::ComputeKernelSet<gm20b::ComputeState> *>(cmd.obj2), nullptr);
        } else if (cmd.op == OpRender) {
            _recordedPass = uint32_t(cmd.x);
            const SwitchMetalPass &pass = _passes.own(cmd.x);
            for (uint8_t mask = pass.colorMask; mask; mask &= uint8_t(mask - 1)) {
                const SwitchMetalPass::Color &a = pass.color[__builtin_ctz(mask)];
                uses.push_back({(__bridge void *)a.texture,
                                uint8_t(gm20b::ResourceUse::Write |
                                        (a.load == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
                if (a.resolve)
                    uses.push_back({(__bridge void *)a.resolve, gm20b::ResourceUse::Write});
            }
            if (pass.depth)
                uses.push_back({(__bridge void *)pass.depth,
                                uint8_t(gm20b::ResourceUse::Write |
                                        (pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad
                                             ? gm20b::ResourceUse::Read
                                             : 0))});
            if (id stencil = pass.zeta[1].texture; stencil && stencil != pass.depth)
                uses.push_back({(__bridge void *)stencil,
                                uint8_t(gm20b::ResourceUse::Write |
                                        (pass.stencilLoad == MTLLoadActionLoad ? gm20b::ResourceUse::Read : 0))});
            if (pass.depthResolve)
                uses.push_back({(__bridge void *)pass.depthResolve, gm20b::ResourceUse::Write});
            if (pass.stencilResolve)
                uses.push_back({(__bridge void *)pass.stencilResolve, gm20b::ResourceUse::Write});
        } else if (cmd.op == OpVisibility && cmd.obj) {
            uses.push_back({cmd.obj, gm20b::ResourceUse::Write});
        } else if (cmd.op == OpPredicate && cmd.obj) {
            uses.push_back({cmd.obj, gm20b::ResourceUse::Read});
        }
        if (!uses.empty())
            _plan.addUses(_plan.size() - 1, uses);
        if (cmd.op == OpEnd && _encOpen) {
            if (gm20bAheadReads(self))
                gm20bPrepareEncoder(self, _encAt, _plan.size());
            [self closeEncoder];
            gm20bPublishEncoder(self);
        }
    }
    const size_t consumed = gm20b::extraWords(cmd);
    if (consumed && cmd.x + consumed == _extra.size())
        _extra.resize(cmd.x);
}

- (void)recordBinding:(const Cmd &)cmd
{
    if (_commandLog)
        _commandLog->push_back({cmd, {}});
    _plan.bind(cmd, _extra.ownData());
}

// The encoder recorded last is done: an OpEnd recorded after this publishes nothing
- (void)closeEncoder
{
    _encOpen = false;
}

- (NSMutableDictionary *)userDictionary
{
    std::lock_guard<gm20b::UnfairMutex> l(_lock);
    if (!_userDictionary)
        _userDictionary = [NSMutableDictionary dictionary];
    return _userDictionary;
}

- (uint64_t)globalTraceObjectID
{
    std::lock_guard<gm20b::UnfairMutex> l(_lock);
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

// Every allocation is mapped in the one GPU VM and stays resident; what the set's host-visible allocations ask of the
// command buffer's jobs is gm20bNameResidencySets'
- (void)useResidencySet:(id)set
{
    gm20bUseResidencySet(self, set);
}

// (QuartzCore 27, per command buffer: the kernel charges GPU time to the task that submits it, this one)
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    for (NSUInteger k = 0; k < count; k++)
        gm20bUseResidencySet(self, sets[k]);
}

- (MTLCommandBufferStatus)status
{
    return _status.load(std::memory_order_acquire);
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

// Before its commit a command buffer's handler lists are its recording thread's alone (Metal takes handlers before the
// commit), published to the submit worker by the commit's hand-off with no lock; one added after the commit takes the lock
// its consumer takes them under
- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    if (!_committed.load(std::memory_order_relaxed)) {
        if (!_scheduledHandlers)
            _scheduledHandlers = [NSMutableArray array];
        [_scheduledHandlers addObject:[block copy]];
        _hasScheduledHandlers.store(true, std::memory_order_relaxed);
        return;
    }
    _hasScheduledHandlers.store(true);
    {
        std::lock_guard<gm20b::UnfairMutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        if (!_scheduledSent) {
            if (!_scheduledHandlers)
                _scheduledHandlers = [NSMutableArray array];
            [_scheduledHandlers addObject:[block copy]];
        } else {
            _handlersQueued = true;
            MTLCommandBufferHandler copy = [block copy];
            _queue->_handlerExec.enqueue([self, copy] {
                @autoreleasepool {
                    copy((id<MTLCommandBuffer>)self);
                }
            });
        }
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    if (!_committed.load(std::memory_order_relaxed)) {
        if (!_completedHandlers)
            _completedHandlers = [NSMutableArray array];
        [_completedHandlers addObject:[block copy]];
        _hasCompletedHandlers.store(true, std::memory_order_relaxed);
        return;
    }
    bool now;
    _hasCompletedHandlers.store(true);
    {
        std::lock_guard<gm20b::UnfairMutex> l(_lock);
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

static void deferPresent(SwitchMetalCommandBuffer *cb, id<MTLDrawable> drawable, SwitchMetalDeferredPresent::Kind kind,
                         CFTimeInterval value)
{
    if (!drawable)
        return;
    if (!cb->_committed.load(std::memory_order_relaxed)) {
        cb->_presents.store(true, std::memory_order_relaxed);
        cb->_deferredPresents.push_back({drawable, kind, value});
        cb->_hasDeferredPresents.store(true, std::memory_order_relaxed);
        return;
    }
    cb->_hasDeferredPresents.store(true, std::memory_order_seq_cst);
    bool completed = false;
    bool failed = false;
    bool wake = false;
    {
        std::lock_guard<gm20b::UnfairMutex> l(cb->_lock);
        completed = cb->_completedSent.load(std::memory_order_seq_cst);
        if (!completed) {
            cb->_presents.store(true, std::memory_order_release);
            cb->_deferredPresents.push_back({drawable, kind, value});
            if (!cb->_latePresent) {
                cb->_latePresent = true;
                gRM->completionWaiters.fetch_add(1, std::memory_order_seq_cst);
                wake = true;
            }
        } else {
            failed = cb->_error != nil;
        }
    }
    if (wake)
        gRM->inflight.wake();
    else if (completed && !failed) {
        dispatch_queue_t q = cb->_queue->_completionQueue;
        SwitchMetalDeferredPresent p{drawable, kind, value};
        dispatch_async(q ?: followingRootQueue(), ^{
            gm20bRunDeferredPresent(p);
        });
    }
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    if (gRM->raw->role() == gm20b::Device::Role::RenderServer) {
        _presents = true;
        [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
            if (presents(cb))
                [drawable present];
        }];
        return;
    }
    deferPresent(self, drawable, SwitchMetalDeferredPresent::Kind::Immediate, 0);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    if (gRM->raw->role() == gm20b::Device::Role::RenderServer) {
        _presents = true;
        [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
            if (presents(cb))
                [drawable presentAtTime:t];
        }];
        return;
    }
    deferPresent(self, drawable, SwitchMetalDeferredPresent::Kind::AtTime, t);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    if (gRM->raw->role() == gm20b::Device::Role::RenderServer) {
        _presents = true;
        [self addScheduledHandler:^(id<MTLCommandBuffer> cb) {
            if (presents(cb))
                [drawable presentAfterMinimumDuration:d];
        }];
        return;
    }
    deferPresent(self, drawable, SwitchMetalDeferredPresent::Kind::AfterMinimumDuration, d);
}

- (void)enqueue
{
#if SWITCH_METAL_STATS
    gm20bNoteEnqueue(self);
#endif
    enqueueBuffer(self);
}

- (void)commit
{
    if (_committed.exchange(true, std::memory_order_acq_rel))
        return;
    gm20bClaimCommitSlot(self);
    _status.store(MTLCommandBufferStatusCommitted, std::memory_order_release);
    _kernelStart = hostSeconds();
#if SWITCH_METAL_DIAGNOSTICS
    _committedTicks = mach_absolute_time();
#endif
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const gm20b::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(SwitchMetalEncoder *)_active endEncoding];
    gm20bSnapshotResidencySets(self);
    [(SwitchMetalDevice *)_device gm20bCommit:self];
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
    // A failed one returns once finish() withdrew the writes it will never make (TegraGM20BMetalSchedule.mm cancelPromises), so
    // a present its caller makes next of a surface it was to write is refused, never shown with the frame before
    auto returns = [&] {
        return gm20b::scheduledWaitReturns(_scheduledDone.load(std::memory_order_acquire),
                                           _segError.set() && _promised.load(std::memory_order_acquire));
    };
    if (_committed.load(std::memory_order_acquire) && !returns()) {
#if SWITCH_FRAME_TIMELINE
        const switch_frame::WaitScope frameWait(_frame);
#endif
#if SWITCH_METAL_STATS
        const BlockedWait blocked(gRM->stats.scheduledWaits, gRM->stats.scheduledWaitUs);
        const uint64_t from = nowUs();
#endif
        waitFor(self, returns);
#if SWITCH_METAL_STATS
        gm20bNoteScheduledWait(self, from, nowUs());
#endif
    }
#if SWITCH_METAL_STATS
    uint64_t none = 0;
    if (_held.load(std::memory_order_acquire) && !_scheduledDone.load(std::memory_order_acquire) &&
        _heldReturnUs.compare_exchange_strong(none, nowUs(), std::memory_order_acq_rel, std::memory_order_relaxed))
        gRM->stats.heldReturns++;
#endif
}

- (void)waitUntilCompleted
{
    if (!_committed.load(std::memory_order_acquire) || _completedDone.load(std::memory_order_acquire))
        return;
#if SWITCH_METAL_STATS
    const BlockedWait blocked(gRM->stats.completedWaits, gRM->stats.completedWaitUs);
#endif
    // a range of it in flight may have ended quiet: the completion thread waits for the newest one
    gRM->completionWaiters.fetch_add(1, std::memory_order_seq_cst);
    gRM->inflight.wake();
    waitFor(self, [&] {
        return _completedDone.load(std::memory_order_acquire);
    });
    gRM->completionWaiters.fetch_sub(1, std::memory_order_relaxed);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
- (NSNumber *)switchMetalWholePrograms
{
    std::lock_guard<gm20b::UnfairMutex> lock(_lock);
    if (_committed.load(std::memory_order_relaxed))
        return @NO;
    gm20bPipelineVeto(self, VetoProbes);
    _forceWholePrograms = true;
    return @YES;
}

// hw_test_app's gpu.path.state_segments: this command buffer binds every state as an inline copy (command_buffer.h
// inlineStates)
- (NSNumber *)switchMetalInlineStates
{
    std::lock_guard<gm20b::UnfairMutex> lock(_lock);
    if (_committed.load(std::memory_order_relaxed))
        return @NO;
    gm20bPipelineVeto(self, VetoProbes);
    _inlineStates = true;
    return @YES;
}

// hw_test_app's clock of GPUStartTime/GPUEndTime, final once the command buffer completed: "ns" (its submission's GPU
// stamps) or "host" (the host's window from submission to completion, which stands in where no stamp was read)
- (NSString *)switchGpuClock
{
    if (!_completedSent.load(std::memory_order_acquire))
        return @"pending";
    return _gpuStamped ? @"ns" : @"host";
}

// hw_test_app's attribution, final once the command buffer completed (its completed handlers may ask): "gm20b", or for
// one that completed with an error "error:<predicate>", the predicate its failure named, "error:none" where it named
// none (the failed.* key switchMetalCounters counted it under). With no lock: what it reads is set before the completed
// mark this acquires.
- (NSString *)switchMetalPath
{
    if (!_completedSent.load(std::memory_order_acquire))
        return @"pending";
    if (!_error)
        return @"gm20b";
    return [NSString stringWithFormat:@"error:%s", failedName(SwitchMetalPred(_failedBy))];
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// Host tests' wait for the submit worker, before its commit: returns once the worker encoded ahead everything it
// published so far up to where it stopped, or gave it back (answering the reasons given back, 0 where none), once it
// was handed the command buffer. A publish too small to hand it over again (TegraGM20BMetalPipeline.mm kHandOff) and a
// recorded veto wake no worker asleep, so the wait wakes it: the semaphore keeps the wake for a worker about to sleep.
- (NSNumber *)switchMetalWaitAhead
{
    const auto done = [&] {
        size_t until = size_t(_published.load(std::memory_order_acquire) >> 32);
        if (const size_t stop = _aheadStop.load(std::memory_order_acquire))
            until = std::min(until, stop);
        if (const size_t halt = _aheadHalt.load(std::memory_order_acquire))
            until = std::min(until, halt);
        return _pipelineDeclined.load(std::memory_order_acquire) || _aheadAt.load(std::memory_order_acquire) >= until;
    };
    gRM->schedule.wake();
    waitFor(self, done);
    return @(_pipelineDeclined.load(std::memory_order_acquire) ? _pipelineGivenBack : 0u);
}
#endif

// hw_test_app's gpu.path.commit_latency, final once it completed: of its raw ranges' encoders, those the worker encoded
// ahead of its commit; the encodes ahead of it the worker gave back and why (given_back), the reasons the recording gave
// (veto, both SwitchMetalPipelineVeto); and, while its stamps' slot still holds them (kStampSlots command buffers on), the time
// from its commit to its submission's start on the GPU (its start stamp's PTIMER on the CPU's clock through the
// kernel's anchors)
- (NSDictionary<NSString *, NSNumber *> *)switchMetalPipelined
{
    if (!_completedSent.load(std::memory_order_acquire))
        return nil;
    NSMutableDictionary *out = [@{
        @"encoders" : @(_pipelineEncoders),
        @"pipelined" : @(_pipelinedEncoders),
        @"abandons" : @(_pipelineAbandons),
        @"veto" : @(_pipelineVeto.load(std::memory_order_relaxed)),
        @"stopped" : @(_aheadStopped.load(std::memory_order_relaxed)),
        @"given_back" : @(_pipelineGivenBack),
    } mutableCopy];
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t units = 0;
    const TegraGM20BAbiClockAnchors anchors = gm20bClockAnchorsNow();
    if (gRM->raw->gpuStartUnits(_firstStamp, units))
        if (const gm20b::counters::Anchor a = gm20b::counters::anchorOf(anchors, units); a.valid) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            const double committedNs = double(_committedTicks) * tb.numer / tb.denom;
            out[@"commit_to_gpu_start_us"] = @((double(gm20b::counters::nanoseconds(a, units)) - committedNs) / 1000.0);
        }
#endif
    return out;
}

// facadecost's held-chunk check: the GPU addresses of its setBytes chunks, read while its job is held in flight (they go
// back at its completion)
- (NSArray<NSNumber *> *)switchMetalBytesChunks
{
    NSMutableArray<NSNumber *> *vas = [NSMutableArray array];
    for (const gm20b::Buffer &k : _bytes.chunks)
        [vas addObject:@(k.va)];
    return vas;
}

// job_words.h's: the lengths the recording filled those chunks to before taking the next
- (NSArray<NSNumber *> *)switchMetalBytesChunkLengths
{
    NSMutableArray<NSNumber *> *lengths = [NSMutableArray array];
    for (const gm20b::Buffer &k : _bytes.chunks)
        [lengths addObject:@(k.length)];
    return lengths;
}
#endif

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

// Every MTLCommandBuffer selector is a method of the command buffer: one that reaches here is no Metal selector, answered zero
// with an error line
- (void)forwardInvocation:(NSInvocation *)inv
{
    SWITCH_REFUSE_DETAIL("gm20b", "call.cbinvocation", uintptr_t(inv.selector),
                         "-[MTLCommandBuffer %s]: no method of this device's command buffer: answered zero",
                         sel_getName(inv.selector));
    answerZero(self, inv);
}

@end

@implementation SwitchMetalQueue

// nil where its handler thread cannot be made, as Metal's newCommandQueue answers nil where it makes no queue
- (instancetype)init
{
    if ((self = [super init])) {
        queuePriorityAdd(self);
        int err = 0;
        if (!_handlerExec.start(err)) {
            os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no command queue made: its handler thread: %{public}s",
                         getprogname(), strerror(err));
            return nil;
        }
    }
    return self;
}

- (void)dealloc
{
    _handlerExec.stop();
    queuePriorityRemove(self);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    SwitchMetalCommandBuffer *b = [SwitchMetalCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_status.store(MTLCommandBufferStatusNotEnqueued, std::memory_order_relaxed);
    gm20bTakeRecording(b);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

// Every command buffer of the queue uses these sets (gm20bSnapshotResidencySets, gm20bNameResidencySets)
- (void)addResidencySet:(id)set
{
    gm20bQueueAddResidencySet(self, set);
}

- (void)addResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    for (NSUInteger k = 0; k < count; k++)
        gm20bQueueAddResidencySet(self, sets[k]);
}

- (void)removeResidencySet:(id)set
{
    gm20bQueueRemoveResidencySet(self, set);
}

- (void)removeResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    for (NSUInteger k = 0; k < count; k++)
        gm20bQueueRemoveResidencySet(self, sets[k]);
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
