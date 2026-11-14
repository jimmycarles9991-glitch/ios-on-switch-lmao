// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU client's completion of an offer and its work-loop pass. A completion that wrote window A's flip for a frame end
// to come signals nothing: that frame end's action runs the pass first (takeCompletionFlip), so the swap retires at the
// latch it always did. A kick waiting behind the offer, a stopped or suspended scanout, or retirement at the offer (no
// latch to wait for) want the pass now: the gate publishes that (completionPassFolds false) and the completion signals.
// The gate stores that word and then reads the flip's, the completion publishes the flip's and then reads that word
// (both sequentially consistent), so a pass the gate wants is signalled by one of them. gpuDonePending has one writer
// per offer (the completion) and one reader (the gate's pass), and the next offer comes only after that pass. Included
// in the class body (and in the host harness).

// Static, as the frame end's words: one display offers to the GPU.
static inline bool completionPassFolds = false;  // the gate's: a written flip's pass may wait for the frame end
static inline bool kickWaiting = false;          // the gate's: a swap is queued behind the offer out

void publishCompletionPass()
{
    const bool folds = latchRetire && !kickWaiting && !scanoutStopped && !hwSuspended;
    if (folds == completionPassFolds)
        return;
    __atomic_store_n(&completionPassFolds, folds, __ATOMIC_SEQ_CST);
    if (!folds && gpuDoneSource &&
        SwitchFlipWord::stateOf(__atomic_load_n(&completionFlip.bits, __ATOMIC_SEQ_CST)) == SwitchFlipWord::kWritten)
        gpuDoneSource->interruptOccurred(nullptr, nullptr, 0);
}

// The kick's first step: a swap waiting behind the offer out.
void kickCompletionPass(bool swapWaiting)
{
    kickWaiting = transferred && swapWaiting;
    publishCompletionPass();
}

// Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop. The GPU frame copy's
// completion (any context, even the GPU's interrupt filter) records its status, writes a direct frame's armed window A flip
// (SwitchCompletionFlipDisplay.h) and signals the work loop.
void gpuComplete(int status)
{
    status = completionFlipVerdict(status);
    if (SWITCH_FRAME_TIMELINE) {
        __atomic_store_n(&gpuDoneAt, mach_absolute_time(), __ATOMIC_RELAXED);
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_PRESENT_DONE), gpuSequence, status);
    }
    if (gpuDoneSource) {
        __atomic_store_n(&gpuStatus, status, __ATOMIC_RELAXED);
        __atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);
        writeCompletionFlip(status);
        if (!completionFlipCarries() || !__atomic_load_n(&completionPassFolds, __ATOMIC_SEQ_CST))
            gpuDoneSource->interruptOccurred(nullptr, nullptr, 0);
        return;
    }

    using Self = decltype(this);

    struct Args {
        Self display;
        int status;
    } args = {this, status};

    IOWorkLoop *loop = getWorkLoop();
    const auto action = [](OSObject *, void *a0, void *, void *, void *) -> IOReturn {
        const Args *a = static_cast<Args *>(a0);
        a->display->finishGpuSwap(a->status);
        return kIOReturnSuccess;
    };
    if (loop) {
        loop->runAction(action, this, &args);
    } else {
        finishGpuSwap(status);
    }
}

template <class Self> static void gpuDoneFired(OSObject *owner, IOInterruptEventSource *, int)
{
    Self self = static_cast<Self>(owner);
    self->notePass();
    if (!__atomic_load_n(&self->gpuDonePending, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&self->gpuDonePending, false, __ATOMIC_RELAXED);
    self->finishGpuSwap(__atomic_load_n(&self->gpuStatus, __ATOMIC_RELAXED));
}
