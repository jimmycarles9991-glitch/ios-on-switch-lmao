// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalStatesCost.h"

#if SWITCH_METAL_PROF || SWITCH_METAL_STATS
#include <mach/mach.h>
#include <mutex>
#include <unistd.h>
#import "TegraGM20BMetalStatesKey.h"
#include "ca_catch_all.h"

#if SWITCH_METAL_STATS
namespace {
std::atomic<uint64_t> gPackHits{0}, gPackMisses{0}, gPackCold{0}, gPackCard{0};
}  // namespace

bool gm20bPackIsTier1(const gm20b::Pack &pack, const gm20b::PackRun &run)
{
    if (!pack.header() || !run.records)
        return false;
    const gm20b::PackPipeline *tier1 = pack.section<gm20b::PackPipeline>(gm20b::PackPipelines) +
                                       pack.tier0Entries(gm20b::PackPipelines, sizeof(gm20b::PackPipeline));
    return run.first >= tier1;
}

// Its records and programs may be tier 1's, whose pages are on the card: read on a utility thread, not the one that made
// the state, whose creation line would otherwise count them
void gm20bPackNoteTier1Use(const gm20b::Pack &pack, const gm20b::PackRun &run, uint64_t fast, uint64_t name,
                           SwitchMetalPipeline *p, NSString *label, const std::string &ownKey)
{
    static std::mutex seenLock;
    static std::set<uint64_t> seenStates;
    {
        std::lock_guard<std::mutex> hold(seenLock);
        if (!seenStates.insert(name).second)
            return;
    }
    static dispatch_queue_t notes =
        dispatch_queue_create_with_target("com.brunostjohn.switch.gm20b.tier1-notes", DISPATCH_QUEUE_SERIAL,
                                          dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    const gm20b::Pack *packed = &pack;
    const gm20b::PackRun records = run;
    const std::string key = ownKey;
    NSString *named = [label copy];
    dispatch_async(notes, ^{
        const gm20b::PackHeader *hdr = packed->header();
        const gm20b::PackProgram *programs = packed->section<gm20b::PackProgram>(gm20b::PackPrograms);
        std::set<uint32_t> programIds;
        const auto collect = [&](uint32_t idx) {
            if (idx < hdr->programs)
                programIds.insert(idx);
        };
        for (const gm20b::PackPipeline *rec = records.first; rec != records.first + records.records; ++rec) {
            collect(rec->vs);
            if (gm20b::pixelStage(*rec))
                collect(rec->fs);
            if (gm20b::tessellates(*rec)) {
                collect(rec->tessPrograms[0]);
                collect(rec->tessPrograms[1]);
            }
            for (uint32_t stage = 0; stage < 2; ++stage) {
                uint32_t count = 0;
                if (const gm20b::PackVariant *vars = packed->variants(*rec, stage, count))
                    for (uint32_t i = 0; i < count; ++i)
                        collect(vars[i].program);
            }
        }
        uint32_t pieces = 0;
        for (const uint32_t idx : programIds)
            pieces += uint64_t(programs[idx].heapOffset) + programs[idx].bytes > hdr->tier0[gm20b::PackHeap];
        gPackCold.fetch_add(1, std::memory_order_relaxed);
        os_log(gm20bLog(), "SwitchMetal: tier-1 pack state %016llx %016llx in %{public}s: %u pieces, card=0",
               (unsigned long long)fast, (unsigned long long)name, getprogname(), pieces);
        gm20b::logHarvestParts(name, named, p->_packable ? gm20bStateKey(p) : key);
    });
}

void gm20bNotePackCreation(bool packed)
{
    (packed ? gPackHits : gPackMisses).fetch_add(1, std::memory_order_relaxed);
}

void gm20bPackCreations(uint64_t &hits, uint64_t &misses, uint64_t &cold, uint64_t &card)
{
    hits = gPackHits.load(std::memory_order_relaxed);
    misses = gPackMisses.load(std::memory_order_relaxed);
    cold = gPackCold.load(std::memory_order_relaxed);
    card = gPackCard.load(std::memory_order_relaxed);
}
#endif

#if SWITCH_METAL_PROF
namespace {

thread_local gm20b::CreationCost tLastCreation;

void taskEvents(uint64_t &faults, uint64_t &pageins)
{
    task_events_info_data_t events = {};
    mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    const bool read = task_info(mach_task_self(), TASK_EVENTS_INFO, task_info_t(&events), &count) == KERN_SUCCESS;
    faults = read ? uint64_t(events.faults) : 0;
    pageins = read ? uint64_t(events.pageins) : 0;
}

uint64_t ticksUs(uint64_t ticks)
{
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b;
        mach_timebase_info(&b);
        return b;
    }();
    return ticks * base.numer / base.denom / 1000;
}

gm20b::CreationWork since(const gm20b::CreationWork &now, const gm20b::CreationWork &then)
{
    gm20b::CreationWork w;
    w.keyTicks = now.keyTicks - then.keyTicks;
    w.placeTicks = now.placeTicks - then.placeTicks;
    w.variantTicks = now.variantTicks - then.variantTicks;
    w.recordTicks = now.recordTicks - then.recordTicks;
    w.lookups = now.lookups - then.lookups;
    w.lookupTicks = now.lookupTicks - then.lookupTicks;
    w.copies = now.copies - then.copies;
    w.copiedBytes = now.copiedBytes - then.copiedBytes;
    w.pieces = now.pieces - then.pieces;
    w.pieceTicks = now.pieceTicks - then.pieceTicks;
    w.builds = now.builds - then.builds;
    w.buildTicks = now.buildTicks - then.buildTicks;
    w.variants = now.variants - then.variants;
    w.textKeys = now.textKeys - then.textKeys;
    return w;
}

void logCreation(const gm20b::CreationCost &c, SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d)
{
    const gm20b::CreationWork &w = c.work;
    const uint64_t wall = ticksUs(c.wallTicks), key = ticksUs(w.keyTicks), place = ticksUs(w.placeTicks),
                   variants = ticksUs(w.variantTicks), record = ticksUs(w.recordTicks);
    const uint64_t phases = key + place + variants + record;
    os_log(gm20bLog(),
           "SwitchMetal: %{public}s: render state %016llx %{public}s in %llu us: key %llu us, place %llu us (%llu "
           "lookups %llu us, %llu pieces %llu us, %llu copies %llu bytes, %llu builds %llu us, %llu variants), packed "
           "variants %llu us, record %llu us, other %llu us; %llu text keys; %llu locks, %llu waited %llu us; %llu "
           "kernel calls %.0f us; task faults %llu, pageins %llu: %{public}s",
           getprogname(), (unsigned long long)(p ? p->_packKey : 0),
           p && p->_packed ? "from the pack" : "the pack lacks", (unsigned long long)wall, (unsigned long long)key,
           (unsigned long long)place, (unsigned long long)w.lookups, (unsigned long long)ticksUs(w.lookupTicks),
           (unsigned long long)w.pieces, (unsigned long long)ticksUs(w.pieceTicks), (unsigned long long)w.copies,
           (unsigned long long)w.copiedBytes, (unsigned long long)w.builds, (unsigned long long)ticksUs(w.buildTicks),
           (unsigned long long)w.variants, (unsigned long long)variants, (unsigned long long)record,
           (unsigned long long)(wall > phases ? wall - phases : 0), (unsigned long long)w.textKeys,
           (unsigned long long)c.locks, (unsigned long long)c.lockWaits, (unsigned long long)ticksUs(c.lockWaitTicks),
           (unsigned long long)c.kernelCalls, c.kernelUs, (unsigned long long)c.faults, (unsigned long long)c.pageins,
           d.label.UTF8String ?: "-");
}

}  // namespace

