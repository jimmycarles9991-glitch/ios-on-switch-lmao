// SPDX-License-Identifier: MIT
// MTLParallelRenderCommandEncoder (raw_parallel.h): its sub-encoders record into command buffers of their own, which no
// other thread touches, and its end records their commands into its command buffer in their creation order.
#include "raw_lock.h"
#import "RawMetalCounters.h"
#include "raw_parallel.h"

// A sub-encoder's command buffer: never committed. It keeps each command with the extra words its own record: drops. (Its
// encoder holds it: weak the other way.)
@interface RMSubRecorder : RMCommandBuffer {
@public
    std::vector<RMSubCommand> _log;
    __weak RMRenderEncoder *_encoder;
    MTLRenderPassDescriptor *_pass;  // its encoder's copy of the pass, which its OpRender names
}
@end

@implementation RMSubRecorder
@end

// Its pass as sub-encoder `at` of `count` records it: m2v's replay draws each as a pass of its own (raw::seamLoad, seamStore)
// and the raw path continues the first, so the raw facts load as the parallel pass at the first and store as it at each
static void seam(RMSubRecorder *s, MTLRenderPassDescriptor *parallel, size_t at, size_t count)
{
    RMPass &pass = s->_passes[0];
    MTLRenderPassDescriptor *own = s->_pass;
    for (unsigned j = 0; j < 8; j++) {
        if (!pass.color[j].texture)
            continue;
        const MTLLoadAction load = parallel.colorAttachments[j].loadAction;
        const MTLStoreAction store = parallel.colorAttachments[j].storeAction;
        own.colorAttachments[j].loadAction = MTLLoadAction(raw::seamLoad(uint8_t(load), at));
        own.colorAttachments[j].storeAction = MTLStoreAction(raw::seamStore(uint8_t(store), at, count));
        pass.color[j].load = MTLLoadAction(raw::seamLoad(uint8_t(load), at));
        pass.color[j].store = store;
    }
    MTLRenderPassAttachmentDescriptor *aspects[2][2] = {{own.depthAttachment, parallel.depthAttachment},
                                                        {own.stencilAttachment, parallel.stencilAttachment}};
    for (auto &[mine, theirs] : aspects)
        if (mine.texture) {
            mine.loadAction = MTLLoadAction(raw::seamLoad(uint8_t(theirs.loadAction), at));
            mine.storeAction = MTLStoreAction(raw::seamStore(uint8_t(theirs.storeAction), at, count));
        }
    pass.depthLoad = own.depthAttachment.loadAction;
    pass.stencilLoad = own.stencilAttachment.loadAction;
    pass.continues = at > 0;
}

// `s`'s objects, setBytes chunks, passes and refusals into `cb`, then its commands as one encoder of `cb`'s
static void splice(RMCommandBuffer *cb, RMSubRecorder *s, bool refused)
{
    for (id object : s->_owners)
        [cb retain:object];
    const size_t chunkBase = cb->_bytes.chunks.size(), passBase = cb->_passes.size();
    if (!s->_bytes.chunks.empty()) {
        cb->_bytes.chunks.insert(cb->_bytes.chunks.end(), s->_bytes.chunks.begin(), s->_bytes.chunks.end());
        cb->_bytes.at = cb->_bytes.chunks.size();  // a later setBytes takes a chunk of its own: these are full
        cb->_bytes.used = 0;
        s->_bytes = {};
    }
    cb->_passes.insert(cb->_passes.end(), s->_passes.begin(), s->_passes.end());
    cb->_sampledShadows |= s->_sampledShadows;
    cb->_standIns |= s->_standIns;
    cb->_whole |= s->_whole;
    if (cb->_reason == WhyRaw && s->_reason != WhyRaw) {
        cb->_reason = s->_reason;
        cb->_why = s->_why;
    }
    cb->_preds |= s->_preds;
    if (!cb->_pred0 && s->_pred0) {
        cb->_pred0 = s->_pred0;
        cb->_predA = s->_predA;
        cb->_predB = s->_predB;
    }
    [cb closeEncoder];
    cb->_encAt = cb->_plan.size();
    cb->_encOpen = true;
    cb->_encRefused = refused;
    for (const RMSubCommand &r : s->_log) {
        const Cmd c = raw::spliced(r.cmd, passBase, chunkBase, cb->_extra.size());
        cb->_extra.insert(cb->_extra.end(), r.extra.begin(), r.extra.end());
        [cb record:c];
    }
    [cb closeEncoder];
}

