// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchVICDecodeOutput.h. The destination is a recorded write under its surface's access, as switch_vic_transform
// writes one; the decoder's buffer is a read of its backing, ordered behind the decoder by the job's syncpoint wait
// (the decoder records nothing in the backing itself).
#include <IOKit/IOLib.h>
#include "SwitchVICDecodeOutput.h"
#include "SwitchVICJobs.h"
#include "SwitchVICMapping.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchVICTransform.h"
#include "TegraVICRequest.h"

namespace {
// VIC's planar Y'CbCr inputs, which it reads into 4:4:4 and 4:2:2 semi-planar outputs (TRM chapter 15, Table 51: both
// input and output support, three surface pointers a slot). No IOSurface pixel format is described as them
// (TegraVICFormats.h lists those): only a decoder's buffer.
const TegraVICFormat kPlanar422 = {0, TEGRA_VIC_T_Y8___U8___V8_N422,  3, TEGRA_VIC_KIND_YCC_FULL, 0, {1, 1, 1}, 1,
                                   0, TEGRA_VIC_T_Y8___U8___V8_N422R, 0};
const TegraVICFormat kPlanar444 = {0, TEGRA_VIC_T_Y8___U8___V8_N444, 3, TEGRA_VIC_KIND_YCC_FULL, 0, {1, 1, 1}, 0,
                                   0, TEGRA_VIC_T_Y8___U8___V8_N444, 0};

struct DecodeOutput {
    uint32_t refs;
    SwitchVICSurfaceAccess access;
    SwitchVICMapping *mappings[2];
    TegraVICJob job;
    void (*completion)(void *, int);
    void *context;
};

void release(DecodeOutput *work)
{
    if (__atomic_sub_fetch(&work->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mappings)
        switch_vic_mapping_release(mapping);
    switch_vic_surface_release_access(&work->access);
    IODelete(work, DecodeOutput, 1);
}

void completed(void *context, int status)
{
    auto *work = static_cast<DecodeOutput *>(context);
    work->completion(work->context, status);
    release(work);
}

// The destination's planes, and the decoder's three described with the destination's standard and siting, so that the
// job moves samples and converts none.
const char *images(const TegraVICSurface *destination, const SwitchVICDecodeOutput *in, uint64_t sourceAddress,
                   uint64_t destinationAddress, TegraVICImage *src, TegraVICImage *dst)
{
    if (const char *why = tegra_vic_image_from(&destination->desc, destinationAddress, dst))
        return why;
    const TegraVICFormat *f = in->chroma444 ? &kPlanar444 : &kPlanar422;
    if (dst->format->kind != f->kind || dst->format->planes != 2 || dst->format->subY || dst->format->subX != f->subX)
        return "destination is not the decode's full-range semi-planar sampling";
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(in->source, &info);
    const uint64_t bytes = uint64_t(info.page_count) << 12;
    *src = {};
    src->format = f;
    src->width = in->width;
    src->height = in->height;
    for (uint32_t p = 0; p < 3; ++p) {
        if (in->planeOffset[p] >= bytes || uint64_t(in->pitch[p]) * in->rows[p] > bytes - in->planeOffset[p])
            return "decoded plane outside its buffer";
        src->pitch[p] = in->pitch[p];
        src->rows[p] = in->rows[p];
        src->base[p] = sourceAddress + in->planeOffset[p];
        src->span[p] = bytes - in->planeOffset[p];
    }
    src->standard = dst->standard;
    src->chromaSite = dst->chromaSite;
    src->transfer = dst->transfer;
    src->primaries = dst->primaries;
    return nullptr;
}

int place(DecodeOutput *work, const TegraVICSurface *destination, const SwitchVICDecodeOutput *in, const char **why)
{
    if (const int error = switch_vic_surface_access(destination, true, &work->access)) {
        *why = "the destination's write could not be acquired";
        return error;
    }
    const uint64_t extent = tegra_vic_surface_extent(&destination->desc);
    if (!switch_vic_pitch_backing(work->access.backing, work->access.offset + extent)) {
        *why = "a destination not laid out as rows";
        return -95;
    }
    work->mappings[0] = switch_vic_mapping_acquire(in->source, false);
    work->mappings[1] = switch_vic_mapping_acquire(work->access.backing, true);
    if (!work->mappings[0] || !work->mappings[1]) {
        *why = "VIC mappings";
        return -12;
    }
    TegraVICImage src = {}, dst = {};
    *why = images(destination, in, switch_vic_mapping_address(work->mappings[0]),
                  switch_vic_mapping_address(work->mappings[1]) + work->access.offset, &src, &dst);
    TegraVICOp op = {};
    op.srcW = uint64_t(in->width) << 16;
    op.srcH = uint64_t(in->height) << 16;
    op.dstX = in->x;
    op.dstY = in->y;
    op.dstW = in->width;
    op.dstH = in->height;
    op.nearest = 1;
    if (!*why)
        *why = tegra_vic_build(&src, &dst, &op, tegra_vic_traits(), tegra_vic_config_address(),
                               tegra_vic_filter_address(), &work->job);
    if (*why)
        return -95;
    switch_gpu_backing_use uses[2] = {{work->access.backing, SWITCH_GPU_BACKING_WRITE, 0},
                                      {in->source, SWITCH_GPU_BACKING_READ, 0}};
    switch_gpu_cpu_lease *lease = work->access.owned;
    const SwitchVICOwnership ownership = {uses, 2, &work->access.guard, 1, lease ? &lease : nullptr, lease ? 1u : 0u};
    __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
    uint32_t fence = 0;
    const int error = switch_vic_submit_checked(&work->job, 1, in->after, ownership, completed, work, &fence);
    switch_vic_surface_unreserve(&work->access);
    if (error) {
        release(work);
        *why = "VIC refused the job";
    }
    return error;
}
}  // namespace

bool switch_vic_decode_output_ready()
{
    return tegra_vic_ready();
}

int switch_vic_decode_output(const TegraVICSurface *destination, const SwitchVICDecodeOutput *output,
                             void (*completion)(void *, int), void *context, const char **why)
{
    *why = nullptr;
    if (!destination || !output || !output->source || !completion) {
        *why = "request";
        return -22;
    }
    if (!switch_vic_decode_output_ready()) {
        *why = "VIC is not up";
        return -19;
    }
    auto *work = IONewZero(DecodeOutput, 1);
    if (!work) {
        *why = "memory";
        return -12;
    }
    work->refs = 1;
    work->completion = completion;
    work->context = context;
    const int error = place(work, destination, output, why);
    release(work);
    return error;
}
