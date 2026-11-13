// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNativeScanout.h"
#include "SwitchVICSurfaceAccess.h"

void switch_native_scanout_release(SwitchNativeScanout *scanout)
{
    if (!scanout)
        return;
    if (scanout->lease)
        switch_gpu_backing_cpu_release_owned(&scanout->lease, 0);
    switch_native_surface_release(&scanout->view);
    *scanout = {};
}

void switch_native_scanout_deliver(SwitchNativeScanout *scanout)
{
    if (scanout && scanout->view.owner)
        switch_native_surface_deliver(scanout->view);
}

int switch_native_scanout_acquire(OSObject *object, SwitchNativeScanout *scanout, bool turns)
{
    if (!scanout)
        return -22;
    *scanout = {};
    TegraVICSurface surface = {};
    if (switch_vic_surface_from_object(object, 0, &surface))
        return -22;
    // The display calls this under its gate while it still scans earlier frames, whose leases a publisher of this
    // surface waits for.
    int error = switch_native_surface_acquire(&surface, &scanout->view, true, turns);
    switch_gpu_cpu_lease *logical = nullptr;
    if (!error)
        error = switch_gpu_backing_conversion_reserve(scanout->view.logical, SWITCH_GPU_BACKING_READ, 1, &logical);
    if (!error) {
        const auto &view = scanout->view;
        if (__atomic_load_n(view.guard.generation, __ATOMIC_ACQUIRE) != view.guard.expected ||
            __atomic_load_n(view.representation.generation, __ATOMIC_ACQUIRE) != view.representation.expected)
            error = SWITCH_GPU_BACKING_STALE;
    }
    switch_gpu_backing_token producer = {};
    if (!error)
        error = switch_gpu_backing_export_read(scanout->view.native, &producer, &scanout->lease);
    if (!error) {
        const auto &expected = scanout->view.producer;
        if (producer.engine != expected.engine || producer.queue != expected.queue ||
            producer.reset_generation != expected.reset_generation || producer.value != expected.value)
            error = SWITCH_GPU_BACKING_STALE;
        else
            switch_gpu_backing_describe(scanout->view.native, &scanout->allocation);
    }
    if (logical)
        switch_gpu_backing_cpu_release_owned(&logical, 0);
    tegra_vic_surface_release(&surface);
    if (error)
        switch_native_scanout_release(scanout);
    return error;
}

enum class SnapshotWait {
    none,
    cpuWriters,
    cpuReader
};

static int pitchSnapshot(OSObject *object, SwitchNativeScanout *scanout, SnapshotWait how)
{
    const bool wait = how != SnapshotWait::none;
    if (!scanout)
        return -22;
    *scanout = {};
    TegraVICSurface surface = {};
    if (switch_vic_surface_from_object(object, 0, &surface))
        return -22;
    SwitchVICSurfaceAccess access;
    int error = switch_vic_surface_access(&surface, false, &access, !wait);
    switch_gpu_backing_info before = {};
    if (!error) {
        switch_gpu_backing_describe(access.backing, &before);
        if (access.owned)
            switch_gpu_backing_cpu_release_owned(&access.owned, 0);
        error = how == SnapshotWait::cpuReader
                    ? switch_gpu_backing_snapshot_read_cpu(access.backing, &scanout->view.producer, &scanout->lease)
                : wait ? switch_gpu_backing_snapshot_read_wait(access.backing, &scanout->view.producer, &scanout->lease)
                       : switch_gpu_backing_snapshot_read(access.backing, &scanout->view.producer, &scanout->lease);
    }
    if (!error) {
        switch_gpu_backing_describe(access.backing, &scanout->allocation);
        if (!wait && before.content_generation != scanout->allocation.content_generation)
            error = SWITCH_GPU_BACKING_STALE;
        else {
            scanout->pitchOffset = access.offset;
            scanout->view.native = access.backing;
            switch_gpu_backing_retain(scanout->view.native);
        }
    }
    switch_vic_surface_release_access(&access);
    tegra_vic_surface_release(&surface);
    if (error)
        switch_native_scanout_release(scanout);
    return error;
}

int switch_native_scanout_pitch(OSObject *object, SwitchNativeScanout *scanout)
{
    return pitchSnapshot(object, scanout, SnapshotWait::none);
}

int switch_native_scanout_pitch_wait(OSObject *object, SwitchNativeScanout *scanout)
{
    return pitchSnapshot(object, scanout, SnapshotWait::cpuWriters);
}

int switch_native_scanout_pitch_cpu(OSObject *object, SwitchNativeScanout *scanout)
{
    return pitchSnapshot(object, scanout, SnapshotWait::cpuReader);
}
