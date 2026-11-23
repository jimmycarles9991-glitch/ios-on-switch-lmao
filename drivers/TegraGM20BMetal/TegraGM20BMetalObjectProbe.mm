// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalEventJobs.h"
#import "TegraGM20BMetalInternal.h"
#import "SwitchSharedEvent.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.*: an object this device made itself answers its class; another device's object does not
// answer
@implementation SwitchMetalObject (NativeProbe)

- (NSDictionary<NSString *, id> *)switchMetalObject
{
    return @{@"class" : NSStringFromClass([self class])};
}

@end

// A render state also answers whether its record is the pack's (hw_test_app's states that must never wait on a compile)
// and its record's set-0 entries (0: no record)
@implementation SwitchMetalPipeline (NativeProbe)

- (NSDictionary<NSString *, id> *)switchMetalObject
{
    return @{
        @"class" : NSStringFromClass([self class]),
        @"packed" : @(_packed),
        @"entries" : @(_raw && _raw->rec ? _raw->entries.size() : 0)
    };
}

@end

@implementation SwitchSharedEvent (NativeProbe)

- (NSDictionary<NSString *, id> *)switchMetalObject
{
    return @{@"class" : NSStringFromClass([self class])};
}

@end

// The jobs of no work made since the device opened that signal or wait, the waits retired with the channel's last submission,
// the signals whose value the CPU set after the work before them (an event the kernel does not hold), the waits the
// scheduler held on the CPU, the jobs the raw channel submitted, the signals they released mid-job and how many of those
// went without the L2 flush (no host-visible memory written before them)
@implementation SwitchMetalDevice (EventProbe)

- (NSDictionary<NSString *, NSNumber *> *)switchMetalEvents
{
    return @{
        @"signal_jobs" : @(eventJobsSubmitted()),
        @"wait_jobs" : @(waitJobsSubmitted()),
        @"promise_waits" : @(promiseWaitsRetired()),
        @"cpu_signals" : @(gRM->stats.signalsAfterWork.load()),
        @"cpu_waits" : @(gRM->stats.waitsHeldOnCpu.load()),
        @"jobs" : @(gRM->raw->jobsSubmitted()),
        @"released_signals" : @(gRM->raw->signalsReleased()),
        @"unflushed_signals" : @(gRM->raw->signalsUnflushed()),
        @"exact_jobs" : @(gRM->raw->exactJobs()),
    };
}

@end
#endif
