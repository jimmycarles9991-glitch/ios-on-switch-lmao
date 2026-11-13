// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "SwitchSharedEvent.h"
#include "raw_event_release.h"

@interface NSObject (SwitchSharedEventPort)
- (mach_port_t)eventPort;
- (BOOL)supportsRollback;
@end

@interface SwitchSharedEventBinding : NSObject {
@public
    std::shared_ptr<raw::SharedEventLease> lease;
    std::atomic<uint64_t> submittedValue;
    // The submit thread's: the event's word on the raw channel of `wordGeneration` (method 40), the last payload a submitted
    // job released into it, and the one the range being encoded (`wordStamp`) did
    uint64_t wordAddress, wordBase, wordGeneration, wordStamp;
    uint32_t wordFloor, wordPending;
    bool wordSpent;  // the kernel answered kIOReturnNoResources: the word passed half its range
}
@end
@implementation SwitchSharedEventBinding
@end

static SwitchSharedEventBinding *eventBinding(id event)
{
    if (!gRM->sharedEvents || ![event respondsToSelector:@selector(eventPort)] ||
        ([event respondsToSelector:@selector(supportsRollback)] && [event supportsRollback]))
        return nil;
    static char bindingKey;
    SwitchSharedEventBinding *binding = objc_getAssociatedObject(event, &bindingKey);
    if (binding && binding->lease->owner() == gRM->sharedEvents)
        return binding;
    @synchronized(event) {
        binding = objc_getAssociatedObject(event, &bindingKey);
        if (binding && binding->lease->owner() == gRM->sharedEvents)
            return binding;
        auto lease = gRM->sharedEvents->import([event eventPort]);
        if (!lease)
            return nil;
        binding = [SwitchSharedEventBinding new];
        binding->lease = std::move(lease);
        binding->submittedValue.store(0, std::memory_order_relaxed);
        objc_setAssociatedObject(event, &bindingKey, binding, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return binding;
    }
}

std::shared_ptr<raw::SharedEventLease> sharedEventLease(id event)
{
    SwitchSharedEventBinding *binding = eventBinding(event);
    return binding ? binding->lease : nullptr;
}

// Asked again on a new channel, or for a value past the word's window when no earlier release of this range is at the old
// base: the kernel moves the base only while none of the word's releases is outstanding
bool sharedEventWord(id event, uint64_t value, uint64_t inlineStamp, uint64_t &address, uint32_t &payload)
{
    SwitchSharedEventBinding *b = eventBinding(event);
    if (!b || b->wordSpent || !gRM->eventWords.load(std::memory_order_relaxed))
        return false;
    const uint64_t generation = gRM->raw->channelGeneration();
    const bool encoding = b->wordStamp == inlineStamp;
    uint32_t p = b->wordGeneration == generation
                     ? raw::eventReleasePayload(value, b->wordBase, encoding ? b->wordPending : b->wordFloor)
                     : 0;
    if (!p && !encoding && (b->wordGeneration != generation || value - b->wordBase >= (uint64_t(1) << 31))) {
        uint64_t base = 0;
        uint32_t floor = 0;
        const int kr = gRM->raw->eventWord(b->lease->handle(), address, base, floor);
        if (kr == kIOReturnUnsupported)
            gRM->eventWords.store(false, std::memory_order_relaxed);
        b->wordSpent = kr == kIOReturnNoResources;
        if (kr != KERN_SUCCESS) {
            once([NSString stringWithFormat:@"event word refused (0x%x): the signal goes at its job's end", kr]);
            return false;
        }
        b->wordAddress = address;
        b->wordBase = base;
        b->wordFloor = floor;
        b->wordGeneration = generation;
        p = raw::eventReleasePayload(value, base, floor);
    }
    if (!p)
        return false;
    address = b->wordAddress;
    payload = p;
    b->wordStamp = inlineStamp;
    b->wordPending = p;
    return true;
}

static void eventFailure(RMCommandBuffer *cb, NSString *message)
{
    std::lock_guard<raw::Mutex> lock(cb->_lock);
    cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                        code:MTLCommandBufferErrorInternal
                                    userInfo:@{NSLocalizedDescriptionKey : message}];
}

