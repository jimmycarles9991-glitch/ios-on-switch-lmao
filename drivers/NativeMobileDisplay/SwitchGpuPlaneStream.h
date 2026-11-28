// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window B/C set the GPU renders flips by host1x stream too. The gate arms its flip at the offer, as
// SwitchMediaPlaneFence.h arms a video set's. The copy thread submits the GPU-to-host1x bridge over the set's GPU
// producers alone, which may sleep for room. Back under the gate, the display stream WAITs on the bridge's syncpoint
// and on any VIC or NVDEC producer's own, so DC A takes the set when they are done, with no CPU thread between.
// Included in the class body after SwitchMediaCompletionFlipDisplay.h.
//
// Only a jobless bridge streams: every failure of its producer cancels a stream queued on its syncpoint before the
// syncpoint passes (SwitchGpuBridge.h). A bridge with its own GPU job can pass over a killed producer's or a given-up
// promise's unfinished writes, so its set completes through the bridge, as does a set whose arm went stale or whose
// stream host1x refused: the copy thread notifies on a GPU-only set's bridge, or fences a mixed set's every producer.
//
// Ownership: planeBridgeArmed is the gate's, written before the hand-off to the copy thread, which reads it. The bridge
// handle is the copy thread's, lent to the gate for planeBridgeSubmitted, which hands it to the stream or back. A
// producer that hangs is the bridge's hang check's from its submission (switch_gpu_bridge_submit_watched) on.

static inline uint32_t planeBridgeArmed = 0;  // the flip generation armed for the bridged set; 0: the bridge fence
static inline bool planeStreamTold[3] = {};   // the gate's: each case's line, once a boot

enum PlaneStreamOutcome : uint8_t {
    kPlaneStreamed,
    kPlaneStreamStopping,
    kPlaneStreamJobful,
    kPlaneStreamStale,
    kPlaneStreamRefused,
};

static bool gpuProducer(const switch_gpu_backing_token &token)
{
    return token.engine == SWITCH_GPU_ENGINE_GM20B || token.engine == SWITCH_GPU_ENGINE_PROMISE;
}

static bool host1xProducer(const switch_gpu_backing_token &token)
{
    return token.engine == SWITCH_GPU_ENGINE_VIC || token.engine == SWITCH_GPU_ENGINE_NVDEC;
}

// Under the gate, at the offer of a set planeMediaFence left to the copy thread: its flip armed for a stream when its
// producers include the GPU's and the rest are at most a stream's VIC and NVDEC ones.
void planeBridgeArm(const PlaneSet &p)
{
    planeBridgeArmed = 0;
    switch_gpu_backing_token producers[kPlaneDependencies] = {};
    const unsigned count = planeDependencies(p, producers);
    unsigned host1x = 0;
    bool gpu = false;
    for (unsigned i = 0; i < count; ++i) {
        if (host1xProducer(producers[i]))
            ++host1x;
        else if (gpuProducer(producers[i]))
            gpu = true;
        else
            return;
    }
    if (!gpu)
        return;
    if (host1x > SwitchHost1xDisplayStream::kProducers || !armMediaFlip(p)) {
        if (stats)
            ++stPlaneGpuByCpu;
        return;
    }
    planeBridgeArmed = mediaFlip.generation();
}

// The copy thread: the bridge over the set's GPU producers, watched for a hang from its submission (in the submit's own
// lock), then the gate's answer; a set the gate did not stream completes from here.
void planeBridgeWork(const switch_gpu_backing_token *producers, unsigned count)
{
    switch_gpu_backing_token gpu[kPlaneDependencies] = {}, host1x[kPlaneDependencies] = {};
    unsigned gpuCount = 0, host1xCount = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (host1xProducer(producers[i]))
            host1x[host1xCount++] = producers[i];
        else
            gpu[gpuCount++] = producers[i];
    }
    switch_gpu_bridge *bridge = nullptr;
    int error = kIOReturnAborted;
    while (!__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        error = switch_gpu_bridge_submit_watched(gpu, gpuCount, &bridge);
        if (error != -16)
            break;
        error = switch_gpu_bridge_wait_room();
        if (error)
            break;
        error = kIOReturnAborted;
    }
    if (error) {
        native_legacy_gpu_complete(this, error);
        return;
    }
    PlaneStreamOutcome outcome = kPlaneStreamStopping;
    PlaneStreamOffer offer = {bridge, host1x, host1xCount, &outcome};
    getWorkLoop()->runAction(&planeBridgeSubmittedAction<decltype(this)>, this, &offer);
    if (outcome == kPlaneStreamed)
        return;
    if (outcome == kPlaneStreamStopping) {
        switch_gpu_bridge_release(&bridge);
        native_legacy_gpu_complete(this, kIOReturnAborted);
    } else if (!host1xCount) {
        const int notified = switch_gpu_bridge_notify(bridge, native_legacy_gpu_complete, this);
        switch_gpu_bridge_release(&bridge);
        if (notified)
            native_legacy_gpu_complete(this, notified);
    } else {
        switch_gpu_bridge_release(&bridge);
        planeFenceProducers(producers, count);
    }
}

struct PlaneStreamOffer {
    switch_gpu_bridge *bridge;
    const switch_gpu_backing_token *host1x;
    unsigned host1xCount;
    PlaneStreamOutcome *outcome;
};

template <class Self> static IOReturn planeBridgeSubmittedAction(OSObject *owner, void *offer, void *, void *, void *)
{
    auto &o = *static_cast<PlaneStreamOffer *>(offer);
    *o.outcome = static_cast<Self>(owner)->planeBridgeSubmitted(o.bridge, o.host1x, o.host1xCount);
    return kIOReturnSuccess;
}

// Under the gate: the stream over a jobless bridge while the set's flip is still the one armed. Any other answer leaves
// the bridge with the copy thread, and the set's completion (finishPlaneFence) withdraws what is armed.
PlaneStreamOutcome planeBridgeSubmitted(switch_gpu_bridge *bridge, const switch_gpu_backing_token *host1x,
                                        unsigned host1xCount)
{
    if (__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE))
        return kPlaneStreamStopping;
    PlaneStreamOutcome outcome = kPlaneStreamed;
    if (switch_gpu_bridge_jobless(bridge) != 1)
        outcome = kPlaneStreamJobful;
    else if (planeBridgeArmed != mediaFlip.generation() || mediaFlip.state() != SwitchCompletionFlip::kArmed)
        outcome = kPlaneStreamStale;
    int refusal = 0;
    if (outcome == kPlaneStreamed && (refusal = submitMediaHostFlip(host1x, host1xCount, bridge)))
        outcome = kPlaneStreamRefused;
    planeStreamCount(outcome, refusal);
    return outcome;
}

void planeStreamCount(PlaneStreamOutcome outcome, int refusal)
{
    if (outcome == kPlaneStreamed) {
        if (stats)
            ++stPlaneGpuStreamed;
        return;
    }
    if (stats)
        ++(outcome == kPlaneStreamJobful ? stPlaneGpuJobful : stPlaneGpuStreamRefused);
    const unsigned which = outcome - kPlaneStreamJobful;
    if (which >= 3 || planeStreamTold[which])
        return;
    planeStreamTold[which] = true;
    static constexpr const char *kWhy[3] = {
        "its bridge has a GPU job of its own, whose producer failures do not cancel a stream",
        "its flip was withdrawn while the bridge was submitted", "host1x refused its stream"};
    IOLog("SwitchDisplay: a GPU-rendered window B/C set completes through its bridge: %s (%d; once a boot)\n",
          kWhy[which], refusal);
}
