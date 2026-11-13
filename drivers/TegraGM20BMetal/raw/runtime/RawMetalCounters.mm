// SPDX-License-Identifier: MIT
// The device's counter sets, sample buffers and clock, as the identity's AGX driver answers them (raw_counters.h).
#import "RawMetalCounters.h"
#include "raw_counter_kernel.h"

static NSString *text(std::string_view s)
{
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

// Metal's own counter classes, made as -[AGXA10FamilyDevice counterSets] and -[AGXA13FamilyDevice counterSets] make them
@interface MTLCounterInternal : NSObject <MTLCounter>
- (instancetype)initWithName:(NSString *)name description:(NSString *)description;
@end

@interface MTLCounterSetInternal : NSObject <MTLCounterSet>
- (instancetype)initWithName:(NSString *)name
                 description:(NSString *)description
                    counters:(NSArray<id<MTLCounter>> *)counters;
@end

static NSArray<id<MTLCounterSet>> *timestampSets()
{
    static Class const counterClass = NSClassFromString(@"MTLCounterInternal"),
                       setClass = NSClassFromString(@"MTLCounterSetInternal");
    if (!counterClass || !setClass) {
        SWITCH_REFUSE("raw", "counter.sets",
                      "Metal has no MTLCounterInternal or MTLCounterSetInternal: no counter set");
        return @[];
    }
    MTLCounterInternal *counter =
        [(MTLCounterInternal *)[counterClass alloc] initWithName:MTLCommonCounterTimestamp
                                                     description:text(raw::counters::kCounterDescription)];
    MTLCounterSetInternal *set =
        [(MTLCounterSetInternal *)[setClass alloc] initWithName:MTLCommonCounterSetTimestamp
                                                    description:text(raw::counters::kSetDescription)
                                                       counters:@[ counter ]];
    return @[ set ];
}

@implementation RMCounterSampleBuffer

- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}

- (NSUInteger)sampleCount
{
    return _sampleCount;
}

// The driver's raw copy of the samples, here each record placed by the anchor of its power-up of the GPU
- (NSData *)resolveCounterRange:(NSRange)range
{
    if (!raw::counters::resolvable(range.location, range.length, _sampleCount))
        return nil;
    const auto *records = static_cast<const raw::counters::Record *>([(id<MTLBuffer>)_storage contents]);
    if (!records)
        return nil;
    const TegraGM20BAbiClockAnchors anchors = rmClockAnchorsNow();
    NSMutableData *data = [NSMutableData dataWithLength:range.length * raw::counters::kSampleBytes];
    auto *out = static_cast<uint64_t *>(data.mutableBytes);
    for (NSUInteger i = 0; i < range.length; i++) {
        raw::counters::Record r;
        memcpy(&r, records + range.location + i, sizeof r);
        out[i] = raw::counters::resolved(r, anchors);
    }
    return data;
}

@end

static std::atomic<bool> gCounterBuffersMade{false};

bool rmCounterBuffersMade()
{
    return gCounterBuffersMade.load(std::memory_order_relaxed);
}

uint64_t rmHostNanoseconds()
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

const RMClockAnchors &rmClockAnchors()
{
    static const RMClockAnchors anchors = [] {
        RMClockAnchors a = {};
        if (!gRM->raw->clockAnchors(a.table, a.va)) {
            a = {};
            SWITCH_REFUSE("raw", "counter.anchors",
                          "the kernel has no clock anchors: counter samples resolve to the error value");
        }
        return a;
    }();
    return anchors;
}

TegraGM20BAbiClockAnchors rmClockAnchorsNow()
{
    TegraGM20BAbiClockAnchors now = {};
    if (const RMClockAnchors &live = rmClockAnchors(); live.table)
        raw::counters::snapshot(*live.table, now);
    return now;
}

