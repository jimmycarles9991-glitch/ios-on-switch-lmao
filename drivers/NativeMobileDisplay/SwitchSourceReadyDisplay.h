// SPDX-License-Identifier: BSD-2-Clause-Patent
// A swap whose surface had a CPU or admission writer at its offer (kIOReturnCannotLock, deferSource): no thread sleeps
// for that writer. A GPU ready watch (switch_gpu_ready_observe) retries the surface's pitch snapshot whenever a writer
// leaves (every CPU release, admission end, publication and preparation end calls switch_gpu_admission_changed); the
// snapshot it takes holds new writers off, and its delivery re-kicks the swap on the display's work loop, whose offer
// then waits on the writer's producer token on the engine. Included in the class body after SwitchSourceWaitDisplay.h.
//
// The watch delivers exactly once (ready, failed, or -125 once cancelled), possibly after its cancel returns: the stop
// cancels it under the gate, then waits outside the gate for that delivery before the event source and sourceWait go.

IOInterruptEventSource *sourceReadySource = nullptr;
switch_gpu_ready_watch *sourceReady = nullptr;
bool sourceReadyArmed = false;
bool sourceReadyDelivered = false;
bool sourceReadyFallbackLogged = false;

void startSourceReady()
{
    IOWorkLoop *loop = getWorkLoop();
    sourceReadySource = IOInterruptEventSource::interruptEventSource(this, &NativeLegacyDisplay::sourceReadyFired);
    if (!loop || !sourceReadySource || loop->addEventSource(sourceReadySource) != kIOReturnSuccess)
        OSSafeReleaseNULL(sourceReadySource);
}

// Outside the gate, with copyStopping set.
void stopSourceReady()
{
    IOWorkLoop *loop = getWorkLoop();
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            switch_gpu_ready_cancel(&static_cast<NativeLegacyDisplay *>(owner)->sourceReady);
            return kIOReturnSuccess;
        },
        this);
    while (__atomic_load_n(&sourceReadyArmed, __ATOMIC_ACQUIRE)) {
        assert_wait(&sourceReadyArmed, THREAD_UNINT);
        if (__atomic_load_n(&sourceReadyArmed, __ATOMIC_ACQUIRE))
            thread_block(THREAD_CONTINUE_NULL);
        else
            clear_wait(current_thread(), THREAD_AWAKENED);
    }
    if (sourceReadySource) {
        loop->removeEventSource(sourceReadySource);
        OSSafeReleaseNULL(sourceReadySource);
    }
}

// deferSource's hand-off: a watch on the writer, or (none to be had) the copy thread's wait.
void watchSource()
{
    if (sourceReadySource) {
        __atomic_store_n(&sourceReadyArmed, true, __ATOMIC_RELEASE);
        const int error = switch_gpu_ready_observe(sourceReadyQuery, sourceReadyDone, this, &sourceReady);
        if (!error)
            return;
        __atomic_store_n(&sourceReadyArmed, false, __ATOMIC_RELEASE);
        if (!sourceReadyFallbackLogged) {
            sourceReadyFallbackLogged = true;
            IOLog("SwitchDisplay: no GPU ready watch for a busy surface (%d): the copy thread waits for its writer\n",
                  error);
        }
    }
    copySource->interruptOccurred(nullptr, nullptr, 0);
}

// On the ready watch's thread: 1 once the snapshot is held, 0 while a writer still has the surface, else its error.
static int sourceReadyQuery(void *owner)
{
    auto *self = static_cast<NativeLegacyDisplay *>(owner);
    if (__atomic_load_n(&self->copyStopping, __ATOMIC_ACQUIRE))
        return kIOReturnAborted;
    const int error = switch_native_scanout_pitch(self->sourceWait->surface, &self->sourceWait->read);
    if (!error)
        return 1;
    return error == -16 || error == SWITCH_GPU_BACKING_STALE ? 0 : error;
}

static void sourceReadyDone(void *owner, int status)
{
    auto *self = static_cast<NativeLegacyDisplay *>(owner);
    self->sourceWait->status = status;
    __atomic_store_n(&self->sourceReadyDelivered, true, __ATOMIC_RELEASE);
    self->sourceReadySource->interruptOccurred(nullptr, nullptr, 0);
    __atomic_store_n(&self->sourceReadyArmed, false, __ATOMIC_RELEASE);
    thread_wakeup(&self->sourceReadyArmed);
}

static void sourceReadyFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<NativeLegacyDisplay *>(owner);
    if (!__atomic_exchange_n(&self->sourceReadyDelivered, false, __ATOMIC_ACQUIRE))
        return;
    switch_gpu_ready_cancel(&self->sourceReady);
    self->sourceWaitFinished();
}
