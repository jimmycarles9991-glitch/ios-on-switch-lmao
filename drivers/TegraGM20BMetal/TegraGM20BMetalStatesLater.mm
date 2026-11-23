// SPDX-License-Identifier: MIT
// Metal's completion-handler creations of render states. They may complete later, so a state the pack lacks is asked of
// the runtime without a waiting thread (Completion::create: the shader compiler's answer arrives on the runtime's queue),
// and the handler runs once it is there, on a global queue of the caller's class, as Metal calls it. No thread sleeps on
// the compiler for a record; a reflection the pack lacks, and the variants a made state's creation names
// (TegraGM20BMetalCubic.mm gm20bMadeVariants), are waited for on that queue's worker before the handler runs
// (TegraGM20BMetalReflection.h). A creation that fails gets the synchronous form's error and no m2v state.
#import "TegraGM20BMetalInternal.h"

static dispatch_queue_t creationQueue(qos_class_t qos)
{
    return dispatch_get_global_queue(qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INITIATED : qos, 0);
}

using Finish = void (^)(id<MTLRenderPipelineState> state, MTLAutoreleasedRenderPipelineReflection reflection,
                        NSError *error);

static void finishState(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool reflect,
                        Finish finish)
{
    NSError *error = nil;
    MTLAutoreleasedRenderPipelineReflection reflection = nil;
    id<MTLRenderPipelineState> state = gm20bFinishState(p, d, o, reflect ? &reflection : nil, &error);
    finish(state, reflection, error);
}

static void createLater(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool reflect,
                        Finish finish)
{
    const qos_class_t qos = qos_class_self();
    dispatch_async(creationQueue(qos), ^{
        if (standInTargetFormat(d)) {
            NSError *error = nil;
            MTLAutoreleasedRenderPipelineReflection reflection = nil;
            id<MTLRenderPipelineState> state = [device standInTargetState:d
                                                                  options:o
                                                               reflection:reflect ? &reflection : nil
                                                                    error:&error];
            return finish(state, reflection, error);
        }
        NSError *error = nil;
        SwitchMetalPipeline *p = gm20bPipelineShell(device, d, o, &error);
        if (!p)
            return finish(nil, nil, error);
        if (!gm20bNeedsRecord(p))
            return finishState(p, d, o, reflect, finish);
        gRM->completion->create(gm20bRecordAsk(p, d),
                                [p, d, o, reflect, finish, qos](gm20b::Pipeline *made, const std::string &failure) {
                                    p->_raw = made;
                                    NSError *failed = failure.empty() ? nil : gm20bCreationError(failure);
                                    dispatch_async(creationQueue(qos), ^{
                                        if (failed)
                                            finish(nil, nil, failed);
                                        else
                                            finishState(p, d, o, reflect, finish);
                                    });
                                });
    });
}

@implementation SwitchMetalDevice (StatesLater)

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
