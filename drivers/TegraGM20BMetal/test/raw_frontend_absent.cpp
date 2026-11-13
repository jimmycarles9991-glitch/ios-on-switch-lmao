// SPDX-License-Identifier: MIT
// The frontend's and the bridge's entry points the raw facade links (NativeBridgeMetalFrontend.h, bridge/NativeMetalObjects.h)
// for a process whose m2v device is a stand-in (raw_facade_harness.h): each answers as the frontend does with no device of
// its own open, so nothing the facade asks of m2v is answered by anything but the stand-in it opened. A stand-in's function
// (raw_facade_compute.h) answers for itself: its metallib, its library name and its compute state; a stand-in's texture
// its image's memory.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"

static id standInAnswer(id object, const char *selector)
{
    const SEL s = sel_registerName(selector);
    return [object respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(object, s) : nil;
}

extern "C" {

id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *))
{
    return nil;
}

bool native_bridge_metal_compile_open(const char *)
{
    return false;
}

id native_bridge_metal_compile_library(const char *, dispatch_data_t, NSError **)
{
    return nil;
}

id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *, void *, NSUInteger, NSUInteger)
{
    return nil;
}

bool native_bridge_metal_compute_direct(id, const struct NmbDirectDispatch *,
                                        bool (*)(const struct NmbDirectKernel *, void *), void *, const char **why)
{
    *why = "no device";
    return false;
}

id native_bridge_metal_compute_prepare(id descriptor, NSUInteger, id *, NSError **)
{
    return standInAnswer([(MTLComputePipelineDescriptor *)descriptor computeFunction], "harnessComputeState");
}

NSString *native_bridge_metal_function_library_name(id function)
{
    return standInAnswer(function, "harnessName");
}

id native_bridge_metal_function_metallib(id function, const void **bytes, size_t *length)
{
    NSData *metallib = standInAnswer(function, "harnessMetallib");
    if (metallib) {
        *bytes = metallib.bytes;
        *length = metallib.length;
    }
    return metallib;
}

void native_bridge_metal_journal_dispatch(id, MTLSize, MTLSize, bool, const char *, const char *, const char *)
{}

void native_bridge_metal_journal_render(id, NSUInteger)
{}

bool native_bridge_metal_memory(uint64_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    return false;
}

void *native_bridge_metal_nvk_symbol(const char *)
{
    return nullptr;
}

bool native_bridge_metal_render_direct(id, bool, NSUInteger, NSUInteger, const struct NmbDirectBinds *,
                                       bool (*)(const struct NmbDirectState *, void *), void *, const char **why)
{
    *why = "no device";
    return false;
}

id native_bridge_metal_render_pipeline_lazy(id, id, NSUInteger)
{
    return nil;
}

int native_bridge_metal_render_pipeline_ready(id, void (^)(void))
{
    return -1;
}

int native_bridge_metal_render_pipeline_urge(id, void (^)(void), dispatch_queue_t)
{
    return -1;
}

void native_bridge_metal_set_surface_importer(switch_surface_importer)
{}

id native_bridge_metal_texture_in_place(id, id, NSUInteger, NSUInteger)
{
    return nil;
}

int native_bridge_metal_texture_memory(id texture, struct switch_nvk_image_memory *out)
{
    NSData *memory = standInAnswer(texture, "harnessImageMemory");
    if (memory.length != sizeof *out)
        return -3;
    memcpy(out, memory.bytes, sizeof *out);
    return 0;
}

id native_bridge_metal_texture_tiled_in_place(id, id, NSUInteger)
{
    return nil;
}

bool native_bridge_metal_argument_layouts(id, NSUInteger, bool (*)(const struct NmbArgumentLayout *, uint32_t, void *),
                                          void *, const char **why)
{
    *why = "no argument buffer at that index";
    return false;
}

const char *nmb_last_error(void)
{
    return "no device";
}

bool switch_nmb_heap_holds(uint32_t, const NmbHeapTextureFacts *)
{
    return true;
}

uint32_t switch_nmb_heap_texture_flags(const NmbHeapTextureFacts *)
{
    return 0;
}

uint32_t switch_nmb_sampler_word(const struct NmbSamplerDescriptor *)
{
    return 0;
}
}
