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
// The system shader compiler's: no device, only what the direct compiles below need (bridge/NativeMetalCompile.h), over NVK
// at `loader`. False with nmb_last_error. A process opens this or the device, never both
bool native_bridge_metal_compile_open(const char *loader);
// A library of a metallib file or of `data`'s bytes (nil `path`) to compile directly over the compile context: its functions
// answer as a device library's do, and make no pipeline state. Nil with `error`
id native_bridge_metal_compile_library(const char *path, dispatch_data_t data, NSError **error);
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

// A render pipeline state of this device made at its first set (AsyncPipelines.h startLazy): the raw path's m2v twin of a
// state it draws itself.
id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options);
int native_bridge_metal_render_pipeline_ready(id pipeline, void (^observer)(void));
// As _ready, for a state a committed command buffer waits for: it is made on `queue` at once, ahead of the workers' queue,
// unless a worker is making it already, which then runs at that queue's class (AsyncPipelines.h claim, urge).
int native_bridge_metal_render_pipeline_urge(id pipeline, void (^observer)(void), dispatch_queue_t queue);
// The metallib a function of this device was made from: its library, which keeps `bytes` while it lives; nil when the
// function has none (the raw path names a function's AIR by its entry there).
id native_bridge_metal_function_metallib(id function, const void **bytes, size_t *length);
// The library function a function of this device was made from (its entry in that metallib), which a specialized function's name is
// not; nil for another device's.
NSString *native_bridge_metal_function_library_name(id function);
// The raw GM20B path's direct compile of a render state (bridge/NativeMetalDirect.h) of this device's functions: false with
// `why` (valid until the next call on the thread) when the translator refuses it or `each` returns false.
struct NmbDirectState;
struct NmbDirectBinds;
bool native_bridge_metal_render_direct(id descriptor, bool barrier, NSUInteger textureFormat, NSUInteger textureSamples,
                                       const struct NmbDirectBinds *binds,
                                       bool (*each)(const struct NmbDirectState *, void *), void *ctx,
                                       const char **why);
// The same of a compute state at one dispatch shape (NmbDirectDispatch)
struct NmbDirectDispatch;
struct NmbDirectKernel;
// The caller retains this descriptor unchanged; its native semantic artifact is reused by later direct dispatches.
id native_bridge_metal_compute_prepare(id descriptor, NSUInteger options, id *reflection, NSError **error);
bool native_bridge_metal_compute_save(id artifact, const char *directory, char *name, size_t name_size);
bool native_bridge_metal_compute_direct(id descriptor, const struct NmbDirectDispatch *dispatch,
                                        bool (*each)(const struct NmbDirectKernel *, void *), void *ctx,
                                        const char **why);
// The layouts of the argument buffer `function` (this device's) takes at buffer `index`, as its AIR lays them out, for the raw
// GM20B path's encoder (bridge/NativeMetalObjects.h nmb_function_argument_layouts): false with `why` (valid until the next
// call on the thread) where Metal makes no encoder, or `each` returns false
struct NmbArgumentLayout;
bool native_bridge_metal_argument_layouts(id function, NSUInteger index,
                                          bool (*each)(const struct NmbArgumentLayout *, uint32_t count, void *),
                                          void *ctx, const char **why);
// A dispatch of compute `state` (this device's) the raw GM20B path ran instead, journaled as its own dispatches are (a D line)
void native_bridge_metal_journal_dispatch(id state, MTLSize grid, MTLSize group, bool threads, const char *memory,
                                          const char *textures, const char *samplers);
// A render state of this device's functions the raw GM20B path draws from a record it completed itself, journaled as a
// creation is (an R line): its m2v twin is made only if a command buffer falls back, and the harvest learns states from
// the journal alone
void native_bridge_metal_journal_render(id descriptor, NSUInteger options);
// A symbol of the NVK this device opened (null: none)
void *native_bridge_metal_nvk_symbol(const char *name);

// Where NVK keeps a texture's texels (NVK's switch_nvk_image_memory, the same layout): one plane, level, layer and sample, its GPU
// address and size, the PTE kind its memory is mapped with, its texel size and level 0's layout. The raw GM20B path renders and
// copies a stored depth/stencil texture there, in place. 0, else why not: NVK's reason (1 no image, 2 more than one plane, 3 not
// bound, 4 more than one level, 5 more than one layer, 6 multisampled), -1 no such NVK export, -2 its texels are no one image's.
struct switch_nvk_image_memory {
    uint64_t addr, size_B, array_stride_B;
    uint32_t row_stride_B, texel_B;
    uint8_t pte_kind, compressed, tiled, y_log2, z_log2;
};

int native_bridge_metal_texture_memory(id texture, struct switch_nvk_image_memory *out);

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
