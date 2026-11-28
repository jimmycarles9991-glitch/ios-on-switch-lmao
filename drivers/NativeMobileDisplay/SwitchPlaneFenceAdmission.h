// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeFenceQueued = false;

void planeFenceWork()
{
    planeFenceQueued = false;
    switch_gpu_backing_token producers[kPlaneDependencies] = {};
    const unsigned count = planeDependencies(planeFencing, producers);
    if (planeBridgeArmed) {
        planeBridgeWork(producers, count);
        return;
    }
    planeFenceProducers(producers, count);
}

// The copy thread: the set's completion once every producer is done, or once the scanout fence of a set with none.
// The completion is the bridge's notify, a waiter, so a GPU producer that hangs gets the hang check, as on the stream.
void planeFenceProducers(const switch_gpu_backing_token *producers, unsigned count)
{
    const uint64_t pa = count ? 0 : planeFencePa(planeFencing);
    switch_gpu_bridge *bridge = nullptr;
    int error = kIOReturnAborted;
    while (!__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        error = count ? switch_gpu_bridge_submit(producers, count, &bridge)
                : pa  ? tegra_gm20b_scanout_fence(this, pa)
                      : -22;
        if (!count || error != -16)
            break;
        error = switch_gpu_bridge_wait_room();
        if (error)
            break;
        error = kIOReturnAborted;
    }
    if (bridge) {
        error = switch_gpu_bridge_notify(bridge, native_legacy_gpu_complete, this);
        switch_gpu_bridge_release(&bridge);
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
    gpuScanout = false;
}
