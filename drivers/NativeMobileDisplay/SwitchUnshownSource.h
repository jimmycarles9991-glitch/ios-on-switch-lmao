// SPDX-License-Identifier: BSD-2-Clause-Patent
// A swapped frame its source does not hold (TegraScanoutCopy.h TEGRA_SCANOUT_SOURCE_*), whichever way the display
// learns it: an offer's answer, a completion's status or a snapshot it took. A producer the display waits on itself (a
// bridge or host1x fence, external_wait) reports a plain failure, which producerFailure reads as the GPU driver's answers
// say it. Included in the class body; under the gate.

// The family's id of a swap still in its queue; 0 for one it let go (retired at its offer, or discarded).
uint32_t swapOf(const OriginalLegacySwapRequest *request) const
{
    uint32_t id = 0;
    for (const OriginalLegacySwapRequest *queued = pendingSwapRequest(); queued; queued = queuedAfter(queued))
        if (queued == request) {
            memcpy(&id, reinterpret_cast<const unsigned char *>(request) + swapId, sizeof(id));
            break;
        }
    return id;
}

// GONE (its writer's client ended its work) and REFUSED (its write was never accepted): one line, and true: the caller
// retires the swap unshown and the panel keeps the frame before it. LOST, accepted work the GPU or an engine failed, is
// the kernel's own failure: a panic. Any other status says nothing of the source: false.
bool sourceUnshown(int status, const OriginalLegacySwapRequest *request, const char *path) const
{
    if (status == TEGRA_SCANOUT_SOURCE_LOST)
        panic("SwitchDisplay: swap %u lost on its %s path (%d): accepted work for its frame failed", swapOf(request),
              path, status);
    if (status != TEGRA_SCANOUT_SOURCE_GONE && status != TEGRA_SCANOUT_SOURCE_REFUSED)
        return false;
    if (status == TEGRA_SCANOUT_SOURCE_REFUSED)
        IOLog("SwitchDisplay: swap %u not shown (%s path): its frame's write was never accepted (%d)\n",
              swapOf(request), path, status);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    else
        IOLog("SwitchDisplay: swap %u not shown (%s path): its writer's client is gone (%d)\n", swapOf(request), path,
              status);
#endif
    return true;
}

// An accepted frame's end: 0 shows it, GONE and REFUSED retire it unshown (true). Any other failure is the display's or
// the kernel's own, which no frame may hide.
bool acceptedUnshown(int status, const OriginalLegacySwapRequest *request, const char *path) const
{
    if (!status)
        return false;
    if (!sourceUnshown(status, request, path))
        panic("SwitchDisplay: swap %u failed on its %s path after it was accepted (%d)", swapOf(request), path, status);
    return true;
}

// A failed wait's status as the producers it waited on tell it: the first that failed says why (a kill, a close, a
// refusal). None failed: status as it is, the fence's own failure.
static int producerFailure(int status, const switch_gpu_backing_token *producers, unsigned count)
{
    const int why = status ? switch_gpu_backing_tokens_failure(producers, count) : 0;
    return why ? why : status;
}
