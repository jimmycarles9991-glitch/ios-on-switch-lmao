// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchVICEncodeInput.h. The source is read as switch_vic_transform reads one (a decoder's native picture, else the
// surface's pitch rows under a checked read), the destination is the engine's buffer as a recorded write, and the job
// waits on the hardware for the engine's last read of that buffer before it overwrites it. The coded picture past the
// region (H.264's macroblocks, H.265's CTBs) gets the region's last two rows and columns stretched over it 1-tap in the
// same batch, as the copy replicated the edge, where that is within VIC's 16x upscale. A source whose rows VIC does not
// read as they are (off kTegraVICTraits.sourcePitchAlign: CoreVideo's 64-byte rows) has the region's rows copied first
// by the GPU's copy engine into the bounce scratch TegraVICBounce.h lays out, and VIC reads that; the scratch's copy is
// a recorded GPU write, so VIC's job waits for it on the hardware as TegraVIC's own bounced transfers do. A source the
// GPU keeps block linear (tiled by default) is bounced the same way, whole planes.
#include <IOKit/IOLib.h>
#include "SwitchVICEncodeInput.h"
#include "SwitchVICBouncePool.h"
#include "SwitchVICJobs.h"
#include "SwitchVICMapping.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchVICTransform.h"
#include "SwitchNativeVICImage.h"
#include "TegraVICBounce.h"
#include "../TegraGM20B/SwitchGpuSharedEvent.h"
#include "../TegraGM20B/TegraGM20BCopy.h"

namespace {
struct EncodeInput {
    uint32_t refs = 1;
    TegraVICSurface surface = {};
    SwitchVICSurfaceAccess access;
    SwitchNativeSurfaceView native = {};
    SwitchVICMapping *mappings[2] = {};
    SwitchVICBounceSlot *slot = nullptr;
    TegraVICJob jobs[3] = {};
};

void release(EncodeInput *work)
{
    if (__atomic_sub_fetch(&work->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mappings)
        switch_vic_mapping_release(mapping);
    switch_vic_surface_release_access(&work->access);
    switch_native_surface_release(&work->native);
    switch_vic_bounce_release(work->slot);
    tegra_vic_surface_release(&work->surface);
    IODelete(work, EncodeInput, 1);
}

void copied(void *context, int status)
{
    if (status) {
        static volatile int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            IOLog("TegraVIC: an encoder input's copy-engine bounce failed (%d); VIC reads what the scratch held\n",
                  status);
    }
    release(static_cast<EncodeInput *>(context));
}

// The region's rows of every plane into the scratch, at the scratch's pitch, by the GPU's copy engine; a block-linear
// (GPU-tiled) source whole planes, the copy engine's only form for a block-linear side (TegraGM20BCopy.h).
int bounce(EncodeInput *work, const SwitchVICEncodeInput *in, const SwitchVICBounceBuffer &scratch, bool tiled)
{
    const TegraVICSurfaceDesc &d = work->surface.desc;
    const TegraVICFormat *f = tegra_vic_format(d.fourcc);
    switch_gpu_cpu_lease *lease = work->access.owned;
    const switch_gpu_copy_ownership ownership = {lease ? &lease : nullptr, lease ? 1u : 0u, &work->access.guard, 1};
    const switch_gpu_event_submission none = {};
    for (uint32_t p = 0; p < f->planes; ++p) {
        const uint64_t first = tiled ? 0 : tegra_vic_plane_rows(f, in->y, p);
        const uint64_t rows = tiled ? d.planeHeight[p] : tegra_vic_plane_rows(f, in->h, p);
        const uint64_t from = d.planeOffset[p] + first * d.planeBytesPerRow[p];
        const uint64_t to = scratch.desc.planeOffset[p] + first * scratch.desc.planeBytesPerRow[p];
        if ((work->surface.baseOffset + from) % TEGRA_VIC_BOUNCE_OFFSET_ALIGN)
            return -95;
        const tegra_gm20b_ce_plane src = {work->surface.memory, work->surface.baseOffset + from,
                                          static_cast<uint32_t>(d.planeBytesPerRow[p])};
        const tegra_gm20b_ce_plane dst = {scratch.memory, to, static_cast<uint32_t>(scratch.desc.planeBytesPerRow[p])};
        const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, copied, work};
        __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
        const int error = tegra_gm20b_ce_copy_plane_checked(
            &src, &dst, tegra_vic_bounce_row_bytes(f, d.width, p), static_cast<uint32_t>(rows), nullptr, &done,
            work->access.backing, work->access.offset + from, scratch.backing, to, &ownership, &none);
        if (error) {
            release(work);
            return error;
        }
    }
    return 0;
}

void completed(void *context, int status)
{
    if (status) {
        static volatile int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            IOLog("TegraVIC: an encoder's input picture failed (%d); its encode fails\n", status);
    }
    release(static_cast<EncodeInput *>(context));
}

const char *destinationImage(const SwitchVICEncodeInput *in, uint64_t address, const TegraVICImage &source,
                             TegraVICImage *out)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(in->destination, &info);
    const uint64_t bytes = uint64_t(info.page_count) << 12;
    TegraVICImage image = {};
    image.format = tegra_vic_format(in->fourcc);
    if (!image.format || image.format->planes != 2 || !image.format->subY || image.format->kind == TEGRA_VIC_KIND_RGB)
        return "encoder input format";
    if (in->lumaOffset >= bytes || in->chromaOffset >= bytes)
        return "encoder input outside its buffer";
    image.width = in->width;
    image.height = in->height;
    const uint64_t offsets[2] = {in->lumaOffset, in->chromaOffset};
    const uint32_t rows[2] = {in->lumaRows, in->chromaRows};
    for (uint32_t plane = 0; plane < 2; ++plane) {
        image.pitch[plane] = in->pitch;
        image.rows[plane] = rows[plane];
        image.base[plane] = address + offsets[plane];
        image.span[plane] = bytes - offsets[plane];
    }
    image.blockLinear = in->blockLinear;
    image.blockHeightLog2 = in->blockHeightLog2;
    // A Y'CbCr source keeps its matrix, so the job is the channel pass-through; an RGB one takes the encoder's.
    const bool ycc = source.format->kind != TEGRA_VIC_KIND_RGB;
    image.standard = ycc ? source.standard : in->matrix;
    image.chromaSite = source.chromaSite;
    image.transfer = source.transfer;
    image.primaries = source.primaries;
    *out = image;
    return nullptr;
}

