// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window B change whose surface only VIC or NVDEC writes (a video frame, NVDEC's export or VIC's placement): its
// present is armed on the producing engines' syncpoints (switch_host1x_token_fence), so the interrupt that ends the
// last producer completes it, with no GPU job, no bridge retirement and no copy-thread pass. NVDEC -> VIC is already
// ordered in VIC's stream (WAIT_SYNCPT on NVDEC's syncpoint). Included in the class body after
// NativePlaneFenceDisplay.h; runs under the family's work-loop gate.
//
// The completion is native_legacy_gpu_complete, as the bridge fence's: it signals gpuDoneSource for finishPlaneFence
// on the work loop, and writes the set itself when the gate armed it (SwitchMediaCompletionFlipDisplay.h). It comes
// once the engines have written the surface, with their hardware outcome: a cancelled producer, or a VIC job that ran
// on one, fails it; only a semantic failure filed after that is not seen (the bridge fence would have seen it).

// True: armed, or refused for a producer that already failed (whose error is completed here): one completion comes.
// False: a producer is the GPU's, or every flip slot is in use; the bridge fence takes the set.
bool planeMediaFence(const PlaneSet &p)
{
    switch_gpu_backing_token producers[kPlaneDependencies] = {};
    const unsigned count = planeDependencies(p, producers);
    if (!count)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (producers[i].engine != SWITCH_GPU_ENGINE_VIC && producers[i].engine != SWITCH_GPU_ENGINE_NVDEC)
            return false;
    const int error = planeFenceWithMediaFlip(p, producers, count);
    if (error == -22 || error == -16)
        return false;
    if (error)
        native_legacy_gpu_complete(this, error);
    return true;
}
