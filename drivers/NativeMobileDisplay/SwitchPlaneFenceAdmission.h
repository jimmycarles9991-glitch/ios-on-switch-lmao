// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeFenceQueued = false;

void planeFenceWork()
{
    planeFenceQueued = false;
    switch_gpu_backing_token producers[kPlaneDependencies] = {};
    const unsigned count = planeDependencies(planeFencing, producers);
    const uint64_t pa = count ? 0 : planeFencePa(planeFencing);
    int error = kIOReturnAborted;
    while (!__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        error = count ? switch_gpu_bridge_fence(producers, count, native_legacy_gpu_complete, this)
                : pa  ? tegra_gm20b_scanout_fence(this, pa)
                      : -22;
        if (!count || error != -16)
            break;
        error = switch_gpu_bridge_wait_room();
        if (error)
            break;
        error = kIOReturnAborted;
    }
    if (error)
        native_legacy_gpu_complete(this, error);
}

void cancelQueuedPlaneFence()
{
    if (!planeFenceQueued)
        return;
    planeFenceQueued = planeFencePending = false;
    gpuDeadline->cancelTimeout();
    __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
}