TegraVICOp copyOp(uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dx, uint32_t dy, uint32_t dw,
                  uint32_t dh)
{
    TegraVICOp op = {};
    op.srcX = uint64_t(sx) << 16;
    op.srcY = uint64_t(sy) << 16;
    op.srcW = uint64_t(sw) << 16;
    op.srcH = uint64_t(sh) << 16;
    op.dstX = dx;
    op.dstY = dy;
    op.dstW = dw;
    op.dstH = dh;
    op.nearest = 1;
    return op;
}

// The bottom and right of the coded picture from the region's last two rows and columns (two: a 4:2:0 source
// rectangle covers whole chroma samples), then the region 1:1. The region goes last: a strip's rows share GOBs with the
// picture's (chroma rows 536-543 at 1080p), and whether VIC writes a block-linear sub-rectangle by the sample or by the
// GOB is not measured. The corner past both, at most 30x30, keeps what it held.
uint32_t edgeOps(const SwitchVICEncodeInput *in, TegraVICOp *ops)
{
    constexpr uint32_t kEdge = 2, kStretch = kEdge * TEGRA_VIC_MAX_UPSCALE;
    uint32_t count = 0;
    const uint32_t below = in->height > in->h ? in->height - in->h : 0;
    const uint32_t right = in->width > in->w ? in->width - in->w : 0;
    if (below && below <= kStretch && in->h >= kEdge)
        ops[count++] = copyOp(in->x, in->y + in->h - kEdge, in->w, kEdge, 0, in->h, in->w, below);
    if (right && right <= kStretch && in->w >= kEdge)
        ops[count++] = copyOp(in->x + in->w - kEdge, in->y, kEdge, in->h, in->w, 0, right, in->h);
    ops[count++] = copyOp(in->x, in->y, in->w, in->h, 0, 0, in->w, in->h);
    return count;
}

