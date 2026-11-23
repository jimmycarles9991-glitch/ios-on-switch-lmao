// SPDX-License-Identifier: MIT
// MTLParallelRenderCommandEncoder (parallel.h): its sub-encoders record into command buffers of their own, which no
// other thread touches, and its end records their commands into its command buffer in their creation order.
#include "lock.h"
#import "TegraGM20BMetalCounters.h"
#include "parallel.h"

// A sub-encoder's command buffer: never committed. It keeps each command with the extra words its own record: drops. (Its
// encoder holds it: weak the other way.)
@interface SwitchMetalSubRecorder : SwitchMetalCommandBuffer {
@public
    std::vector<SwitchMetalSubCommand> _log;
    __weak SwitchMetalRenderEncoder *_encoder;
    MTLRenderPassDescriptor *_pass;  // its encoder's copy of the pass, which its OpRender names
}
@end

@implementation SwitchMetalSubRecorder
@end

// Its pass as sub-encoder `at` of `count` records it: the driver continues the first (gm20b::seamLoad, seamStore), so the
// facts load as the parallel pass at the first and store as it at each
static void seam(SwitchMetalSubRecorder *s, MTLRenderPassDescriptor *parallel, size_t at, size_t count)
{
    SwitchMetalPass &pass = s->_passes[0];
    MTLRenderPassDescriptor *own = s->_pass;
    for (unsigned j = 0; j < 8; j++) {
        if (!pass.color[j].texture)
            continue;
        const MTLLoadAction load = parallel.colorAttachments[j].loadAction;
        const MTLStoreAction store = parallel.colorAttachments[j].storeAction;
        own.colorAttachments[j].loadAction = MTLLoadAction(gm20b::seamLoad(uint8_t(load), at));
        own.colorAttachments[j].storeAction = MTLStoreAction(gm20b::seamStore(uint8_t(store), at, count));
        pass.color[j].load = MTLLoadAction(gm20b::seamLoad(uint8_t(load), at));
        pass.color[j].store = store;
    }
    MTLRenderPassAttachmentDescriptor *aspects[2][2] = {{own.depthAttachment, parallel.depthAttachment},
                                                        {own.stencilAttachment, parallel.stencilAttachment}};
    for (auto &[mine, theirs] : aspects)
        if (mine.texture) {
            mine.loadAction = MTLLoadAction(gm20b::seamLoad(uint8_t(theirs.loadAction), at));
            mine.storeAction = MTLStoreAction(gm20b::seamStore(uint8_t(theirs.storeAction), at, count));
        }
    pass.depthLoad = own.depthAttachment.loadAction;
    pass.stencilLoad = own.stencilAttachment.loadAction;
    pass.continues = at > 0;
}

// `s`'s objects, setBytes chunks and shadow, passes and failures into `cb`, then its commands as one encoder of `cb`'s
static void splice(SwitchMetalCommandBuffer *cb, SwitchMetalSubRecorder *s)
{
    for (id object : s->_owners)
        [cb retain:object];
    const size_t chunkBase = cb->_bytes.chunks.size(), passBase = cb->_passes.size(), shadowBase = cb->_shadow.size();
    cb->_shadow.append(s->_shadow.ownData(), s->_shadow.ownData() + s->_shadow.size());
    cb->_borderSamplers |= s->_borderSamplers;
    if (!s->_bytes.chunks.empty()) {
        cb->_bytes.chunks.append(s->_bytes.chunks.ownData(), s->_bytes.chunks.ownData() + s->_bytes.chunks.size());
        cb->_bytes.at = cb->_bytes.chunks.size();  // a later setBytes takes a chunk of its own: these are full
        cb->_bytes.used = 0;
        s->_bytes = {};
    }
    cb->_passes.append(s->_passes.ownData(), s->_passes.ownData() + s->_passes.size());
    cb->_sampledShadows |= s->_sampledShadows;
    cb->_standIns |= s->_standIns;
    if (!cb->_pred0 && s->_pred0) {
        cb->_pred0 = s->_pred0;
        cb->_predA = s->_predA;
        cb->_predB = s->_predB;
    }
    if (s->_segError.set()) {
        NSError *error = nil;
        {
            std::lock_guard<gm20b::Mutex> l(s->_lock);
            error = s->_segError;
        }
        std::lock_guard<gm20b::Mutex> l(cb->_lock);
        if (!cb->_segError.set()) {
            cb->_segError = error;
            cb->_failedBy = s->_failedBy;
        }
    }
    [cb closeEncoder];
    cb->_encAt = cb->_plan.size();
    cb->_encOpen = true;
    for (const SwitchMetalSubCommand &r : s->_log) {
        const Cmd c = gm20b::spliced(r.cmd, passBase, chunkBase, shadowBase, cb->_extra.size());
        cb->_extra.append(r.extra.begin(), r.extra.end());
        [cb record:c];
    }
    [cb closeEncoder];
}

