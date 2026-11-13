// SPDX-License-Identifier: MIT
// Metal's completion-handler creations of render states. They may complete later, so a state the pack lacks is asked of
// the runtime without a waiting thread (Completion::create: the shader compiler's answer arrives on the runtime's queue),
// and the handler runs once it is there, on a global queue of the caller's class, as Metal calls it. No thread sleeps on
// the compiler for them. A creation that fails gets the synchronous form's error and no m2v state.
#import "RawMetalInternal.h"

static dispatch_queue_t creationQueue(qos_class_t qos)
{
    return dispatch_get_global_queue(qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INITIATED : qos, 0);
}

using Finish = void (^)(id<MTLRenderPipelineState> state, MTLAutoreleasedRenderPipelineReflection reflection,
                        NSError *error);

static void finishState(RMPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool reflect, Finish finish)
{
    NSError *error = nil;
    MTLAutoreleasedRenderPipelineReflection reflection = nil;
    id<MTLRenderPipelineState> state = rmFinishState(p, d, o, reflect ? &reflection : nil, &error);
    finish(state, reflection, error);
}

static void createLater(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool reflect,
                        Finish finish)
{
    const qos_class_t qos = qos_class_self();
    dispatch_async(creationQueue(qos), ^{
        if (ycbcrTargetFormat(d)) {
            NSError *error = nil;
            MTLAutoreleasedRenderPipelineReflection reflection = nil;
            id<MTLRenderPipelineState> state = [device ycbcrTargetState:d
                                                                options:o
                                                             reflection:reflect ? &reflection : nil
                                                                  error:&error];
            return finish(state, reflection, error);
        }
        NSError *error = nil;
        RMPipeline *p = rmPipelineShell(device, d, o, &error);
        if (!p)
            return finish(nil, nil, error);
        if (!rmNeedsRecord(p))
            return finishState(p, d, o, reflect, finish);
        gRM->completion->create(rmRecordAsk(p, d),
                                [p, d, o, reflect, finish, qos](raw::Pipeline *made, const std::string &failure) {
                                    p->_raw = made;
                                    NSError *failed = failure.empty() ? nil : rmCreationError(failure);
                                    dispatch_async(creationQueue(qos), ^{
                                        if (failed)
                                            finish(nil, nil, failed);
                                        else
                                            finishState(p, d, o, reflect, finish);
                                    });
                                });
    });
}

@implementation RMDevice (StatesLater)

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
    createLater(self, [d copy], MTLPipelineOptionNone, false,
                ^(id<MTLRenderPipelineState> state, MTLAutoreleasedRenderPipelineReflection, NSError *error) {
                    handler(state, error);
                });
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                     options:(MTLPipelineOption)o
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
    createLater(
        self, [d copy], o, o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo),
        ^(id<MTLRenderPipelineState> state, MTLAutoreleasedRenderPipelineReflection reflection, NSError *error) {
            handler(state, reflection, error);
        });
}

@end