int place(EncodeInput *work, uint32_t surfaceID, task_t task, const SwitchVICEncodeInput *in, uint32_t *fence,
          switch_gpu_backing_token *written, bool *bounced, const char **why)
{
    if (tegra_vic_surface_lookup(surfaceID, task, &work->surface)) {
        *why = "surface ID not found in the encoding task";
        return -22;
    }
    const int native = switch_native_surface_acquire(&work->surface, &work->native, false);
    int error = native;
    if (native == -2 || native == -95 || native == SWITCH_GPU_BACKING_STALE)
        error = switch_vic_surface_access(&work->surface, false, &work->access);
    if (error) {
        *why = "the surface's read could not be acquired";
        return error;
    }
    auto *input = work->native.owner ? work->native.native : work->access.backing;
    const uint64_t extent = tegra_vic_surface_extent(&work->surface.desc);
    bool tiled = false;
    if (!work->native.owner) {
        switch_gpu_backing_info info = {};
        switch_gpu_backing_describe(input, &info);
        if (extent > UINT64_MAX - work->access.offset ||
            work->access.offset + extent > static_cast<uint64_t>(info.page_count) * 4096) {
            *why = "a source surface larger than its memory";
            return -95;
        }
        tiled = !switch_vic_pitch_backing(input, work->access.offset + extent);
    }
    const int needed = work->native.owner ? 1
                                          : tegra_vic_bounce_needed(&work->surface.desc,
                                                                    tegra_vic_pitch_align(tegra_vic_traits(), true));
    if (needed < 0) {
        *why = "a source plane the copy engine cannot take either";
        return -95;
    }
    *bounced = needed == 0 || tiled;
    if (*bounced && (error = switch_vic_bounce_acquire(&work->surface.desc, nullptr, &work->slot))) {
        *why = "the bounce scratch";
        return error;
    }
    const SwitchVICBounceBuffer *scratch = *bounced ? &work->slot->buffers[0] : nullptr;
    if (!scratch)
        work->mappings[0] = switch_vic_mapping_acquire(input, false);
    work->mappings[1] = switch_vic_mapping_acquire(in->destination, true);
    if ((!scratch && !work->mappings[0]) || !work->mappings[1]) {
        *why = "VIC mappings";
        return -12;
    }
    TegraVICImage src = {}, dst = {};
    const uint64_t address = switch_vic_mapping_address(scratch ? scratch->mapping : work->mappings[0]);
    if (work->native.owner)
        *why = switch_native_vic_image(work->native, address, &src);
    else
        *why = scratch ? tegra_vic_image_from(&scratch->desc, address, &src)
                       : tegra_vic_image_from(&work->surface.desc, address + work->access.offset, &src);
    if (!*why)
        *why = destinationImage(in, switch_vic_mapping_address(work->mappings[1]), src, &dst);
    TegraVICOp ops[3] = {};
    const uint32_t count = edgeOps(in, ops);
    for (uint32_t i = 0; !*why && i < count; ++i) {
        if (work->native.owner && !switch_native_vic_crop(work->native, &ops[i]))
            *why = "native source crop";
        else
            *why = tegra_vic_build(&src, &dst, &ops[i], tegra_vic_traits(), tegra_vic_config_address(),
                                   tegra_vic_filter_address(), &work->jobs[i]);
    }
    if (*why)
        return -95;
    if (scratch && (error = bounce(work, in, *scratch, tiled))) {
        *why = "the copy engine refused the bounce";
        return error;
    }
    // The bounced source stays listed: its representation guard is evaluated with it locked.
    switch_gpu_backing_use uses[3] = {
        {in->destination, SWITCH_GPU_BACKING_WRITE, 0},
        {scratch ? scratch->backing : input, SWITCH_GPU_BACKING_READ, 0},
        {work->native.owner ? work->native.logical : input,
         SWITCH_GPU_BACKING_READ | (work->native.owner ? SWITCH_GPU_BACKING_NO_CPU_EXCLUSION : 0u), 0}};
    switch_gpu_backing_guard guards[2] = {};
    uint32_t guardCount = 0;
    if (work->native.owner) {
        guards[guardCount++] = work->native.guard;
        guards[guardCount++] = work->native.representation;
    } else {
        guards[guardCount++] = work->access.guard;
    }
    switch_gpu_cpu_lease *lease = work->access.owned;
    const SwitchVICOwnership ownership = {
        uses, work->native.owner || scratch ? 3u : 2u, guards, guardCount, lease ? &lease : nullptr, lease ? 1u : 0u};
    __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
    error =
        switch_vic_submit_checked(work->jobs, count, in->after, ownership, completed, work, fence, nullptr, written);
    switch_vic_surface_unreserve(&work->access);
    if (error) {
        release(work);
        *why = "VIC refused the job";
    }
    return error;
}
}  // namespace

bool switch_vic_encode_input_ready(bool blockLinear)
{
    const auto *traits = tegra_vic_traits();
    return tegra_vic_ready() && traits && (!blockLinear || traits->blockLinearOut);
}

int switch_vic_encode_input(uint32_t surfaceID, task_t task, const SwitchVICEncodeInput *input, uint32_t *fence,
                            switch_gpu_backing_token *written, bool *bounced, const char **why)
{
    *why = nullptr;
    *bounced = false;
    if (!input || !input->destination || !fence || !written) {
        *why = "request";
        return -22;
    }
    if (!switch_vic_encode_input_ready(input->blockLinear)) {
        *why = "VIC is not up or has not proven block-linear output";
        return -19;
    }
    auto *work = IONewZero(EncodeInput, 1);
    if (!work) {
        *why = "memory";
        return -12;
    }
    work->refs = 1;
    const int error = place(work, surfaceID, task, input, fence, written, bounced, why);
    release(work);
    return error;
}
