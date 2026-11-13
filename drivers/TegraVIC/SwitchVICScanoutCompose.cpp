// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICScanoutCompose.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"

namespace {
struct Composite {
    uint32_t references;
    SwitchVICMapping *mapping[kSwitchVICComposeSources + 1];
    TegraVICJob job;
};

void release(Composite *work)
{
    if (__atomic_sub_fetch(&work->references, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mapping)
        switch_vic_mapping_release(mapping);
    IODelete(work, Composite, 1);
}

void completed(void *context, int)
{
    release(static_cast<Composite *>(context));
}

bool sourceValid(const SwitchVICComposeSource &s, switch_gpu_backing *destination)
{
    const auto *scanout = s.scanout;
    if (!scanout || !scanout->lease || !scanout->view.native || !s.image.format || !s.image.format->planes ||
        s.image.format->planes > 2 || scanout->view.native == destination || scanout->view.logical == destination)
        return false;
    const uint64_t bytes = uint64_t(scanout->allocation.page_count) << 12;
    for (uint32_t p = 0; p < s.image.format->planes; ++p)
        if (s.image.base[p] > bytes || !s.image.span[p] || s.image.span[p] > bytes - s.image.base[p])
            return false;
    return true;
}

void publish(const TegraVICImage &layout, switch_gpu_backing *destination, SwitchNativeScanout *output)
{
    switch_gpu_backing_retain(destination);
    output->view.native = destination;
    output->view.layout.format = layout.format->fourcc;
    output->view.layout.plane_count = 1;
    auto &plane = output->view.layout.planes[0];
    plane.width = layout.width;
    plane.height = layout.height;
    plane.pitch = layout.pitch[0];
    plane.bytes = layout.span[0];
    plane.bytes_per_element = layout.format->bpe[0];
    plane.layout = layout.blockLinear ? SWITCH_GPU_BACKING_BLOCK_LINEAR : SWITCH_GPU_BACKING_PITCH;
    plane.block_height_log2 = layout.blockLinear ? layout.blockHeightLog2 : 0;
    output->view.crop = {0, 0, layout.width, layout.height};
    switch_gpu_backing_describe(destination, &output->allocation);
}
}  // namespace

int switch_vic_scanout_compose(const SwitchVICComposeSource *sources, uint32_t count, switch_gpu_backing *destination,
                               const TegraVICImage &layout, const TegraVICRect &target, SwitchNativeScanout *output)
{
    if (!output)
        return -22;
    *output = {};
    if (!sources || !count || count > kSwitchVICComposeSources || !destination || !layout.format || layout.base[0])
        return -22;
    for (uint32_t i = 0; i < count; ++i)
        if (!sourceValid(sources[i], destination))
            return -22;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return -19;
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(destination, &info);
    if (layout.span[0] > (uint64_t(info.page_count) << 12))
        return -22;
    auto *work = IONewZero(Composite, 1);
    if (!work)
        return -12;
    work->references = 1;
    int error = 0;
    for (uint32_t i = 0; i < count && !error; ++i) {
        work->mapping[i] = switch_vic_mapping_acquire(sources[i].scanout->view.native, false);
        error = work->mapping[i] ? 0 : -12;
    }
    if (!error) {
        work->mapping[count] = switch_vic_mapping_acquire(destination, true);
        error = work->mapping[count] ? 0 : -12;
    }
    TegraVICImage images[kSwitchVICComposeSources] = {};
    TegraVICComposeSlot slots[kSwitchVICComposeSources] = {};
    for (uint32_t i = 0; i < count && !error; ++i) {
        images[i] = sources[i].image;
        const uint64_t base = switch_vic_mapping_address(work->mapping[i]);
        for (uint32_t p = 0; p < images[i].format->planes; ++p) {
            if (images[i].base[p] > UINT64_MAX - base)
                error = -22;
            images[i].base[p] += base;
        }
        slots[i] = sources[i].slot;
        slots[i].source = &images[i];
    }
    if (!error) {
        TegraVICImage out = layout;
        out.base[0] = switch_vic_mapping_address(work->mapping[count]);
        if (tegra_vic_compose_build(slots, count, &out, &target, tegra_vic_traits(), tegra_vic_config_address(),
                                    tegra_vic_filter_address(), &work->job))
            error = -95;
    }
    if (!error) {
        switch_gpu_backing_use uses[2 * kSwitchVICComposeSources + 1] = {};
        switch_gpu_backing_guard guards[2 * kSwitchVICComposeSources] = {};
        uint32_t useCount = 0, guardCount = 0;
        const auto read = [&](switch_gpu_backing *backing) {
            for (uint32_t u = 0; u < useCount; ++u)
                if (uses[u].backing == backing)
                    return false;
            uses[useCount++] = {backing, SWITCH_GPU_BACKING_READ, 0};
            return true;
        };
        for (uint32_t i = 0; i < count; ++i)
            read(sources[i].scanout->view.native);
        uses[useCount++] = {destination, SWITCH_GPU_BACKING_WRITE, 0};
        for (uint32_t i = 0; i < count; ++i) {
            const auto &view = sources[i].scanout->view;
            if (!view.logical || !read(view.logical))
                continue;
            if (view.guard.generation)
                guards[guardCount++] = view.guard;
            if (view.representation.generation)
                guards[guardCount++] = view.representation;
        }
        const SwitchVICOwnership ownership = {uses, useCount, guardCount ? guards : nullptr, guardCount, nullptr, 0};
        __atomic_add_fetch(&work->references, 1, __ATOMIC_RELAXED);
        error = switch_vic_submit_checked(&work->job, 1, nullptr, ownership, completed, work, nullptr);
        if (error)
            release(work);
    }
    if (!error)
        error = switch_gpu_backing_export_read(destination, &output->view.producer, &output->lease);
    if (!error)
        publish(layout, destination, output);
    release(work);
    return error;
}
