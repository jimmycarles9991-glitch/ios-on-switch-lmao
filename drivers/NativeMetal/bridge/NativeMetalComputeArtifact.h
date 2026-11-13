// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NmbDevice;
struct NmbComputePipelineDescriptor;
struct NmbComputeArtifact;
struct NmbDirectDispatch;
struct NmbDirectKernel;

struct NmbComputeArtifactInfo {
    uint32_t version, max_threads, execution_width, static_memory, metal_max_threads, bindings;
};

// Version 1 nodes: root, binding, struct, member, array, pointer, texture reference.
// Child indices are struct/array/texture/pointer, UINT32_MAX when absent; root/struct edges retain declaration order.
struct NmbReflectionNode {
    uint32_t kind, flags;
    const char *name;
    uint64_t values[8];
    uint32_t children[4], first, count;
};

struct NmbComputeArtifact *nmb_compute_artifact_new(struct NmbDevice *device,
                                                    const struct NmbComputePipelineDescriptor *descriptor,
                                                    bool reflection);
bool nmb_compute_artifact_key(struct NmbDevice *device, const struct NmbComputePipelineDescriptor *descriptor,
                              bool reflection, uint8_t out[32]);
struct NmbComputeArtifact *nmb_compute_artifact_import(struct NmbDevice *device,
                                                       const struct NmbComputePipelineDescriptor *descriptor,
                                                       bool reflection, const void *bytes, size_t length);
bool nmb_compute_artifact_serialize(const struct NmbComputeArtifact *artifact,
                                    bool (*write)(const void *, size_t, void *), void *context);
void nmb_compute_artifact_release(struct NmbComputeArtifact *artifact);
bool nmb_compute_artifact_info(const struct NmbComputeArtifact *artifact, struct NmbComputeArtifactInfo *out);
bool nmb_compute_artifact_reflection(const struct NmbComputeArtifact *artifact, const struct NmbReflectionNode **nodes,
                                     size_t *node_count, const uint32_t **edges, size_t *edge_count);
bool nmb_compute_artifact_direct(struct NmbDevice *device, const struct NmbComputeArtifact *artifact,
                                 const struct NmbDirectDispatch *dispatch,
                                 bool (*each)(const struct NmbDirectKernel *, void *), void *ctx);

#ifdef __cplusplus
}
#endif
