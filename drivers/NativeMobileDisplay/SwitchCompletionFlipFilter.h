// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's armed completion flip written from its producer's primary interrupt filter (SwitchCompletionFlipDisplay.h):
// the GPU's nonstall filter calls gpuFilterFlip for a watched present whose producer is complete and visible, host1x's
// syncpoint filter calls vicFilterFlip for a VIC copy's job. Both run with interrupts off and take no lock: each claims
// the flip of the arm it was set up for with one compare-and-swap. Included in the class body after
// SwitchCompletionFlipDisplay.h.

// A VIC copy's host1x filter waiter, one per arm parity: a waiter outlives its offer only by the work-loop pass that
// retires it, so the next arm never shares its slot.
struct VicFilterWaiter {
    void *display;
    uint32_t generation;
    switch_gpu_backing_token producer;
};

static inline VicFilterWaiter vicFilterWaiters[2] = {};

// The GPU's callout is registered at each arm with that arm's generation as its context: a filter that read an older
// registration claims only the older arm. (The GPU's filter reads the callout before its last check of the present it
// watches, and a newer registration comes after that present's detach.)
template <class Self> static void gpuFilterFlip(void *armed, int status)
{
    if (!status)
        static_cast<Self>(completionFlipOwner)
            ->writeEarlyCompletionFlip(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(armed)),
                                       SwitchCompletionFlip::kFromGpuFilter);
}

template <class Self> static void vicFilterFlip(void *ctx)
{
    const auto *waiter = static_cast<const VicFilterWaiter *>(ctx);
    if (switch_gpu_backing_token_filter_status(&waiter->producer) == 1)
        static_cast<Self>(waiter->display)
            ->writeEarlyCompletionFlip(waiter->generation, SwitchCompletionFlip::kFromVicFilter);
}

// The copy thread, the VIC job's fence armed: host1x's syncpoint filter writes the armed flip once the job is done.
// A job already done, or a waiter refused, leaves it to the fence's completion.
void armCompletionFlipWaiter(const switch_gpu_backing_token &producer)
{
    const uint32_t word = completionFlip.now();
    if (SwitchFlipWord::stateOf(word) != SwitchFlipWord::kArmed)
        return;
    const uint32_t generation = SwitchFlipWord::generationOf(word);
    auto &waiter = vicFilterWaiters[generation & 1];
    waiter = {this, generation, producer};
    switch_host1x_filter_handle handle = 0;
    const int waiting = switch_host1x_filter_wait(producer.queue, static_cast<uint32_t>(producer.value),
                                                  &vicFilterFlip<decltype(this)>, &waiter, &handle);
    if (waiting && SWITCH_FRAME_TIMELINE)
        noteFlipTotal(7, 1);
}

// The producer's primary interrupt filter (interrupts off): no lock, allocation or log.
void writeEarlyCompletionFlip(uint32_t armed, uint8_t by)
{
    if (SWITCH_FRAME_TIMELINE)
        noteFlipTotal(8, 1);
    if (gPanicRestore.panicking || !completionFlip.claimEarly(armed, by))
        return;
    writeCompletionFlipStores();
    [[maybe_unused]] const uint32_t start = completionFlip.start;
    completionFlip.claimed(SwitchCompletionFlip::kEarly);
    if (SWITCH_FRAME_TIMELINE) {
        noteFlipTotal(5, 1);
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FLIP), start);
    }
}

// gpuComplete's first step: a flip its producer's filter already wrote is on screen, so its completion is a success
// (the filter wrote only for a job certain to end so). A failure here breaks that: one error line; the next flip's filter
// runs as usual.
int completionFlipVerdict(int status)
{
    status = mediaFlipVerdict(status);
    if (!status || !completionFlip.early())
        return status;
    const int entry = completionFlip.entry;
    IOLog("SwitchDisplay: window A was flipped to entry %d from its producer's interrupt filter, whose completion then "
          "failed (%d): shown as done\n",
          entry, status);
    return 0;
}
