// SPDX-License-Identifier: MIT
// Entry points of the native Metal frontend over the bridge (NativeBridgeMetalFrontend.mm).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
extern "C" {
// Opens the process's one device: loader exports vkGetInstanceProcAddr (Turnip), cache is a writable shader-cache directory, registryID is the
// accelerator service's registry entry. missing (may be null) is told the class and selector of every unimplemented call.
id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID,
                            void (*missing)(const char *, const char *));
// Every Metal selector `cls` (and its subclasses) lacks that an MTL* protocol declares is answered zero/nil/NO through message
// forwarding, `answered` (may be null) told each class and selector once; respondsToSelector: still answers NO. Each selector's
// protocols are looked up once: a forwarded call costs an NSInvocation. Counted in the rates line (fwd=).
void native_bridge_metal_answer_unimplemented(Class cls, void (*answered)(const char *, const char *));
// Bare AIR bitcode as a library (the bridge's fixtures; Metal itself accepts only metallibs).
id native_bridge_metal_library_with_air(const void *bytes, size_t length);
// One line for the plugin's status report: command buffers committed/failed locally, pipelines made, refusals, the last refusal's text.
size_t native_bridge_metal_status(char *out, size_t size);
// The pixel formats of the textures asked for (first eight distinct) and how many texture requests the runtime refused.
size_t native_bridge_metal_formats(char *out, size_t size);
size_t native_bridge_metal_telemetry(char *out, size_t size);
// One line of rates since the previous call (commits/s, CPU per commit, submission and waits, surface maintenance, compiles, cache
// hits); 0 when nothing was committed or compiled since. The plugin logs it every 5 s.
size_t native_bridge_metal_rates(char *out, size_t size);
// The caller stack of one sampled commit (one per 30 s), frames as image+offset; 0 when none was sampled since the last call.
size_t native_bridge_metal_commit_origin(char *out, size_t size);
// Structured cumulative artifact/driver-cache counters for offline qualification.
struct NmbShaderCacheStats;
bool native_bridge_metal_shader_cache_stats(struct NmbShaderCacheStats *out);
bool native_bridge_metal_work_stats(uint64_t out[10]);
// Give back the GPU memory no resource holds now, as on a memory warning (GpuMemoryRelease.h).
void native_bridge_metal_release_idle_memory(void);
// The device's GPU memory held now, allocated so far, and its memoryless backings (nmb_device_memory).
bool native_bridge_metal_memory(uint64_t out[3]);
// The same fixed CPU, memory and pipeline workload on the tablet and the host, so their times compare directly (int-145): compute, first-touch
// page faults, memset bandwidth, CommonCrypto SHA-256, then the first `pairs` vertex/fragment pairs of the given QuartzCore metallib made into
// BGRA8 render states and drawn once each (pipeline_timing_host's draw=1 order). The results become the telemetry's Y rows; also returned.
size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size);
// A texture over `buffer` (this device's) rendered and sampled in place, as an IOSurface's: the raw GM20B path's m2v twin of memory
// it shares (drivers/TegraGM20BMetal/raw/runtime/RawMetal.mm). Nil if the buffer is not this device's or the runtime refuses.
id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow);
// The device's optimal image of `descriptor` (any type but a texture buffer, any levels, layers and samples) over `buffer`'s
// memory from `offset`, holding the bytes there as the device lays that image out: texels another driver of the process
// writes in the device's layout (the raw GM20B path's block-linear textures and mip chains, drivers/TegraGM20B/TegraGM20BLayout.h).
// `buffer` is a no-copy buffer of this device; over foreign memory (native_bridge_metal_buffer_over_foreign) bound under the
// image's PTE kind, the image takes the memory's own VA. Nil where the device makes no such image there.
id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset);

// The raw GM20B path's capture of a render state (raw/runtime/raw_capture.h): `commandBuffer`'s recorded NVK push segments handed
// to `segment` in submission order instead of a commit, never run, and what NVK's context init set as this process's addresses
// (*context: the pack's init relocations, raw_pack.h PackReloc). The command buffer cannot be committed after. False: refused
// (another device's, committed, commands left to record at commit, or an NVK without the capture exports).
typedef struct {
    uint64_t shaderHeap, zeroPage, cb0, images, samplers;
    uint32_t imageMax, samplerMax;
} NBCaptureContext;

bool native_bridge_metal_capture(id commandBuffer, void (^segment)(uint64_t addr, const uint32_t *cpu, uint32_t dwords),
                                 NBCaptureContext *context);
// The CPU's view of [va, va + bytes) of the captured command buffer's device memory, while the command buffer lives; null: none
const void *native_bridge_metal_capture_map(id commandBuffer, uint64_t va, uint64_t bytes);

// The program a captured draw binds at SET_PIPELINE_PROGRAM offset `program`: its 80-byte header, code and shader data as NVK
// uploads them, and each constant buffer's nvk_cbuf_type. False: none of its draws binds one there.
typedef struct {
    const void *header, *code, *data;
    uint32_t headerBytes, codeBytes, dataBytes, cbufCount;
    uint8_t cbufTypes[16];
} NBCapturedProgram;

bool native_bridge_metal_capture_program(id commandBuffer, uint32_t program, NBCapturedProgram *out);
// A render pipeline state of this device made at its first set (AsyncPipelines.h startLazy): the raw path's m2v twin of a
// state it draws itself.
id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options);
// The metallib a function of this device was made from: its library, which keeps `bytes` while it lives; nil when the
// function has none (the raw path names a function's AIR by its entry there).
id native_bridge_metal_function_metallib(id function, const void **bytes, size_t *length);

// Memory another driver of this process keeps bound in the GPU VM it shares with NVK (the raw GM20B path's: raw_gpu.h Alias): the
// BO's first byte for the CPU, its size, handle and GPU address, the PTE kind of that binding, and the lease a view holds.
struct switch_foreign_memory {
    const void *cpu;
    uint64_t bytes, handle, va;
    uint32_t kind;
    void (*hold)(void *lease);
    void (*drop)(void *lease);
    void *lease;
};

// A buffer over `length` bytes at `bytes`, inside `memory`: NVK makes its memory over memory's binding with no kernel call, and
// holds memory's lease from then until it frees it. The caller's own hold stays the caller's. Nil if the runtime refuses.
id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *memory, void *bytes, NSUInteger length,
                                           NSUInteger options);
// Every IOSurface import the frontend makes from now on is `importer`'s: it fills *memory with the surface's import and one hold
// of its lease for the frontend, so a process whose raw GM20B path imports the surface has one import of it. false: the frontend
// imports the surface itself.
typedef bool (*switch_surface_importer)(uint32_t surface_id, const void *surface, void *base, uint64_t bytes,
                                        struct switch_foreign_memory *memory);
void native_bridge_metal_set_surface_importer(switch_surface_importer importer);
}
#endif
