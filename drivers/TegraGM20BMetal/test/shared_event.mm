// SPDX-License-Identifier: MIT
#import "../raw/runtime/SwitchSharedEvent.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;

static void check(bool value, const char *message)
{
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
    ++checks;
}

int main()
{
    @autoreleasepool {
        SwitchSharedEvent *event = [[SwitchSharedEvent alloc] initWithOptions:0];
        check(event && MACH_PORT_VALID(event.eventPort), "native event creation");
        check(event.signaledValue == 0 && !event.supportsRollback, "initial monotonic value");
        check(![[SwitchSharedEvent alloc] initWithOptions:~NSUInteger(0)], "unsupported options refused");
        check(![[SwitchSharedEvent alloc] initWithMachPort:MACH_PORT_NULL], "invalid port refused");
        event.label = @"Switch native shared event";
        MTLSharedEventHandle *handle = [event newSharedEventHandle];
        check([handle isKindOfClass:[MTLSharedEventHandle class]] && [handle.label isEqual:event.label],
              "actual Apple handle and label");
        SwitchSharedEvent *alias = [[SwitchSharedEvent alloc] initWithHandle:handle];
        check(alias && alias.labelTraceID == event.labelTraceID && [alias.label isEqual:event.label],
              "handle import preserves identity");
        const uint64_t value = (uint64_t(1) << 40) + 17;
        event.signaledValue = value;
        check(alias.signaledValue == value, "full-width signal visible through handle alias");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLSharedEvent> apple = [device newSharedEventWithHandle:handle];
        check(apple && apple.signaledValue == value, "actual Metal device imports native handle");
        apple.signaledValue = value + 1;
        check(event.signaledValue == value + 1 && alias.signaledValue == value + 1,
              "Apple event signal reaches both wrappers");
        check(![alias waitUntilSignaledValue:value + 2 timeoutMS:0], "unreached value does not pass");
        dispatch_queue_t queue = dispatch_queue_create("SwitchSharedEventTest", DISPATCH_QUEUE_SERIAL);
        static char queueKey;
        dispatch_queue_set_specific(queue, &queueKey, &queueKey, nullptr);
        dispatch_semaphore_t notified = dispatch_semaphore_create(0);
        __block unsigned calls = 0;
        __block bool callbackCorrect = false;
        @autoreleasepool {
            MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:queue];
            [alias notifyListener:listener
                          atValue:value + 2
                            block:^(id<MTLSharedEvent> received, uint64_t reached) {
                                ++calls;
                                callbackCorrect = received == alias && reached >= value + 2 &&
                                                  dispatch_get_specific(&queueKey) == &queueKey;
                                dispatch_semaphore_signal(notified);
                            }];
        }
        check(dispatch_semaphore_wait(notified, DISPATCH_TIME_NOW) != 0, "notification waits for actual value");
        apple.signaledValue = value + 2;
        check(!dispatch_semaphore_wait(notified, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)),
              "notification survives listener owner release");
        dispatch_sync(queue, ^{
                      });
        check(callbackCorrect && calls == 1, "correct public event and callback queue");
        check([alias waitUntilSignaledValue:value + 2 timeoutMS:0], "satisfied native wait");
        SwitchSharedEvent *portAlias = [[SwitchSharedEvent alloc] initWithMachPort:event.eventPort];
        check(portAlias && portAlias.signaledValue == value + 2, "native Mach port import");
        event = nil;
        alias = nil;
        portAlias = nil;
        apple = nil;
        SwitchSharedEvent *survivor = [[SwitchSharedEvent alloc] initWithHandle:handle];
        check(survivor && survivor.signaledValue == value + 2, "exported handle owns its capability");
        printf("PASS: %u native shared-event checks\n", checks);
    }
}
