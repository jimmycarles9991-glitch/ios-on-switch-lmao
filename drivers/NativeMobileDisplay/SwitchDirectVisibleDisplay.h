// SPDX-License-Identifier: BSD-2-Clause-Patent
// Direct scanout with no present job when the frame is already in DRAM. A slot holds its surface's backing as scanned
// (switch_gpu_backing_scanout), so the GPU driver ends every job that writes it External: its completion then means its
// writes left the GPU's L2 (FRAMECPU C6.1). A swap whose slot's producer token is externally visible at the offer (or
// that no engine wrote) flips from the kick itself; one whose producer is still running keeps the fence-only present.
// A window B change going out with the frame keeps the present when it has writers of its own to wait for. A producer
// still running at the offer, past the slot's first frame, is watched instead: its own interrupt pass completes the
// fence-only offer with no present job (TEGRA_SCANOUT_COPY_PRODUCER_WATCH). A surface with a writer promise pending
// (SwitchGpuWriterPromise.h) never flips from the kick: the DC's flip is a register write with no GPU work in front of
// it to acquire the promise in. Past the slot's first frame it is watched: the watch waits for the promise's job to be
// submitted and then watches that job, whose interrupt pass writes the flip (SwitchGpuPresentWatch.h).
// Included in the class body after SwitchDirectSlots.h; under the gate.

switch_gpu_backing *scanoutHeld[kSlots] = {};
uint64_t scanoutHeldUse[kSlots] = {};
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool directVisibleLogged = false;
// directSlotFor's: what this swap's own snapshot found, also when the slot keeps an earlier frame's lease instead.
switch_gpu_scanout_look directLook = {};

// A slot's last frame shown (taken: its take sequence, 0 for none) and its surface's allocation and contents then.
struct DirectShown {
    uint64_t allocation, content, taken;
    uint32_t swap;
};

DirectShown directShown[kSlots] = {};
uint32_t directAgainFlips = 0;
#endif

void holdScanoutBacking(unsigned k)
{
    releaseScanoutBacking(k);
    auto *backing = slots[k].read ? slots[k].read->view.native : nullptr;
    if (!backing)
        return;
    switch_gpu_backing_retain(backing);
    switch_gpu_backing_scanout(backing, 1);
    scanoutHeld[k] = backing;
    scanoutHeldUse[k] = slots[k].used;
}

// The slot's first frame was written before its backing was held scanned, by a job that may have ended Internal: only
// the present job's flush puts it in DRAM.
uint32_t producerWatch(int entry) const
{
    const auto k = static_cast<unsigned>(entry - kSlot0);
    return scanoutHeld[k] && slots[k].used != scanoutHeldUse[k] ? TEGRA_SCANOUT_COPY_PRODUCER_WATCH : 0u;
}

void releaseScanoutBacking(unsigned k)
{
    if (!scanoutHeld[k])
        return;
    switch_gpu_backing_scanout(scanoutHeld[k], 0);
    switch_gpu_backing_release(scanoutHeld[k]);
    scanoutHeld[k] = nullptr;
}

bool directFrameVisible(int entry) const
{
    const auto *read = slots[entry - kSlot0].read;
    if (!read || !read->lease || planeNextAlsoWritten() || (planeNextSet && planeFenced(planeNext)))
        return false;
    const auto &producer = read->view.producer;
    return !switch_gpu_backing_promised(read->view.native) &&
           (!producer.engine || switch_gpu_backing_token_external_status(&producer) == 1);
}

// offerDirect's first step once the slot is mapped: true when the frame went to window A from here.
bool presentDirectVisible(const OriginalLegacySwapRequest *request, int entry, uint32_t width, uint32_t height,
                          uint64_t kickAt)
{
    if (!directFrameVisible(entry))
        return false;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    const int scanned = ring.front;
    const bool planeChange = planeNextSet;
#endif
    noteTaken(request, kickAt);
    gpuLength = static_cast<uint64_t>(width) * height * 4;
    flipRequest = request;
    presentEntry(entry);
    native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
    if (stats)
        ++stDirect;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (!directVisibleLogged) {
        directVisibleLogged = true;
        IOLog("SwitchDisplay: first direct frame flipped with no present job: its producer's writes were in DRAM\n");
    }
    logDirectFlip(entry, request, scanned, planeChange);
#endif
    return true;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// request, already taken, shows entry's surface as directLook found it: by this flip or by an accepted present.
void noteDirectShown(int entry, const OriginalLegacySwapRequest *request)
{
    directShown[entry - kSlot0] = {directLook.allocation, directLook.content, seqOf(request), swapOf(request)};
}

// The GPU driver's present lines (TegraGM20BUserClientPresent.cpp) for a frame that never reaches its present. scanned:
// the entry the UI window scanned before this flip. A pending writer promise is a write still to come, whatever the
// generation.
void logDirectFlip(int entry, const OriginalLegacySwapRequest *request, int scanned, bool planeChange)
{
    const switch_gpu_scanout_look &look = directLook;
    const DirectShown before = directShown[entry - kSlot0];
    noteDirectShown(entry, request);
    const DirectShown &now = directShown[entry - kSlot0];
    if (!before.taken || before.allocation != look.allocation || before.content != look.content || look.promised ||
        !switch_gpu_refusal::logged(directAgainFlips++))
        return;
    IOLog("SwitchDisplay: swap %u (taken %llu) flips from slot %d to surface %u (allocation %llu) in slot %d with no "
          "present job, again with nothing written since swap %u (taken %llu), %lld frames ago (generation %llu, "
          "writer engine %u queue %u job %llu); %s (%u so far)\n",
          now.swap, now.taken, scanned, look.surface, look.allocation, entry, before.swap, before.taken,
          static_cast<long long>(now.taken - before.taken), look.content, look.writer.engine, look.writer.queue,
          look.writer.value, planeChange ? "a window B change goes with it" : "nothing else changes with it",
          directAgainFlips);
}
#endif
