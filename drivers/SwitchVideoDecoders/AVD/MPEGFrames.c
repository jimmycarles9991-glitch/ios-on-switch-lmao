// SPDX-License-Identifier: BSD-2-Clause-Patent
// See MPEGFrames.h.
#include "MPEGFrames.h"
#include <string.h>
#include "../../TegraNVDEC/nvidia/clc5b0.h"

#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

void mpeg_frames_release(MPEGFrames *f)
{
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (f->handle[i])
            f->b.free(f->b.ctx, f->handle[i]);
        f->handle[i] = 0;
        f->va[i] = NULL;
    }
    nvdec_ring_release(&f->ring, &f->b);
    f->past = f->future = -1;
}

int mpeg_frames_configure(MPEGFrames *f, uint32_t width_mbs, uint32_t height_mbs, uint32_t tables)
{
    mpeg_frames_release(f);
    f->width_mbs = width_mbs;
    f->height_mbs = height_mbs;
    f->pitch = ALIGN(width_mbs * 16, 64);
    f->rows = ALIGN(height_mbs * 16, 32);
    f->luma_bytes = f->pitch * f->rows;
    f->bytes = f->luma_bytes + f->luma_bytes / 2;
    void *va;
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (f->b.alloc(f->b.ctx, f->bytes, &f->handle[i], &va)) {
            f->handle[i] = 0;
            mpeg_frames_release(f);
            return -3;
        }
        f->va[i] = va;
    }
    f->bits_off = MPEG_FRAMES_TABLES + ALIGN(tables, 256);
    if (nvdec_ring_alloc(&f->ring, &f->b,
                         ALIGN(f->bits_off + (uint64_t)width_mbs * height_mbs * 192 + 0x10000, 0x10000))) {
        mpeg_frames_release(f);
        return -3;
    }
    return 0;
}

int mpeg_frames_take(MPEGFrames *f)
{
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (i == f->past || i == f->future)
            continue;
        if (f->b.recycle) {
            void *address = f->va[i];
            if (f->b.recycle(f->b.ctx, f->handle[i], f->bytes, &address))
                return -1;
            f->va[i] = address;
        }
        return i;
    }
    return -1;
}

void mpeg_frames_reference(MPEGFrames *f, int index)
{
    f->past = f->future;
    f->future = (int8_t)index;
}

NVDECInput *mpeg_frames_input(MPEGFrames *f)
{
    return nvdec_ring_current(&f->ring);
}

int mpeg_frames_append(MPEGFrames *f, const uint8_t *data, size_t size, bool reset)
{
    if (reset)
        f->bits_len = 0;
    const uint64_t needed = (uint64_t)f->bits_off + f->bits_len + size;
    if (needed > mpeg_frames_input(f)->bytes &&
        nvdec_ring_grow(&f->ring, &f->b, needed, (uint64_t)f->bits_off + f->bits_len))
        return -1;
    memcpy(mpeg_frames_input(f)->va + f->bits_off + f->bits_len, data, size);
    f->bits_len += (uint32_t)size;
    return 0;
}

void mpeg_frames_entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

void mpeg_frames_bind(MPEGFrames *f, TegraNVDECSubmit *s, int current, int past, int future)
{
    mpeg_frames_entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, f->picture_index++, 0);
    const uint32_t input = mpeg_frames_input(f)->handle;
    mpeg_frames_entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, f->bits_off);
    mpeg_frames_entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, 0);
    const int slots[3] = {current, past, future};
    for (uint16_t i = 0; i < 3; i++) {
        const uint32_t handle = f->handle[slots[i]];
        mpeg_frames_entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, handle, 0);
        mpeg_frames_entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, handle,
                          f->luma_bytes);
    }
}

int64_t mpeg_frames_run(MPEGFrames *f, TegraNVDECSubmit *s, TegraNVDECRange clean)
{
    mpeg_frames_entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    const NVDECInput *input = mpeg_frames_input(f);
    s->clean[s->clean_count++] = (TegraNVDECRange){input->handle, f->bits_off, f->bits_len, 0};
    if (clean.length)
        s->clean[s->clean_count++] = clean;
    uint32_t fence = 0;
    if (f->b.submit(f->b.ctx, s, &fence))
        return -2;
    nvdec_ring_advance(&f->ring);
    return fence;
}

void mpeg_frames_output(MPEGFrames *f, int index, uint32_t width, uint32_t height, uint8_t fields, uint32_t job,
                        void *token)
{
    const H264Picture picture = {
        .luma = f->va[index],
        .chroma = f->va[index] + f->luma_bytes,
        .pitch = f->pitch,
        .handle = f->handle[index],
        .chroma_offset = f->luma_bytes,
        .rows = f->rows,
        .coded_width = f->width_mbs * 16,
        .coded_height = f->height_mbs * 16,
        .width = width,
        .height = height,
        .fields = fields,
        .job = job,
        .token = token,
    };
    void *item = f->b.decoded(f->b.ctx, &picture);
    if (item)
        f->b.emit(f->b.ctx, item);
}