// No thread CPU clock or thread_info: backboardd's sandbox kills on thread_selfusage (syscall 482) and some refuse thread_info.
SwitchMetalCreationMeter::SwitchMetalCreationMeter() : work_(gm20b::tCreationWork), saved_(gm20b::tEncodeProfile)
{
    const uint64_t probed = mach_absolute_time();
    gm20b::LockCount::here(locks_, waits_, waitTicks_);
    taskEvents(faults_, pageins_);
    gm20b::tEncodeProfile = &profile_;
    began_ = mach_absolute_time();
    probe_ = began_ - probed;
}

SwitchMetalCreationMeter::~SwitchMetalCreationMeter()
{
    if (!finished_)
        gm20b::tEncodeProfile = saved_;
}

void SwitchMetalCreationMeter::finish(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d)
{
    const uint64_t ended = mach_absolute_time();
    finished_ = true;
    gm20b::tEncodeProfile = saved_;
    gm20b::CreationCost c;
    c.wallTicks = ended - began_;
    uint64_t locks = 0, waits = 0, waitTicks = 0, faults = 0, pageins = 0;
    gm20b::LockCount::here(locks, waits, waitTicks);
    taskEvents(faults, pageins);
    c.locks = locks - locks_;
    c.lockWaits = waits - waits_;
    c.lockWaitTicks = waitTicks - waitTicks_;
    c.faults = faults - faults_;
    c.pageins = pageins - pageins_;
    c.work = since(gm20b::tCreationWork, work_);
    c.kernelCalls = profile_.calls[size_t(gm20b::EncodePart::Kernel)];
    c.kernelUs = double(profile_.ticks[size_t(gm20b::EncodePart::Kernel)]) / gm20b::encodeTicksPerUs();
    c.probeTicks = probe_ + (mach_absolute_time() - ended);
#if SWITCH_METAL_STATS
    gm20bNotePackCreation(p && p->_packed);
#endif
    tLastCreation = c;
    const uint64_t wallUs = ticksUs(c.wallTicks);
    gm20b::noteRenderSyncCreation(wallUs);
    if (wallUs >= 1000)
        logCreation(c, p, d);
}

