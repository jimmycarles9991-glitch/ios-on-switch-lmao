// SPDX-License-Identifier: BSD-2-Clause-Patent
struct MediaHostOffer {
    void *display;
    uint32_t generation;
};

static inline SwitchHost1xDisplayStream mediaHostStream;
static inline MediaHostOffer mediaHostOffers[2] = {};
static inline uint32_t mediaHostGeneration = 0, mediaHostCancelled = 0, mediaHostRestored = 0;
static inline uint64_t mediaHostHeldSlots = 0;

bool startMediaHostFlip()
{
    const int error = switch_host1x_display_start(&mediaHostStream);
    if (error)
        IOLog("SwitchDisplay: controller stream could not start (%d)\n", error);
    return !error;
}

template <class Self> static void mediaHostCompleted(void *context, int status)
{
    const auto &offer = *static_cast<const MediaHostOffer *>(context);
    auto *display = static_cast<Self>(offer.display);
    if (offer.generation == display->mediaFlip.generation()) {
        if (status) {
            if (!display->mediaFlip.early() && (mediaHostStream.restore || !mediaHostStream.status))
                __atomic_store_n(&mediaHostCancelled, offer.generation, __ATOMIC_RELEASE);
        } else if (display->mediaFlip.claim(0)) {
            display->stampMediaHostFlip();
            display->mediaFlip.claimed(SwitchCompletionFlip::kWritten);
        }
    }
    native_legacy_gpu_complete(offer.display, status);
}

template <class Self> static void mediaHostFiltered(void *context)
{
    const auto &offer = *static_cast<const MediaHostOffer *>(context);
    if (!__atomic_load_n(&mediaHostStream.status, __ATOMIC_ACQUIRE))
        static_cast<Self>(offer.display)->writeEarlyMediaFlip(offer.generation);
}

void stampMediaHostFlip()
{
    mediaFlip.requestedAt = __atomic_load_n(&frameEnds, __ATOMIC_RELAXED);
    mediaFlip.writtenAt = mach_absolute_time();
}

int submitMediaHostFlip(const switch_gpu_backing_token *producers, unsigned count)
{
    const uint32_t generation = mediaFlip.generation();
    auto &offer = mediaHostOffers[generation & 1];
    offer = {this, generation};
    __atomic_store_n(&mediaHostGeneration, generation, __ATOMIC_RELEASE);
    const int error = switch_host1x_display_submit(&mediaHostStream, producers, count, mediaFlip.word, mediaFlip.value,
                                                   mediaFlip.stores, mediaFlip.update, mediaFlip.request,
                                                   &mediaHostCompleted<decltype(this)>, &offer);
    if (error) {
        __atomic_store_n(&mediaHostGeneration, 0u, __ATOMIC_RELEASE);
        withdrawMediaFlip();
        return error == -22 || error == -16 ? SwitchHost1xDisplayStream::kCancelled : error;
    }
    if (mediaFlip.state() == SwitchCompletionFlip::kArmed) {
        switch_host1x_filter_handle handle = 0;
        const int waiting = switch_host1x_filter_wait(mediaHostStream.syncpt, mediaHostStream.fence.threshold,
                                                      &mediaHostFiltered<decltype(this)>, &offer, &handle);
        if (waiting && SWITCH_FRAME_TIMELINE)
            noteMediaFlipTotal(6, 1);
    }
    return 0;
}

void cancelMediaHostFlip()
{
    const uint32_t generation = mediaFlip.generation();
    if (!generation || __atomic_load_n(&mediaHostGeneration, __ATOMIC_ACQUIRE) != generation)
        return;
    const int status = switch_host1x_display_cancel(&mediaHostStream);
    if (status && mediaHostStream.restore)
        __atomic_store_n(&mediaHostCancelled, generation, __ATOMIC_RELEASE);
    else if (!status)
        writeEarlyMediaFlip(generation);
}

void restoreCancelledMediaHostFlip()
{
    const uint32_t generation = __atomic_load_n(&mediaHostCancelled, __ATOMIC_ACQUIRE);
    if (!generation || generation == mediaHostRestored || generation != mediaFlip.generation())
        return;
    mediaHostRestored = generation;
    const PlaneSet stopped = planeSetOf(planeLatch.armed);
    for (const auto &overlay : stopped.window)
        if (overlay.on)
            mediaHostHeldSlots |= uint64_t(1) << unsigned(overlay.slot);
    mediaFlip.withdraw();
    planeLatch.withdraw();
    const PlaneSet good = newestRequestedSet();
    uint32_t update = 0, request = 0;
    if (!planeStage(good, update, request))
        panic("SwitchDisplay: the last good overlay set could not be restored after its controller stream stopped");
    scanoutMemoryBarrier();
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    planeRequestedAt = frameEnds;
    armFlipDeadline();
}
