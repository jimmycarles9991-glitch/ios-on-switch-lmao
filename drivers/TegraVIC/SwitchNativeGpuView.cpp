// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNativeSurface.h"
#include "SwitchVICSurfaceAccess.h"
#include <IOKit/IOMemoryDescriptor.h>
#include "../TegraGM20B/SwitchGpuNativeSurface.h"

namespace {
int pitchView(const TegraVICSurface &surface, switch_gpu_native_surface_view *out)
{
    const auto &desc = surface.desc;
    if ((desc.fourcc != 0x34323076 && desc.fourcc != 0x34323066) || desc.planes != 2 || desc.protection)
        return -2;
    if (!desc.width || !desc.height)
        return -22;
    SwitchVICSurfaceAccess access;
    int error = switch_vic_surface_access(&surface, false, &access);
    if (error)
        return error;
    switch_gpu_backing_layout layout = {};
    layout.format = desc.fourcc;
    layout.plane_count = 2;
    layout.color_matrix = desc.colorMatrix;
    layout.transfer = desc.transfer;
    layout.primaries = desc.primaries;
    layout.chroma_top = desc.chromaTop;
    layout.chroma_bottom = desc.chromaBottom;
    layout.color_range = desc.fourcc == 0x34323066 ? 2 : 1;
    for (unsigned p = 0; p < 2; ++p) {
        const uint32_t width = p ? desc.width / 2 + (desc.width & 1) : desc.width;
        const uint32_t height = p ? desc.height / 2 + (desc.height & 1) : desc.height;
        const uint32_t sample = p ? 2 : 1;
        if (desc.planeWidth[p] > UINT32_MAX || desc.planeHeight[p] > UINT32_MAX ||
            desc.planeBytesPerRow[p] > UINT32_MAX) {
            error = -22;
            break;
        }
        const uint64_t bytes = desc.planeBytesPerRow[p] * desc.planeHeight[p];
        const uint64_t offset = desc.planeOffset[p];
        if (desc.planeWidth[p] < width || desc.planeHeight[p] < height ||
            uint64_t(desc.planeWidth[p]) * sample > desc.planeBytesPerRow[p] || offset > desc.length ||
            bytes > desc.length - offset || offset > UINT64_MAX - access.offset) {
            error = -22;
            break;
        }
        layout.planes[p] = {offset + access.offset,
                            bytes,
                            uint32_t(desc.planeWidth[p]),
                            uint32_t(desc.planeHeight[p]),
                            uint32_t(desc.planeBytesPerRow[p]),
                            sample,
                            SWITCH_GPU_BACKING_PITCH,
                            0};
    }
    if (!error && layout.planes[0].offset + layout.planes[0].bytes > layout.planes[1].offset)
        error = -22;
    if (!error) {
        surface.memory->retain();
        switch_gpu_backing_retain(access.backing);
        *out = {nullptr, surface.memory, access.backing, access.backing, {}, access.guard, {}, layout, 0,
                0,       desc.width,     desc.height};
        access.backing = nullptr;
    }
    switch_vic_surface_release_access(&access);
    return error;
}
}

extern "C" int switch_native_surface_gpu_acquire(void *object, switch_gpu_native_surface_view *out)
{
    if (!out)
        return -22;
    *out = {};
    TegraVICSurface surface = {};
    const int described = switch_vic_surface_from_object(static_cast<OSObject *>(object), 0, &surface);
    if (described)
        return described;
    SwitchNativeSurfaceView view = {};
    int error = switch_native_surface_acquire(&surface, &view, false);
    if (error == -2 || error == -95 || error == SWITCH_GPU_BACKING_STALE) {
        error = pitchView(surface, out);
    } else if (!error) {
        *out = {view.owner,    view.descriptor, view.logical, view.native, view.guard,      view.representation,
                view.producer, view.layout,     view.crop.x,  view.crop.y, view.crop.width, view.crop.height};
    }
    tegra_vic_surface_release(&surface);
    return error;
}

extern "C" void switch_native_surface_gpu_release(switch_gpu_native_surface_view *view)
{
    if (!view)
        return;
    SwitchNativeSurfaceView held = {static_cast<OSObject *>(view->owner),
                                    static_cast<IOMemoryDescriptor *>(view->descriptor),
                                    view->logical,
                                    view->native,
                                    view->payload,
                                    view->representation,
                                    view->producer,
                                    view->layout,
                                    {view->crop_x, view->crop_y, view->crop_width, view->crop_height},
                                    {},
                                    nullptr,
                                    0};
    *view = {};
    switch_native_surface_release(&held);
}
