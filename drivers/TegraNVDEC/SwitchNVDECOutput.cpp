// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchNVDECOutput.h"
#include "SwitchNVDECPark.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraVIC/SwitchNativeSurface.h"
#include "../TegraVIC/TegraVICPresent.h"

extern "C" bool switch_native_surface_gpu_ready(void) __attribute__((weak));
extern "C" bool switch_native_surface_display_ready(void) __attribute__((weak));

bool switch_nvdec_native_output_ready()
{
    return tegra_vic_present_ready() && switch_native_surface_gpu_ready && switch_native_surface_gpu_ready() &&
           switch_native_surface_display_ready && switch_native_surface_display_ready();
}

// A buffer is reused in place unless a CPU owner holds it (an exported picture's read lease, which a decode job may not
// write past) or an engine still uses it that the next job's dependencies cannot order (the display). NVDEC's own jobs
// run in order, and a VIC or GPU user becomes a syncpoint wait in the next job's stream: neither costs a new buffer.
// idle: no engine has work recorded on it at all, so its memory may be given back.
int switch_nvdec_output_reusable(const TegraNVDECBlock *block, bool *idle)
{
    constexpr uint32_t kOrdered = 1u << SWITCH_GPU_ENGINE_NVDEC | 1u << SWITCH_GPU_ENGINE_VIC |
                                  1u << SWITCH_GPU_ENGINE_GM20B | 1u << SWITCH_GPU_ENGINE_NVENC;
    uint32_t engines = 0, owners = 0;
    const int read = switch_gpu_backing_pending(block->backing, SWITCH_GPU_BACKING_WRITE, &engines, &owners);
    if (read)
        return read;
    if (idle)
        *idle = !owners && !engines;
    return !owners && !(engines & ~kOrdered) ? 0 : -16;
}

int switch_nvdec_output_recycle(SwitchNVDECBuffer *buffer, task_t task, SwitchNVDECPark *park)
{
    if (!buffer || !buffer->handle || !buffer->block.backing || !buffer->map)
        return -22;
    const int available = switch_nvdec_output_reusable(&buffer->block, nullptr);
    if (available != -16)
        return available;
    if (park && switch_nvdec_park_take(park, buffer, switch_nvdec_output_reusable))
        return 0;
    TegraNVDECBlock replacement = {};
    const int allocated = tegra_nvdec_buffer_alloc(buffer->block.size, &replacement);
    if (allocated)
        return allocated;
    auto *mapping = replacement.pages.memory->createMappingInTask(task, 0, kIOMapAnywhere | kIOMapDefaultCache);
    if (!mapping) {
        tegra_nvdec_buffer_free(&replacement);
        return -12;
    }
    const auto previous = *buffer;
    *buffer = {previous.handle, replacement, mapping};
    if (park) {
        switch_nvdec_park_put(park, previous.block, previous.map);
        return 0;
    }
    previous.map->release();
    auto old = previous.block;
    tegra_nvdec_buffer_free(&old);
    return 0;
}

int switch_nvdec_output_picture(const TegraNVDECBlock *block, const TegraNVDECPresent *request,
                                TegraVICPicture *picture)
{
    if (!block || !request || !picture)
        return -22;
    const uint64_t luma = uint64_t(request->pitch) * request->rows;
    const uint64_t bytes = uint64_t(request->chroma_offset) + luma / 2;
    if (!request->pitch || request->pitch % 64 || !request->rows || request->rows % 32 ||
        request->block_height_log2 != 1 || request->chroma_offset % 256 || request->chroma_offset < luma ||
        bytes > block->size || (request->sample_bytes != 1 && request->sample_bytes != 2) ||
        (request->plane != 0 && (request->plane != 2 || request->sample_bytes != 1)) ||
        (uint64_t(request->crop_x) + request->width) * request->sample_bytes > request->pitch ||
        uint64_t(request->crop_y) + request->height > request->rows)
        return -22;
    *picture = {block->backing,  block->pages.pages, block->pages.count,     bytes,
                request->pitch,  request->rows,      request->chroma_offset, request->block_height_log2,
                request->crop_x, request->crop_y,    request->width,         request->height,
                request->dst_x,  request->dst_y,     request->surface_id,    request->sample_bytes,
                request->plane};
    return 0;
}

int switch_nvdec_output_export(const TegraNVDECBlock *block, const TegraNVDECPresent *request, task_t task,
                               SwitchNVDECExportRefusal *refusal, bool *fresh)
{
    SwitchNVDECExportRefusal unused = {};
    if (!refusal)
        refusal = &unused;
    *refusal = {"native output while VIC, the GPU or the display cannot read it", 0, 0, 0};
    if (!switch_nvdec_native_output_ready())
        return -95;
    TegraVICPicture picture = {};
    refusal->why = "native output of a picture the request does not describe";
    const int checked = switch_nvdec_output_picture(block, request, &picture);
    if (checked)
        return checked;
    refusal->why = "native output of a placed picture";
    if (request->dst_x || request->dst_y || request->plane)
        return -95;
    TegraVICSurface surface = {};
    refusal->why = "native output into a surface the caller does not hold";
    if (tegra_vic_surface_lookup(request->surface_id, task, &surface))
        return -22;
    refusal->allocationBytes = surface.desc.length;
    refusal->descriptorBytes = surface.memory ? surface.memory->getLength() : 0;
    refusal->baseOffset = surface.baseOffset;
    const int result = switch_native_surface_publish(&surface, &picture, &refusal->why, fresh);
    tegra_vic_surface_release(&surface);
    return result;
}
