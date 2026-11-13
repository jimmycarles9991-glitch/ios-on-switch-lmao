// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_object_pool.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
namespace {

constexpr const char *kPartNames[] = {"resources",
                                      "loop",
                                      "state",
                                      "pass",
                                      "split_plan",
                                      "fusion",
                                      "pass_end",
                                      "draw",
                                      "prepare",
                                      "geometry",
                                      "descriptors",
                                      "barriers",
                                      "split_launch",
                                      "blit",
                                      "compute",
                                      "tail",
                                      "commit",
                                      "prologues",
                                      "kernel",
                                      "exec",
                                      "geometry_acquire",
                                      "geometry_release",
                                      "use",
                                      "uniforms",
                                      "bind",
                                      "variant",
                                      "pass_prologue",
                                      "apply",
                                      "textures",
                                      "retire",
                                      "pass_begin"};
static_assert(sizeof kPartNames / sizeof *kPartNames == raw::kEncodeParts);

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
static_assert(sizeof kCountNames / sizeof *kCountNames == raw::kEncodeCounts);

}  // namespace

@implementation RMDevice (EncodeProfile)

// hw_test_app's gpu.path.encode_cost: this process's encode profile since the device opened (raw_encode_profile.h),
// never reset. <part>_us and <part>_self_us are microseconds, <part>_calls the spans; the events are counts, mallocs
// and frees over malloc_encodes of the encodes only; facade_locks the runtime's locks on every thread since the process
// began (raw_lock.h); <kind>_encoders_made and _freed its pooled encoders' blocks (raw_object_pool.h).
- (NSDictionary<NSString *, NSNumber *> *)switchRawEncodeProfile
{
    const raw::EncodeTotals &k = gRM->raw->stats.encode;
    const double usPerTick = 1.0 / raw::encodeTicksPerUs();
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"encodes"] = @(k.encodes.load());
    d[@"malloc_encodes"] = @(k.mallocEncodes.load());
    d[@"wall_us"] = @(double(k.wallTicks.load()) * usPerTick);
    d[@"exec_uses"] = @(k.execUses.load());
    d[@"exec_pushes"] = @(k.execPushes.load());
    d[@"facade_locks"] = @(raw::LockCount::total());
    const std::pair<const char *, Class> pooled[] = {
        {"render", [RMRenderEncoder class]}, {"blit", [RMBlitEncoder class]}, {"compute", [RMComputeEncoder class]}};
    for (const auto &[kind, cls] : pooled) {
        d[[NSString stringWithFormat:@"%s_encoders_made", kind]] = @(rawPoolMade(cls));
        d[[NSString stringWithFormat:@"%s_encoders_freed", kind]] = @(rawPoolFreed(cls));
    }
    for (size_t i = 0; i < raw::kEncodeParts; ++i) {
        d[[NSString stringWithFormat:@"%s_us", kPartNames[i]]] = @(double(k.ticks[i].load()) * usPerTick);
        d[[NSString stringWithFormat:@"%s_self_us", kPartNames[i]]] = @(double(k.self[i].load()) * usPerTick);
        d[[NSString stringWithFormat:@"%s_calls", kPartNames[i]]] = @(k.calls[i].load());
    }
    for (size_t i = 0; i < raw::kEncodeCounts; ++i)
        d[@(kCountNames[i])] = @(k.counts[i].load());
    return d;
}

@end
#endif
