// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVDECRing.h.
#include "NVDECRing.h"
#include <string.h>

int nvdec_ring_alloc(NVDECRing *ring, const H264Backend *b, uint64_t bytes)
{
    nvdec_ring_release(ring, b);
    const uint32_t count = b->inputs < 1 ? 1 : b->inputs > NVDEC_RING_MAX ? NVDEC_RING_MAX : b->inputs;
    for (uint32_t i = 0; i < count; i++) {
        NVDECInput *input = &ring->input[i];
        void *va;
        if (b->alloc(b->ctx, bytes, &input->handle, &va)) {
            input->handle = 0;
            nvdec_ring_release(ring, b);
            return -1;
        }
        input->va = va;
        input->bytes = bytes;
        ring->count = i + 1;
    }
    return 0;
}

void nvdec_ring_release(NVDECRing *ring, const H264Backend *b)
{
    for (uint32_t i = 0; i < ring->count; i++)
        if (ring->input[i].handle)
            b->free(b->ctx, ring->input[i].handle);
    memset(ring, 0, sizeof(*ring));
}

NVDECInput *nvdec_ring_current(NVDECRing *ring)
{
    return ring->count ? &ring->input[ring->next] : NULL;
}

int nvdec_ring_grow(NVDECRing *ring, const H264Backend *b, uint64_t needed, uint64_t keep)
{
    NVDECInput *input = nvdec_ring_current(ring);
    if (!input)
        return -1;
    uint64_t bytes = input->bytes;
    while (bytes < needed)
        bytes *= 2;
    if (bytes > (32u << 20))
        return -1;
    uint32_t handle;
    void *va;
    if (b->alloc(b->ctx, bytes, &handle, &va))
        return -1;
    memcpy(va, input->va, keep < input->bytes ? keep : input->bytes);
    b->free(b->ctx, input->handle);
    *input = (NVDECInput){handle, va, bytes};
    return 0;
}

void nvdec_ring_advance(NVDECRing *ring)
{
    if (ring->count)
        ring->next = (ring->next + 1) % ring->count;
}
