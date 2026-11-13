// SPDX-License-Identifier: MIT
#import "RawMetalEventJobs.h"
#import "RawMetalInternal.h"
#import "SwitchSharedEvent.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.*: an object this device made itself answers its class and whether m2v's twin of it was ever
// made; an m2v object does not answer
@implementation RMObject (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @(_twin != nil)};
}

@end

// A render state also answers whether its record is the pack's (hw_test_app's states that must never wait on a compile)
@implementation RMPipeline (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @(_twin != nil), @"packed" : @(_packed)};
}

@end

@implementation SwitchSharedEvent (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @NO};
}

@end

// The jobs of no work made since the device opened that signal or wait, the waits retired with the channel's last submission,
// the signals whose value the CPU set after the work before them (an event the kernel does not hold), the waits the
// scheduler held on the CPU, the jobs the raw channel submitted, the signals they released mid-job and how many of those
// went without the L2 flush (no host-visible memory written before them)
@implementation RMDevice (EventProbe)

- (NSDictionary<NSString *, NSNumber *> *)switchRawEvents
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
