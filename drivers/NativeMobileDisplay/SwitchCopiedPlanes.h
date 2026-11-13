// SPDX-License-Identifier: BSD-2-Clause-Patent
bool copyPlaneJoined = false, copyPlaneReady = false, copyPixelsReady = false;
int copyPlaneStatus = 0;

bool beginCopiedPlaneFence()
{
    copyPlaneJoined = copyPlaneReady = copyPixelsReady = false;
    copyPlaneStatus = 0;
    if (!planeNextSet || !planeFenced(planeNext))
        return true;
    PlaneSet set = planeNext;
    set.seq = seqOf(job.request);
    copyPlaneJoined = true;
    if (planeFence(set))
        return true;
    copyPlaneJoined = false;
    return false;
}

void completeCopiedPlanes()
{
    if (!copyPixelsReady || !copyPlaneReady)
        return;
    copyPlaneJoined = copyPlaneReady = copyPixelsReady = false;
    if (copyPlaneStatus || job.status != kIOReturnSuccess) {
        if (copyPlaneStatus)
            job.status = kIOReturnAborted;
        planeNextSet = false;
        planeTarget = planeHeldSet ? planeHeld : planeSetOf(planeLatch.active);
    }
    completeCopiedJob();
}

void copiedPlaneFenceFinished(int status)
{
    copyPlaneStatus = status;
    copyPlaneReady = true;
    completeCopiedPlanes();
}

bool copiedPixelsFinished()
{
    if (!copyPlaneJoined)
        return false;
    copyPixelsReady = true;
    completeCopiedPlanes();
    return true;
}
