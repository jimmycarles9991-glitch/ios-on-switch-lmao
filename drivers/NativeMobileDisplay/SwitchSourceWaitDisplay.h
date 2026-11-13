// SPDX-License-Identifier: BSD-2-Clause-Patent
struct SourceWait {
    const OriginalLegacySwapRequest *request;
    OSObject *surface;
    SwitchNativeScanout read;
    int status;
};

SourceWait *sourceWait = nullptr;
bool sourceWaitQueued = false;

void releaseSourceWait()
{
    if (!sourceWait || sourceWaitQueued)
        return;
    switch_native_scanout_release(&sourceWait->read);
    OSSafeReleaseNULL(sourceWait->surface);
    sourceWait->request = nullptr;
}

bool deferSource(const OriginalLegacySwapRequest *request, OSObject *surface)
{
    if (!copySource || !surface || copyStopping) {
        native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
        return true;
    }
    if (!sourceWait)
        sourceWait = IONewZero(SourceWait, 1);
    if (!sourceWait) {
        native_legacy_transfer_observed(kIOReturnNoMemory, request, 0);
        return true;
    }
    releaseSourceWait();
    sourceWait->request = request;
    sourceWait->surface = surface;
    surface->retain();
    sourceWaitQueued = transferred = true;
    watchSource();
    return true;
}

bool holdCopySource(OSObject *surface)
{
    if (!surface)
        return false;
    if (sourceWait && sourceWait->read.lease && sourceWait->surface == surface)
        return true;
    if (!sourceWait)
        sourceWait = IONewZero(SourceWait, 1);
    if (!sourceWait)
        return false;
    releaseSourceWait();
    sourceWait->surface = surface;
    surface->retain();
    return true;
}

// 0 once the producer's writes are in DRAM: external_wait answers 1 for that, 0 while it is still pending. A read taken
// behind a writer promise names no producer the CPU can wait for: it is taken again once the promised job is submitted.
int waitCopySource()
{
    if (sourceWait->read.lease && switch_gpu_backing_promised(sourceWait->read.view.native))
        switch_native_scanout_release(&sourceWait->read);
    const int error =
        sourceWait->read.lease ? 0 : switch_native_scanout_pitch_cpu(sourceWait->surface, &sourceWait->read);
    if (error)
        return error;
    const auto &producer = sourceWait->read.view.producer;
    if (!producer.engine)
        return 0;
    const int visible = switch_gpu_backing_token_external_wait(&producer);
    return visible == 1 ? 0 : visible < 0 ? visible : -11;
}

// The copy reads through a read-only cached mapping: DC IVAC needs write permission there and would drop a line
// another mapping dirtied, so the source's lines are cleaned and invalidated once its producer is visible.
int readyCopySource(const void *source, uint64_t bytes, uint64_t &gpu, uint64_t &cache)
{
    const uint64_t waitAt = stats ? mach_absolute_time() : 0;
    const int error = waitCopySource();
    if (error)
        return error;
    const uint64_t cacheAt = stats ? mach_absolute_time() : 0;
    switch_dcache_clean_invalidate(source, bytes);
    if (stats) {
        gpu = cacheAt - waitAt;
        cache = mach_absolute_time() - cacheAt;
    }
    return 0;
}

void sourceWaitFinished()
{
    sourceWaitQueued = false;
    transferred = false;
    if (!copyStopping && (latchRetire ? firstUntaken() : pendingSwapRequest()) == sourceWait->request) {
        if (!sourceWait->status) {
            kickSwapQueue();
        } else if (sourceWait->status == TEGRA_SCANOUT_SOURCE_LOST) {
            noteTaken(sourceWait->request, mach_absolute_time());
            planeNextSet = false;
            dropLostFrame(sourceWait->request, kNoBuffer);
        } else {
            native_legacy_transfer_observed(kIOReturnNotReady, sourceWait->request, 0);
        }
    }
    // A CPU fallback still reads this allocation after the retry leaves the family gate.
    if (!copyQueued)
        releaseSourceWait();
}

void sourceWaitWork()
{
    sourceWait->status = __atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)
                             ? kIOReturnAborted
                             : switch_native_scanout_pitch_wait(sourceWait->surface, &sourceWait->read);
    getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->sourceWaitFinished();
            return kIOReturnSuccess;
        },
        this);
}
