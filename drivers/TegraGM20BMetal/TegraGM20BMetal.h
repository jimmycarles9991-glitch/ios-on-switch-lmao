// SPDX-License-Identifier: MIT
// The driver as a process's Metal device (TegraGM20BMetalPlugin.mm decides: every process whose sandbox may read the pack).
//
// Every Metal object it hands out answers on the driver (gpu.h: direct MAXWELL_B methods). A command buffer is recorded as
// it is encoded and encoded on the driver at commit; what the driver cannot run fails it with Metal's error. A device
// selector it has no method for is answered zero where a Metal protocol declares it.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "device_open.h"

#ifdef __cplusplus
extern "C" {
#endif
// The render server's: starts the driver's own setup on a thread of its own (the pack read, the kernel connection, the channel
// and its context, the shader heap, the first pool chunk), so it runs beside the plugin's cache directory search;
// metal_device_open with the same pack and key takes its result. A device opened without it is another process's
// (gpu.h Device::Role::Client): each of those made at its first use, in smaller steps.
void metal_prepare(const char *packPath, uint64_t stack);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// Debug builds: the render server's prepare thread's Device::open and warm, and how long metal_device_open then waited
// for that thread, in nanoseconds (all 0 in a process that did not prepare).
void metal_prepare_split(uint64_t *open_ns, uint64_t *warm_ns, uint64_t *waited_ns);
#endif
// Turns on the runtime's completion of the Core Animation states the pack lacks (TegraGM20BMetalComplete.h): their records made off the render
// thread and kept under `cacheDir`, the process's own shader cache directory (null: in memory only), the installed native records
// under `nativeSeed` read first (RecordCache::attachSeed; null: none). After metal_device_open.
typedef bool (*switch_cache_dir_fn)(char *out, size_t size);
void metal_complete_states(const char *cacheDir, const char *nativeSeed = nullptr);
void metal_complete_states_lazy(switch_cache_dir_fn resolveCacheDir, const char *nativeSeed = nullptr);
// The pack key of a render pipeline descriptor whose functions are this facade's (the frontend journal's R-line fields but the label,
// then " | l <framework>" for functions of a framework's default library other than QuartzCore's).
NSString *metal_pipeline_key(MTLRenderPipelineDescriptor *descriptor);
// Whether every function the descriptor links is a framework library's, which the pack can name
bool metal_linked_packable(MTLRenderPipelineDescriptor *descriptor);
// One rates line since the previous call (0 when the device did nothing): command buffers on each path and why, encode time,
// draws, passes, copies, waits, memory; and the states the runtime made since the device opened (states=<records from the
// cache>/<probes>/<refused>/<failed>/<ms making them>). The plugin logs it every 5 s in debug builds.
size_t metal_rates(char *out, size_t size);
size_t metal_split_rates(char *out, size_t size);  // TegraGM20BMetalRates.mm: split twins taken and not, by why
// Debug builds: one line of the fetching draws since the previous call (0 when there were none), by how each kept raster
// order (fetch_order.h FetchCounts). The plugin logs it after the rates line.
size_t metal_fetch_rates(char *out, size_t size);
// Debug builds: "encode n=<encodes> wall= res= loop= ... pmax= ... walk= ... geov=" since the previous call (0 when
// nothing was encoded): the submit worker's encodes of raw ranges, in microseconds per encode by part (encode_profile.h;
// a part includes those nested in it, a self time leaves them out), "/x" the calls per encode, pmax= the longest
// prepare(), then the encode's events per command buffer (mallocs, locks, sends, slots per draw).
size_t metal_encode_rates(char *out, size_t size);
// Debug builds: "frame pass= hit= ... ahead= why= encoders= park= ws=" since the previous call (0 when nothing was
// recorded or waited for): the recording thread's passes by how the pass cache gave them, command buffers encoded ahead
// and why not, the pooled objects' blocks, queue heads parked by why, and waitUntilScheduled by where its time went
// (TegraGM20BMetalEncoderRates.mm)
size_t metal_frame_rates(char *out, size_t size);
// "fetch keys rec=<key>:n,... enc=... sto=...": the pipeline keys behind the window's record-time and encode-time
// refusals and storage-to-barrier fallbacks, eight of each at most, most first; 0 when none
size_t metal_fetch_keys(char *out, size_t size);
// Host tests: the CPU's view of the image the driver samples for `texture` (a planar texture's converted image), its row pitch
// in *pitch; null when the driver holds none.
const void *metal_texture_image(id<MTLTexture> texture, size_t *pitch);
// Whether the driver keeps `texture` (host tests: test/shapecheck.mm), and whether mip chains are on (the self-test at open)
bool metal_texture_kept(id<MTLTexture> texture);
bool metal_chains_on(void);
#ifdef __cplusplus
}
#endif

// A raw device over the pack at `packPath` (its stack key must be `stack`; 0: any, for host tests), an _MTLDevice of its
// own, answering registryID with `registryID`. Nil: the reason in *error.
id<MTLDevice> metal_device_open(const char *packPath, uint64_t stack, uint64_t registryID, NSString **error);
// What the last metal_device_open that returned nil met (device_open.h)
gm20b::OpenFailure metal_open_failure(void);
