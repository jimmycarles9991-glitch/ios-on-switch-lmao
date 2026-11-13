// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICScanoutConvert.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"

namespace {
struct Conversion {
    uint32_t references;
    SwitchVICMapping *mapping[2];
    TegraVICJob job;
};

void release(Conversion *work)
{
    if (__atomic_sub_fetch(&work->references, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mapping)
        switch_vic_mapping_release(mapping);
    IODelete(work, Conversion, 1);
}

void completed(void *context, int)
{
    release(static_cast<Conversion *>(context));
}

// The single-plane destination `out` describes, offsets from its first byte, whole inside its pages.
bool fits(const TegraVICImage &out, switch_gpu_backing *destination)
{
    if (!out.format || out.format->planes != 1 || !out.width || !out.height || out.width > 16384 ||
        out.height > 16384 || out.pitch[0] < uint64_t(out.width) * 4 || !out.span[0] || out.base[0] & 255)
        return false;
    switch_gpu_backing_info target = {};
    switch_gpu_backing_describe(destination, &target);
    return out.base[0] <= (uint64_t(target.page_count) << 12) &&
           out.span[0] <= (uint64_t(target.page_count) << 12) - out.base[0];
}

int place(const SwitchNativeScanout *source, const TegraVICImage *image, switch_gpu_backing *destination,
          const TegraVICImage &layout, const TegraVICOp *operation, const SwitchVICPlaceReads &also,
          SwitchNativeScanout *output)
{
    if (!output || output == source)
        return -22;
    *output = {};
    if (!source || !source->lease || !source->view.native || !image || !image->format || !destination || !operation ||
        !image->format->planes || image->format->planes > 2 || source->view.native == destination ||
        source->view.logical == destination || also.count > kSwitchVICPlaceReads || !fits(layout, destination))
        return -22;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return -19;
    const uint64_t sourceBytes = uint64_t(source->allocation.page_count) << 12;
    for (uint32_t p = 0; p < image->format->planes; ++p)
        if (image->base[p] > sourceBytes || !image->span[p] || image->span[p] > sourceBytes - image->base[p])
            return -22;
    auto *work = IONewZero(Conversion, 1);
    if (!work)
        return -12;
    work->references = 1;
    work->mapping[0] = switch_vic_mapping_acquire(source->view.native, false);
    work->mapping[1] = switch_vic_mapping_acquire(destination, true);
    int error = !work->mapping[0] || !work->mapping[1] ? -12 : 0;
    if (!error) {
        TegraVICImage input = *image, out = layout;
        const uint64_t base = switch_vic_mapping_address(work->mapping[0]);
        for (uint32_t p = 0; p < image->format->planes; ++p) {
            if (input.base[p] > UINT64_MAX - base)
                error = -22;
            input.base[p] += base;
        }
        out.transfer = input.transfer;
        out.primaries = input.primaries;
        out.base[0] += switch_vic_mapping_address(work->mapping[1]);
        if (!error && tegra_vic_build(&input, &out, operation, tegra_vic_traits(), tegra_vic_config_address(),
                                      tegra_vic_filter_address(), &work->job))
            error = -95;
    }
    if (!error) {
        switch_gpu_backing_use uses[3 + kSwitchVICPlaceReads] = {{source->view.native, SWITCH_GPU_BACKING_READ, 0},
                                                                 {destination, SWITCH_GPU_BACKING_WRITE, 0}};
        uint32_t count = 2, guardCount = 0;
        switch_gpu_backing_guard guards[2] = {};
        if (source->view.logical) {
            uses[count++] = {source->view.logical, SWITCH_GPU_BACKING_READ, 0};
            if (source->view.guard.generation)
                guards[guardCount++] = source->view.guard;
            if (source->view.representation.generation)
                guards[guardCount++] = source->view.representation;
        }
        for (uint32_t i = 0; i < also.count; ++i)
            if (also.backings[i])
                uses[count++] = {also.backings[i], SWITCH_GPU_BACKING_READ, 0};
        const SwitchVICOwnership ownership = {uses, count, guards, guardCount, nullptr, 0};
        __atomic_add_fetch(&work->references, 1, __ATOMIC_RELAXED);
        error = switch_vic_submit_checked(&work->job, 1, nullptr, ownership, completed, work, nullptr);
        if (error)
            release(work);
    }
    if (!error)
        error = switch_gpu_backing_export_read(destination, &output->view.producer, &output->lease);
    if (!error) {
        switch_gpu_backing_retain(destination);
        output->view.native = destination;
        output->view.layout.format = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
        output->view.layout.plane_count = 1;
        output->view.layout.transfer = image->transfer;
        output->view.layout.primaries = image->primaries;
        auto &plane = output->view.layout.planes[0];
        plane.offset = layout.base[0];
        plane.width = layout.width;
        plane.height = layout.height;
        plane.pitch = layout.pitch[0];
        plane.bytes = layout.span[0];
        plane.bytes_per_element = 4;
        plane.layout = layout.blockLinear ? SWITCH_GPU_BACKING_BLOCK_LINEAR : SWITCH_GPU_BACKING_PITCH;
        plane.block_height_log2 = layout.blockLinear ? layout.blockHeightLog2 : 0;
        output->view.crop = {0, 0, layout.width, layout.height};
        switch_gpu_backing_describe(destination, &output->allocation);
    }
    release(work);
    return error;
}
}  // namespace

int switch_vic_scanout_convert(const SwitchNativeScanout *source, const TegraVICImage *image,
                               switch_gpu_backing *destination, uint32_t width, uint32_t height, uint32_t pitch,
                               const TegraVICOp *operation, SwitchNativeScanout *output)
{
    if (output && output != source)
        *output = {};
    if (!output || output == source || pitch < uint64_t(width) * 4 || (pitch & 255))
        return -22;
    TegraVICImage layout = {};
    layout.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    layout.width = width;
    layout.height = height;
    layout.pitch[0] = pitch;
    layout.rows[0] = height;
    layout.span[0] = uint64_t(pitch) * height;
    return place(source, image, destination, layout, operation, {}, output);
}

int switch_vic_scanout_place(const SwitchNativeScanout *source, const TegraVICImage *image,
                             switch_gpu_backing *destination, const TegraVICImage *layout, const TegraVICOp *operation,
                             const SwitchVICPlaceReads &also, SwitchNativeScanout *output)
{
    if (!layout)
        return -22;
    return place(source, image, destination, *layout, operation, also, output);
}
