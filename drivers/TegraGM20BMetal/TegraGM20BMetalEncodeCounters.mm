// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "object_pool.h"

#if SWITCH_METAL_PROF
namespace {

constexpr const char *kPartNames[] = {
    "resources",     "loop",      "state",       "pass",     "split_plan",   "fusion",        "pass_end", "draw",
    "prepare",       "geometry",  "descriptors", "barriers", "split_launch", "blit",          "compute",  "tail",
    "commit",        "prologues", "kernel",      "exec",     "use",          "uniforms",      "bind",     "variant",
    "pass_prologue", "apply",     "textures",    "retire",   "pass_begin",   "variant_lookup"};
static_assert(sizeof kPartNames / sizeof *kPartNames == gm20b::kEncodeParts);

constexpr const char *kCountNames[] = {"mallocs",
                                       "frees",
                                       "locks",
                                       "sends",
                                       "applied",
                                       "uses",
                                       "use_adds",
                                       "descriptor_builds",
                                       "descriptor_uploads",
                                       "geometry_vertices"};
static_assert(sizeof kCountNames / sizeof *kCountNames == gm20b::kEncodeCounts);

}  // namespace

@implementation SwitchMetalDevice (EncodeProfile)

// hw_test_app's gpu.path.encode_cost: this process's encode profile since the device opened (encode_profile.h),
// never reset. <part>_us and <part>_self_us are microseconds, <part>_calls the spans; the events are counts, mallocs
// and frees over malloc_encodes of the encodes only; facade_locks the runtime's locks on every thread since the process
// began (lock.h); <kind>_encoders_made and _freed its pooled encoders' blocks (object_pool.h); recording_misses
// and recording_drops_<why> the kept recordings' (gm20bRecordingCounts).
- (NSDictionary<NSString *, NSNumber *> *)switchMetalEncodeProfile
{
    const gm20b::EncodeTotals &k = gRM->raw->stats.encode;
    const double usPerTick = 1.0 / gm20b::encodeTicksPerUs();
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"encodes"] = @(k.encodes.load());
    d[@"malloc_encodes"] = @(k.mallocEncodes.load());
    d[@"wall_us"] = @(double(k.wallTicks.load()) * usPerTick);
    d[@"exec_uses"] = @(k.execUses.load());
    d[@"exec_pushes"] = @(k.execPushes.load());
    d[@"facade_locks"] = @(gm20b::LockCount::total());
    const std::pair<const char *, Class> pooled[] = {{"render", [SwitchMetalRenderEncoder class]},
                                                     {"blit", [SwitchMetalBlitEncoder class]},
                                                     {"compute", [SwitchMetalComputeEncoder class]}};
    for (const auto &[kind, cls] : pooled) {
        d[[NSString stringWithFormat:@"%s_encoders_made", kind]] = @(gm20bPoolMade(cls));
        d[[NSString stringWithFormat:@"%s_encoders_freed", kind]] = @(gm20bPoolFreed(cls));
    }
    for (size_t i = 0; i < gm20b::kEncodeParts; ++i) {
        d[[NSString stringWithFormat:@"%s_us", kPartNames[i]]] = @(double(k.ticks[i].load()) * usPerTick);
        d[[NSString stringWithFormat:@"%s_self_us", kPartNames[i]]] = @(double(k.self[i].load()) * usPerTick);
        d[[NSString stringWithFormat:@"%s_calls", kPartNames[i]]] = @(k.calls[i].load());
    }
    for (size_t i = 0; i < gm20b::kEncodeCounts; ++i)
        d[@(kCountNames[i])] = @(k.counts[i].load());
    const SwitchMetalRecordingCounts recordings = gm20bRecordingCounts();
    d[@"recording_misses"] = @(recordings.misses);
    d[@"recording_drops_large"] = @(recordings.dropsLarge);
    d[@"recording_drops_total"] = @(recordings.dropsTotal);
    d[@"recording_drops_slots"] = @(recordings.dropsSlots);
    return d;
}

@end
#endif
