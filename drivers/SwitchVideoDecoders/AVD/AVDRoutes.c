// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's debug-build properties for hw_test_app: SwitchPictureRoutes and SwitchDisplayBufferProbe.
#include <stdio.h>
#include "AVDDecoder.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's decode.*, decode.native_export and decode.hevc_alpha: how this session's pictures reached the client so
// far, the counts the session's closing line logs, then AVDAlpha.c's: the plane it writes VideoToolbox's target from
// and how many, and the process's alpha-plane pictures by VIC and by the CPU (a HEVC-with-alpha client reaches only the
// colour layer's session); then AVDEmitter.h's waits and latency, and 1 while job completions come on TegraNVDEC's
// port (0: its Wait route, the sandbox refused the port); then, from a debug kernel, the connection's buffer recycles
// and native exports (SwitchNVDECBufferStats): a session cycling a fixed set allocates and makes objects only until
// its pool has gone round once.
CFDictionaryRef avd_copy_picture_routes(const Decoder *d)
{
    enum {
        kAlpha = 5,
        kEmitter = 9,
        kPort = kEmitter + AVD_ROUTE_COUNT,
        kBuffers,
        kCount = kBuffers + 7
    };

    int32_t counts[kCount] = {(int32_t)d->frames, (int32_t)d->exported, (int32_t)(d->presented - d->exported),
                              (int32_t)d->copied, (int32_t)avd_emitter_errors(d->emitter)};
    avd_alpha_routes(d, &counts[kAlpha]);
    avd_emitter_routes(d->emitter, &counts[kEmitter]);
    counts[kPort] = nvdec_completions_on_port(d->client);
    SwitchNVDECBufferStats stats = {0};
    const int count = nvdec_buffer_stats(d->client, &stats) ? kCount : kBuffers;
    counts[kBuffers] = (int32_t)stats.recycles;
    counts[kBuffers + 1] = (int32_t)stats.replaced;
    counts[kBuffers + 2] = (int32_t)stats.parked;
    counts[kBuffers + 3] = (int32_t)stats.allocated;
    counts[kBuffers + 4] = (int32_t)stats.exports;
    counts[kBuffers + 5] = (int32_t)stats.export_objects;
    counts[kBuffers + 6] = (int32_t)stats.parked_most;
    const void *keys[kCount] = {CFSTR("frames"),
                                CFSTR("exported"),
                                CFSTR("vic"),
                                CFSTR("cpu"),
                                CFSTR("errors"),
                                CFSTR("first_plane"),
                                CFSTR("into_target"),
                                CFSTR("process_alpha_vic"),
                                CFSTR("process_alpha_cpu"),
                                CFSTR("picture_waits"),
                                CFSTR("sync_waits"),
                                CFSTR("dependency_waits"),
                                CFSTR("room_waits"),
                                CFSTR("in_flight_max"),
                                CFSTR("emit_latency_us"),
                                CFSTR("completion_port"),
                                CFSTR("recycles"),
                                CFSTR("recycles_replaced"),
                                CFSTR("recycles_parked"),
                                CFSTR("buffers_allocated"),
                                CFSTR("native_exports"),
                                CFSTR("native_objects"),
                                CFSTR("parked_most")};
    const void *values[kCount];
    for (int i = 0; i < count; i++)
        values[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &counts[i]);
    CFDictionaryRef routes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, count,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < count; i++)
        CFRelease(values[i]);
    return routes;
}

// hw_test_app's decode.display_buffer: HEVCDisplayProbe.h's reading of modes 2 and 3, once the session's first Main 10
// IDR picture has been decoded; nothing before.
CFDictionaryRef avd_copy_display_buffer_probe(const Decoder *d)
{
    HEVCDisplayProbeResult r[2];
    if (!hevc_decoder_display_probe(d->hevc, r))
        return NULL;
    static const char *const kFields[] = {"status", "layout", "max", "mean_milli", "trunc_milli", "round_milli"};
    CFMutableDictionaryRef out = CFDictionaryCreateMutable(kCFAllocatorDefault, 12, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    for (int mode = 0; out && mode < 2; mode++) {
        const int32_t v[] = {r[mode].status,
                             (int32_t)r[mode].layout,
                             (int32_t)r[mode].max_diff,
                             (int32_t)r[mode].mean_milli,
                             (int32_t)r[mode].trunc_milli,
                             (int32_t)r[mode].round_milli};
        os_log(gAVDLog,
               "AVD: display-buffer probe mode %d: status %d layout %d, max %d mean %d.%03d, trunc %d.%d%% "
               "round %d.%d%%",
               mode + 2, v[0], v[1], v[2], v[3] / 1000, v[3] % 1000, v[4] / 10, v[4] % 10, v[5] / 10, v[5] % 10);
        for (int f = 0; f < 6; f++) {
            char name[32];
            snprintf(name, sizeof(name), "mode%d_%s", mode + 2, kFields[f]);
            CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
            CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v[f]);
            if (key && number)
                CFDictionarySetValue(out, key, number);
            if (key)
                CFRelease(key);
            if (number)
                CFRelease(number);
        }
    }
    return out;
}
#endif