@implementation SwitchMetalParallelRenderEncoder {
@public
    MTLRenderPassDescriptor *_descriptor;
    gm20b::Mutex _lock;
    std::vector<SwitchMetalSubRecorder *> _subs;
}

- (SwitchMetalRenderEncoder *)makeSub
{
    SwitchMetalSubRecorder *s = [SwitchMetalSubRecorder new];
    gm20bTakeRecording(s);
    s->_commandLog = &s->_log;
    s->_device = _device;
    s->_queue = _cb->_queue;
    s->_status = MTLCommandBufferStatusNotEnqueued;
    SwitchMetalRenderEncoder *e = (SwitchMetalRenderEncoder *)[s renderCommandEncoderWithDescriptor:_descriptor];
    gm20bOwnPass(e);  // its seam changes its pass's loads and stores
    s->_encoder = e;
    s->_pass = e->_pass;
    _subs.push_back(s);
    return e;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoder
{
    std::lock_guard<gm20b::Mutex> hold(_lock);
    return _ended ? nil : (id<MTLRenderCommandEncoder>)[self makeSub];
}

- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index
{
    _descriptor.colorAttachments[index].storeAction = action;
}

- (void)setDepthStoreAction:(MTLStoreAction)action
{
    _descriptor.depthAttachment.storeAction = action;
}

- (void)setStencilStoreAction:(MTLStoreAction)action
{
    _descriptor.stencilAttachment.storeAction = action;
}

- (void)setColorStoreActionOptions:(MTLStoreActionOptions)options atIndex:(NSUInteger)index
{
    _descriptor.colorAttachments[index].storeActionOptions = options;
}

- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)options
{
    _descriptor.depthAttachment.storeActionOptions = options;
}

- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)options
{
    _descriptor.stencilAttachment.storeActionOptions = options;
}

// One pass of every sub-encoder, each continuing the one before; a sub-encoder's failure fails the command buffer (splice)
- (void)endEncoding
{
    std::vector<SwitchMetalSubRecorder *> subs;
    {
        std::lock_guard<gm20b::Mutex> hold(_lock);
        if (_ended)
            return;
        _ended = true;
        if (_subs.empty())
            [[self makeSub] endEncoding];  // the pass's loads, clears and stores with no draw
        subs = _subs;
    }
    for (SwitchMetalSubRecorder *s : subs) {
        SwitchMetalRenderEncoder *e = s->_encoder;  // nil: its caller let it go, which ended it
        if (e && !e->_ended)
            [e endEncoding];
    }
    SwitchMetalCommandBuffer *cb = _cb;
    const size_t first = cb->_plan.size();
    for (size_t at = 0; at < subs.size(); at++) {
        seam(subs[at], _descriptor, at, subs.size());
        splice(cb, subs[at]);
        gm20bRecordingDone(subs[at]);
        // a draw its encoder records from now on takes checkDraw's failed branch (gm20bDrawAfterParallelEnd)
        subs[at]->_encFailed = true;
    }
    gm20bPlaceCounterMarks(cb, self, first, true);
    gm20bPlaceCounterMarks(cb, self, cb->_plan.size() - 1, false);
    cb->_active = nil;
}

@end

@implementation SwitchMetalCommandBuffer (Parallel)

- (id<MTLParallelRenderCommandEncoder>)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    SwitchMetalParallelRenderEncoder *e = [SwitchMetalParallelRenderEncoder new];
    gm20bPipelineVeto(self, VetoParallel);
    [self begin:e];
    e->_descriptor = [d copy];
    [self retain:e->_descriptor];
    gm20bCounterMarks(self, e, e->_descriptor);
    return (id<MTLParallelRenderCommandEncoder>)e;
}

@end

bool gm20bContinuesPass(SwitchMetalCommandBuffer *cb, size_t at)
{
    return at < cb->_plan.size() && cb->_plan.operation(at).op == OpRender &&
           cb->_passes[cb->_plan.legacy(at).x].continues;
}

bool gm20bGroupedPass(SwitchMetalCommandBuffer *cb, size_t begin)
{
    return gm20bContinuesPass(cb, begin) || gm20bContinuesPass(cb, cb->_plan.encoderEnd(begin));
}

// A draw on a sub-encoder after its parallel encoder ended is Metal's validation error, which an M4 Max with validation
// off crashes on. The sub-encoder holds no reference to the command buffer its recorder was spliced into, so nothing
// fails: the draw records nothing and is counted skipped.
bool gm20bDrawAfterParallelEnd(SwitchMetalCommandBuffer *cb)
{
    if (![cb isKindOfClass:[SwitchMetalSubRecorder class]] || !cb->_recordingReturned)
        return false;
    gm20bSkipped(PredEncPassRender);
    SWITCH_REFUSE("gm20b", "parallel.ended_sub",
                  "a draw on a parallel encoder's sub-encoder after the parallel encoder ended: Metal's validation "
                  "error, nothing drawn (skipped enc.pass)");
    return true;
}
