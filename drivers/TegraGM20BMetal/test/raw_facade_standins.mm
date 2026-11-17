// SPDX-License-Identifier: MIT
// What the facade asks of the frontend and the bridge on a host with no m2v device: the frontend's calls answer none (no
// device, no twin, no NVK), as one with no device would.
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"

extern "C" const char *nmb_last_error(void)
{
    return "no bridge on this host";
}

extern "C" {

// The translation's device-free entry points (bridge/NativeMetalDirect.h) the facade's RawOffline.mm links: none made here
NmbOfflineLibrary *nmb_offline_library_new(const void *, size_t)
{
    return nullptr;
}

void nmb_offline_library_release(NmbOfflineLibrary *)
{}

NmbOfflineFunction *nmb_offline_function_new(const NmbOfflineLibrary *, const char *)
{
    return nullptr;
}

NmbOfflineFunction *nmb_offline_function_new_specialized(const NmbOfflineLibrary *, const char *, const char *,
                                                         const struct NmbConstant *, uint32_t)
{
    return nullptr;
}

void nmb_offline_function_release(NmbOfflineFunction *)
{}

uint64_t nmb_offline_function_type(const NmbOfflineFunction *)
{
    return 0;
}

int64_t nmb_offline_function_vertex_attribute_count(const NmbOfflineFunction *)
{
    return -1;
}

size_t nmb_offline_function_vertex_attribute(const NmbOfflineFunction *, uint32_t, char *, size_t, uint64_t[5])
{
    return 0;
}

bool nmb_render_pipeline_offline(const NmbOfflineRenderPipelineDescriptor *, bool, uint64_t, uint32_t,
                                 const NmbDirectBinds *, bool (*)(const NmbDirectState *, void *), void *)
{
    return false;
}

bool nmb_compute_pipeline_offline(const NmbOfflineComputePipelineDescriptor *, const NmbDirectDispatch *,
                                  bool (*)(const NmbDirectKernel *, void *), void *)
{
    return false;
}

bool nmb_offline_compute_pipeline_compiles(const NmbOfflineComputePipelineDescriptor *)
{
    return false;
}

bool nmb_offline_compute_pipeline_facts(const NmbOfflineComputePipelineDescriptor *, uint32_t[3])
{
    return false;
}

NmbReflectionBlob *nmb_offline_render_pipeline_reflection(const NmbOfflineRenderPipelineDescriptor *)
{
    return nullptr;
}

NmbReflectionBlob *nmb_offline_compute_pipeline_reflection(const NmbOfflineComputePipelineDescriptor *)
{
    return nullptr;
}

const uint8_t *nmb_reflection_blob_bytes(const NmbReflectionBlob *, size_t *)
{
    return nullptr;
}

void nmb_reflection_blob_release(NmbReflectionBlob *)
{}
}

id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *, void *, NSUInteger, NSUInteger)
{
    return nil;
}

bool native_bridge_metal_compute_direct(id, const struct NmbDirectDispatch *,
                                        bool (*)(const struct NmbDirectKernel *, void *), void *, const char **why)
{
    *why = "no frontend on this host";
    return false;
}

id native_bridge_metal_compute_prepare(id, NSUInteger, id *, NSError **)
{
    return nil;
}

NSString *native_bridge_metal_function_library_name(id)
{
    return nil;
}

id native_bridge_metal_function_metallib(id, const void **, size_t *)
{
    return nil;
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
    *why = "no frontend on this host";
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

int native_bridge_metal_texture_memory(id, struct switch_nvk_image_memory *)
{
    return -1;
}

id native_bridge_metal_texture_tiled_in_place(id, id, NSUInteger)
{
    return nil;
}