@implementation SwitchMetalDevice (CreationCost)

// hw_test_app's gpu.pack.hit_cost: what opening the pack took at this process's device open (Pack::load)
- (NSDictionary<NSString *, NSNumber *> *)switchMetalPackOpen
{
    return @{@"load_us" : @(ticksUs(gm20b::gPackLoadTicks))};
}

// hw_test_app's gpu.pack.hit_cost: what the calling thread's last render state creation did (creation_cost.h), in
// microseconds and counts; probe_us: what reading them took, which a release build does not pay
- (NSDictionary<NSString *, NSNumber *> *)switchMetalLastCreation
{
    const gm20b::CreationCost &c = tLastCreation;
    const gm20b::CreationWork &w = c.work;
    return @{
        @"wall_us" : @(ticksUs(c.wallTicks)),
        @"probe_us" : @(ticksUs(c.probeTicks)),
        @"key_us" : @(ticksUs(w.keyTicks)),
        @"place_us" : @(ticksUs(w.placeTicks)),
        @"variants_us" : @(ticksUs(w.variantTicks)),
        @"record_us" : @(ticksUs(w.recordTicks)),
        @"lookups" : @(w.lookups),
        @"lookup_us" : @(ticksUs(w.lookupTicks)),
        @"copies" : @(w.copies),
        @"copied_bytes" : @(w.copiedBytes),
        @"pieces" : @(w.pieces),
        @"piece_us" : @(ticksUs(w.pieceTicks)),
        @"builds" : @(w.builds),
        @"build_us" : @(ticksUs(w.buildTicks)),
        @"variants" : @(w.variants),
        @"text_keys" : @(w.textKeys),
        @"locks" : @(c.locks),
        @"lock_waits" : @(c.lockWaits),
        @"lock_wait_us" : @(ticksUs(c.lockWaitTicks)),
        @"kernel_calls" : @(c.kernelCalls),
        @"kernel_us" : @(uint64_t(c.kernelUs)),
        @"faults" : @(c.faults),
        @"pageins" : @(c.pageins),
    };
}

@end
#endif
#endif
