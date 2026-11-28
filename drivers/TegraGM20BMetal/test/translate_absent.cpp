// SPDX-License-Identifier: MIT
// The translation's entry points (translate/NativeMetalTranslate.h) that the facade's TegraGM20BMetalOffline.mm and
// TegraGM20BMetalOfflineReflection.mm link, for a host test that links no translation: none made here.
#include "../translate/NativeMetalTranslate.h"

extern "C" {

const char *nmb_last_error(void)
{
    return "no device";
}

NmbOfflineLibrary *nmb_offline_library_new(const void *, size_t)
{
    return nullptr;
}

void nmb_offline_library_release(NmbOfflineLibrary *)
{}

NmbOfflineLibrary *nmb_offline_library_new_held(const void *, size_t, uint64_t, const void *owner,
                                                void (*release)(const void *owner))
{
    release(owner);
    return nullptr;
}

uint64_t nmb_offline_scope_new()
{
    return 1;
}

void nmb_keep_translations(uint64_t)
{}

void nmb_kept_translations(uint64_t out[2])
{
    out[0] = out[1] = 0;
}

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

int64_t nmb_offline_function_stage_input_attribute_count(const NmbOfflineFunction *)
{
    return -1;
}

size_t nmb_offline_function_stage_input_attribute(const NmbOfflineFunction *, uint32_t, char *, size_t, uint64_t[5])
{
    return 0;
}

bool nmb_offline_function_buffer_encoders(const NmbOfflineFunction *,
                                          bool (*)(uint32_t, bool, const struct NmbArgumentLayout *, uint32_t,
                                                   const char *, void *),
                                          void *)
{
    return false;
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