int sharedEventReadiness(RMCommandBuffer *cb, id event, uint64_t value, bool native)
{
    if (!native)
        return -1;
    SwitchSharedEventBinding *binding = eventBinding(event);
    if (!binding)
        return -1;
    auto lease = binding->lease;
    // This is only a readiness hint. Atomic submit still resolves the authoritative producer and its failure state.
    const int result = binding->submittedValue.load(std::memory_order_acquire) >= value
                           ? KERN_SUCCESS
                           : gRM->sharedEvents->probe(*lease, value);
    if (result == KERN_SUCCESS) {
        cb->_sharedEventWaits.push_back({(__bridge void *)event, std::move(lease), value});
        return 1;
    }
    if (result != kIOReturnNotReady) {
        eventFailure(cb, @"A shared-event producer failed");
        return 1;
    }
    if (!cb->_sharedEventWatch) {
        RMQueue *queue = cb->_queue;
        int armed = KERN_SUCCESS;
        cb->_sharedEventWatch = gRM->sharedEvents->observe(
            std::move(lease), value,
            [queue](int) {
                wakeSubmit((__bridge void *)queue);
            },
            armed);
        if (!cb->_sharedEventWatch) {
            eventFailure(cb, @"Shared-event producer notification could not be armed");
            return 1;
        }
    }
    return 0;
}

bool sharedWaitsReady(RMCommandBuffer *cb, bool native)
{
    if (native)
        return true;
    for (auto &wait : cb->_sharedEventWaits) {
        if (!wait.completion) {
            RMQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            wait.completion = gRM->sharedEvents->completion(
                wait.lease, wait.value,
                [queue] {
                    wakeSubmit((__bridge void *)queue);
                },
                armed);
            if (!wait.completion) {
                eventFailure(cb, @"Shared-event completion notification could not be armed");
                return true;
            }
        }
        const int result = wait.completion->result();
        if (result == kIOReturnNotReady)
            return false;
        if (result != KERN_SUCCESS) {
            eventFailure(cb, @"A shared-event producer failed before compatibility work");
            return true;
        }
    }
    cb->_sharedEventWaits.clear();
    return true;
}

void prepareSharedEvents(RMCommandBuffer *cb, raw::CommandBuffer *commands)
{
    std::vector<TegraGM20BAbiEventUse> waits, signals;
    std::vector<uint32_t> releases;
    waits.reserve(cb->_sharedEventWaits.size());
    for (const auto &wait : cb->_sharedEventWaits)
        waits.push_back({wait.lease->handle(), wait.value});
    for (const auto &[at, payload] : cb->_released) {
        const Cmd event = cb->_plan.legacy(at);
        signals.push_back({sharedEventLease((__bridge id)event.obj)->handle(), event.c});
        releases.push_back(payload);
    }
    cb->_sharedSignalRanges.clear();
    if (cb->_gpuPrefixEligible) {
        for (size_t at = cb->_segNext + 1; at < cb->_segs.size(); ++at) {
            const auto &range = cb->_segs[at];
            if (!range.event)
                break;
            const Cmd event = cb->_plan.legacy(range.from);
            if (event.op != OpSignalEvent)
                break;
            auto lease = sharedEventLease((__bridge id)event.obj);
            if (!lease)
                continue;
            signals.push_back({lease->handle(), event.c});
            cb->_sharedSignalRanges.push_back(at);
        }
    }
    commands->sharedEvents(std::move(waits), std::move(signals), std::move(releases));
}

static void submittedUpTo(SwitchSharedEventBinding *binding, uint64_t value)
{
    uint64_t old = binding->submittedValue.load(std::memory_order_relaxed);
    while (old < value && !binding->submittedValue.compare_exchange_weak(old, value, std::memory_order_release,
                                                                         std::memory_order_relaxed)) {
    }
}

void publishSharedEvents(RMCommandBuffer *cb)
{
    for (size_t at : cb->_sharedSignalRanges) {
        cb->_segs[at].gpuSignal = true;
        const Cmd event = cb->_plan.legacy(cb->_segs[at].from);
        if (SwitchSharedEventBinding *binding = eventBinding((__bridge id)event.obj))
            submittedUpTo(binding, event.c);
    }
    for (const auto &[at, payload] : cb->_released) {
        const Cmd event = cb->_plan.legacy(at);
        SwitchSharedEventBinding *binding = eventBinding((__bridge id)event.obj);
        submittedUpTo(binding, event.c);
        if (payload)
            binding->wordFloor = std::max(binding->wordFloor, payload);
    }
    cb->_released.clear();
    cb->_sharedSignalRanges.clear();
    cb->_sharedEventWaits.clear();
}