@implementation RMParallelRenderEncoder {
@public
    MTLRenderPassDescriptor *_descriptor;
    raw::Mutex _lock;
    std::vector<RMSubRecorder *> _subs;
}

- (RMRenderEncoder *)makeSub
{
    RMSubRecorder *s = [RMSubRecorder new];
    s->_commandLog = &s->_log;
    s->_device = _device;
    s->_queue = _cb->_queue;
    s->_status = MTLCommandBufferStatusNotEnqueued;
    RMRenderEncoder *e = (RMRenderEncoder *)[s renderCommandEncoderWithDescriptor:_descriptor];
    rmOwnPass(e);  // its seam changes its pass's loads and stores
    s->_encoder = e;
    s->_pass = e->_pass;
    _subs.push_back(s);
    return e;
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoder
{
    std::lock_guard<raw::Mutex> hold(_lock);
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

// One pass of every sub-encoder, all on the raw path or all on m2v: a raw pass started again after one on m2v would clear
// what the one before drew
- (void)endEncoding
{
    std::vector<RMSubRecorder *> subs;
    {
        std::lock_guard<raw::Mutex> hold(_lock);
        if (_ended)
            return;
        _ended = true;
        if (_subs.empty())
            [[self makeSub] endEncoding];  // the pass's loads, clears and stores with no draw
        subs = _subs;
    }
    bool refused = false;
    for (RMSubRecorder *s : subs) {
        RMRenderEncoder *e = s->_encoder;  // nil: its caller let it go, which ended it
        [e endEncoding];
        refused |= s->_encRefused || s->_whole;
    }
    RMCommandBuffer *cb = _cb;
    std::vector<size_t> begins;
    for (size_t at = 0; at < subs.size(); at++) {
        seam(subs[at], _descriptor, at, subs.size());
        begins.push_back(cb->_plan.size());
        splice(cb, subs[at], refused);
    }
    if (!refused && std::any_of(begins.begin(), begins.end(), [&](size_t b) {
            return cb->_plan.begin(b).refused;
        }))
        for (size_t b : begins)
            cb->_plan.setRefused(b, true);
    rmPlaceCounterMarks(cb, self, begins.front(), true);
    rmPlaceCounterMarks(cb, self, cb->_plan.size() - 1, false);
    cb->_active = nil;
}

@end

@implementation RMCommandBuffer (Parallel)

- (id<MTLParallelRenderCommandEncoder>)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    SWITCH_API(ApiEncoder);
    RMParallelRenderEncoder *e = [RMParallelRenderEncoder new];
    [self begin:e];
    e->_descriptor = [d copy];
    [self retain:e->_descriptor];
    rmCounterMarks(self, e, e->_descriptor);
    return (id<MTLParallelRenderCommandEncoder>)e;
}

@end

bool rmContinuesPass(RMCommandBuffer *cb, size_t at)
{
    return at < cb->_plan.size() && cb->_plan.operation(at).op == OpRender &&
           cb->_passes[cb->_plan.legacy(at).x].continues;
}

bool rmGroupedPass(RMCommandBuffer *cb, size_t begin)
{
    return rmContinuesPass(cb, begin) || rmContinuesPass(cb, cb->_plan.encoderEnd(begin));
}

size_t rmPassGroupEnd(RMCommandBuffer *cb, size_t begin)
{
    size_t end = cb->_plan.encoderEnd(begin);
    while (rmContinuesPass(cb, end))
        end = cb->_plan.encoderEnd(end);
    return end;
}
