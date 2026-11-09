// SPDX-License-Identifier: MIT
// The raw GM20B path as a process's Metal device (TegraGM20BMetalPlugin.mm decides: every process whose sandbox may read the pack).
//
// Every Metal object it hands out answers on the raw path (raw_gpu.h: direct MAXWELL_B methods, one Exec per command buffer) and
// has an m2v twin over the same memory, made when first needed (the m2v device it wraps: NativeBridgeMetalFrontend.mm). A command
// buffer is recorded as it is encoded; at commit it is encoded on the raw path, or, when anything in it is something the raw path
// does not do exactly as m2v (a state not in the pack, a format or call it lacks, compute, mipmaps...), replayed whole on m2v.
// The two drivers' command buffers run in commit order: m2v's waits for the raw work before it, raw's for the m2v work before it.
// Everything a device is asked that the raw path has no own answer for goes to the m2v device (its answers stay today's).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#ifdef __cplusplus
extern "C" {
#endif
// The render server's: starts the raw path's own setup on a thread of its own (the pack read, the kernel connection, the channel
// and its context, the shader heap, the first pool chunk), so it runs beside the m2v device's open; raw_metal_device_open with
// the same pack and key takes its result. A device opened without it is another process's (raw_gpu.h Device::Role::Client): each
// of those made at its first use, in smaller steps.
void raw_metal_prepare(const char *packPath, uint64_t stack);
// A raw device over the pack at `packPath` (its stack key must be `stack`; 0: any, for host tests), wrapping the m2v device
// `fallback` (nil on a host benchmark: nothing can fall back then), or nil (the reason in *error).
id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error);
// Turns on the runtime's completion of the Core Animation states the pack lacks (RawComplete.h): their records made off the render
// thread and kept under `cacheDir`, the process's own shader cache directory (null: in memory only). After raw_metal_device_open.
void raw_metal_complete_states(const char *cacheDir);
// The pack key of a render pipeline descriptor whose functions are this facade's (the frontend journal's R-line fields but the label,
// then " | l <framework>" for functions of a framework's default library other than QuartzCore's).
NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *descriptor);
// Whether every function the descriptor links is a framework library's, which the pack can name
bool raw_metal_linked_packable(MTLRenderPipelineDescriptor *descriptor);
// One rates line since the previous call (0 when the device did nothing): command buffers on each path and why, encode time,
// draws, passes, copies, waits, memory; and the states the runtime made since the device opened (states=<records from the
// cache>/<probes>/<refused>/<failed>/<ms making them>). The plugin logs it every 5 s with the m2v line (TEGRA_M2V_TELEMETRY).
size_t raw_metal_rates(char *out, size_t size);
// Host tests: the CPU's view of the image the raw path samples for `texture` (a planar texture's converted image), its row pitch
// in *pitch; null when the raw path holds none.
const void *raw_metal_texture_image(id<MTLTexture> texture, size_t *pitch);
// Whether the raw path keeps `texture` (host tests: test/shapecheck.mm), and whether mip chains are on (the self-test at open)
bool raw_metal_texture_kept(id<MTLTexture> texture);
bool raw_metal_chains_on(void);
#ifdef __cplusplus
}
#endif