static id failed(NSError **error, MTLCounterSampleBufferError code, const std::string &message)
{
    if (error)
        *error = [NSError errorWithDomain:MTLCounterErrorDomain
                                     code:code
                                 userInfo:@{
                                     NSLocalizedDescriptionKey : @(message.c_str())
                                 }];
    return nil;
}

static id<MTLCounterSampleBuffer> newSamples(RMDevice *device, NSUInteger samples, NSString *label, bool resolvedAhead,
                                             NSError **error)
{
    const NSUInteger bytes = samples * raw::counters::kRecordBytes;
    RMBuffer *storage = (RMBuffer *)[(id<MTLDevice>)device newBufferWithLength:bytes
                                                                       options:MTLResourceStorageModeShared];
    if (![(id)storage isKindOfClass:[RMBuffer class]] || !storage->_raw.cpu)
        return failed(error, MTLCounterSampleBufferErrorOutOfMemory, "Cannot allocate sample buffer");
    memset(storage->_raw.cpu, 0, bytes);
    if (!storage->_raw.wc)
        cacheLines(storage->_raw.cpu, bytes, false);
    RMCounterSampleBuffer *b = [RMCounterSampleBuffer new];
    b->_device = device;
    b->_storage = storage;
    b->_sampleCount = samples;
    b.label = label;
    gCounterBuffersMade.store(true, std::memory_order_relaxed);
    static std::once_flag warm;
    if (resolvedAhead)
        std::call_once(warm, [] {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                rmCounterResolveState();
            });
        });
    if (error)
        *error = nil;
    return b;
}

@interface RMDevice (Counters)
@end
@implementation RMDevice (Counters)

- (NSArray<id<MTLCounterSet>> *)counterSets
{
    return timestampSets();
}

// -[AGXA1xFamilyDevice counterSampleBufferLimits]: the same constant on both identities
- (NSDictionary *)counterSampleBufferLimits
{
    return @{
        @"maxBufferSize" : @(raw::counters::kMaxBufferBytes),
        @"maxDeviceBufferNum" : @32,
        @"maxGlobalBufferNum" : @1024,
    };
}

- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)d
                                                             error:(NSError **)error
{
    id<MTLCounterSet> set = d.counterSet;
    NSArray<id<MTLCounter>> *counters = set.counters;
    NSString *setName = set.name, *counterName = counters.count ? counters[0].name : nil;
    const raw::counters::Check check = raw::counters::checkDescriptor(
        counters.count, setName.UTF8String ?: "", counterName.UTF8String ?: "", uint64_t(d.sampleCount));
    if (check != raw::counters::Check::Ok) {
        NSString *found = check == raw::counters::Check::SetName ? setName : counterName;
        return failed(
            error, MTLCounterSampleBufferErrorInvalid,
            raw::counters::checkMessage(check, found ? found.UTF8String : "(null)", uint64_t(d.sampleCount) << 3));
    }
    // A private buffer's samples are resolved by a blit alone: its kernel compiled ahead (a shared one's at the first)
    return newSamples(self, d.sampleCount, d.label, d.storageMode == MTLStorageModePrivate, error);
}

// -[AGXMTLCounterSampleBuffer initWithDevice:length:label:options:error:], options 0 (shared): the length's checks alone
- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithLength:(NSUInteger)length
                                                         label:(NSString *)label
                                                         error:(NSError **)error
{
    const raw::counters::Check check = raw::counters::checkLength(uint64_t(length));
    if (check != raw::counters::Check::Ok)
        return failed(error, MTLCounterSampleBufferErrorInvalid,
                      raw::counters::checkMessage(check, {}, uint64_t(length)));
    return newSamples(self, length / raw::counters::kSampleBytes, label, false, error);
}

// The driver's answer: one reading of the CPU's clock for both, the GPU's samples being in its domain
- (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp
{
    const uint64_t ns = rmHostNanoseconds();
    if (cpuTimestamp)
        *cpuTimestamp = ns;
    if (gpuTimestamp)
        *gpuTimestamp = ns;
}

@end
