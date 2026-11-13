// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <switch/memory_attachment.h>
#include "SwitchVICSurfaceAccess.h"
#include "SwitchNativeSurface.h"
#include "../TegraGM20B/TegraGM20BViews.h"

int switch_vic_surface_access(const TegraVICSurface *surface, bool write, SwitchVICSurfaceAccess *access, bool avoid)
{
    if (!surface || !surface->surface || !surface->memory || !access)
        return -22;
    *access = {};
    int error = SwitchMemoryAttachment::enterCpu(surface->memory, avoid);
    if (error)
        return error;
    access->descriptor = surface->memory;
    error = switch_native_surface_prepare_gpu(surface->surface, surface->id, nullptr,
                                              (write ? 0u : 1u) | (avoid ? 2u : 0u), &access->backing, &access->owned);
    if (!error) {
        if (access->backing)
            access->offset = surface->baseOffset;
        else
            access->backing = switch_vic_surface_backing(surface);
        auto *generation = SwitchMemoryAttachment::generation(surface->memory);
        if (!access->backing || !generation)
            error = -12;
        else
            access->guard = {access->backing, generation, __atomic_load_n(generation, __ATOMIC_ACQUIRE)};
    }
    if (error)
        switch_vic_surface_release_access(access);
    return error;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *access)
{
    if (access->owned)
        switch_gpu_backing_cpu_release_owned(&access->owned, 0);
    if (access->descriptor)
        switch_gpu_surface_finish(access->descriptor);
    access->descriptor = nullptr;
}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access)
{
    switch_vic_surface_unreserve(access);
    if (access->backing)
        switch_gpu_backing_release(access->backing);
    *access = {};
}
