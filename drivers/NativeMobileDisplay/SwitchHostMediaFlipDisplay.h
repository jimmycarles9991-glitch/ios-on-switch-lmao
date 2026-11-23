// SPDX-License-Identifier: BSD-2-Clause-Patent
struct MediaHostOffer {
    void *display;
    uint32_t generation;
};

static inline SwitchHost1xDisplayStream mediaHostStream;
static inline MediaHostOffer mediaHostOffers[2] = {};
static inline uint32_t mediaHostGeneration = 0;  // the flip whose stream is queued; 0 once it completed

bool startMediaHostFlip()
{
    const int error = switch_host1x_display_start(&mediaHostStream);
    if (error)
        IOLog("SwitchDisplay: controller stream could not start (%d)\n", error);
    return !error;
}

// A stream that ran (its own status 0) wrote its set and request, whatever its producers' final status says by now: the
// set is the latch's, as an early write's is, and the gate reads why it failed. A stream the display cancelled itself
// (SC7 entry, its stop, an overlay recovery: switch_host1x_display_cancel's restore) completes kIOReturnAborted, as a
// stopped copy does; any other is its producers'. The generation is let go only after the claim: a gate writer that
// still finds it asks host1x, which says the stream wrote its set.
template <class Self> static void mediaHostCompleted(void *context, int status)
{
    const auto &offer = *static_cast<const MediaHostOffer *>(context);
    auto *display = static_cast<Self>(offer.display);
    if (!mediaHostStream.status && offer.generation == display->mediaFlip.generation() &&
        (!status || !display->mediaFlip.early()) && display->mediaFlip.claim(0)) {
        display->stampMediaHostFlip();
        display->mediaFlip.claimed(SwitchCompletionFlip::kWritten);
    }
    uint32_t queued = offer.generation;
    __atomic_compare_exchange_n(&mediaHostGeneration, &queued, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    native_legacy_gpu_complete(offer.display, status && mediaHostStream.restore ? kIOReturnAborted : status);
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
        IOLog("SwitchDisplay: host1x refused windows B and C's controller stream (%d)\n", error);
        return error;
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

// A gate writer of windows B and C stops a stream still queued. SC7 entry (the resume), the display's stop and an
// overlay recovery then write or turn off both windows before their own request, so a store the stream made before it
// stopped never reaches the screen.
void cancelMediaHostFlip()
{
    const uint32_t generation = mediaFlip.generation();
    if (!generation || __atomic_load_n(&mediaHostGeneration, __ATOMIC_ACQUIRE) != generation)
        return;
    if (!switch_host1x_display_cancel(&mediaHostStream))
        writeEarlyMediaFlip(generation);
}
