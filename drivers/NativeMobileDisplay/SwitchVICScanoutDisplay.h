// SPDX-License-Identifier: BSD-2-Clause-Patent
// The scanout copy on VIC, for a frame the GPU's copy engine declines or is bypassed for (TegraScanoutCopy.h): VIC
// writes the swapped surface into the buffer window A neither scans nor may take (ring.target()) in window A's own
// layout, and the flip is armed on VIC's syncpoint (switch_host1x_token_fence), so the interrupt that ends the job
// completes it through native_legacy_gpu_complete as a GPU copy's completion does. A pitch source (the compositor's
// rows, 256-byte aligned by DisplayPipeStrideRequirements) is placed block linear (OutBlkKind GENERIC_16Bx2, window A's
// 16-GOB blocks); a block-linear source already in window A's blocks is copied byte for byte, read and written as
// 4096-byte rows. VIC reads source rows only on multiples of its sourcePitchAlign (gpu.x1.vic_pitch): another stride
// is declined. The CPU copies a frame only while VIC is not up (the emulator, whose VIC never starts); with VIC up, a
// frame no engine took is not shown and its swap retires. Included in the class body: the kick and the completion
// run under the family's work-loop gate, the submission on the copy thread, since switch_vic_submit_checked sleeps
// while VIC's queue is full.

struct VicScanoutJob {
    OSObject *surface;  // retained until the completion
    SwitchNativeScanout source, output;
    TegraVICImage layout;
    SwitchVICPlaceReads also;  // retained backings
    int target;
    bool tiled;
    uint64_t stride;
    switch_gpu_backing *targets[kScanBuffers];  // the scan surfaces as backings VIC maps
};

VicScanoutJob *vicJob = nullptr;  // allocated at the start: the display class has a size limit
bool vicCopyQueued = false, vicScanout = false;
unsigned vicDeadlines = 0;

#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kScanoutRoutes = 4;
static inline uint32_t scanoutRoutes[kScanoutRoutes] = {};  // VIC copies, CPU copies, frames not shown, engines
static inline struct sysctl_oid scanoutRouteOids[kScanoutRoutes] = {};

static int scanoutRouteSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const unsigned i = static_cast<unsigned>(which) % kScanoutRoutes;
    int value = static_cast<int>(__atomic_load_n(&scanoutRoutes[i], __ATOMIC_RELAXED));
    if (i == 3)
        value = (tegra_vic_ready() && tegra_vic_traits()) ? 1 : 0;
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void scanoutRoutesRegister()
{
    static const char *const names[kScanoutRoutes] = {"switch_scanout_vic", "switch_scanout_cpu",
                                                      "switch_scanout_unshown", "switch_scanout_vic_up"};
    for (unsigned i = 0; i < kScanoutRoutes; ++i) {
        if (scanoutRouteOids[i].oid_name)
            continue;
        switch_sysctl_oid(&scanoutRouteOids[i], &sysctl__kern_children, names[i],
                          CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, scanoutRouteSysctl, "I",
                          "scanout frames since boot: copied by VIC, copied by the CPU, not shown; VIC up now");
        scanoutRouteOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&scanoutRouteOids[i]);
    }
}

static void noteScanoutRoute(unsigned route)
{
    __atomic_add_fetch(&scanoutRoutes[route], 1, __ATOMIC_RELAXED);
}
#else
static void scanoutRoutesRegister()
{}

static void noteScanoutRoute(unsigned)
{}
#endif

// VIC's route: VIC up, and the scanout multi-buffered with a completion deadline and a copy thread to submit from.
bool vicRouteUp() const
{
    return tegra_vic_ready() && tegra_vic_traits() && hwRotate && doubleBuffer && gpuDeadline && copySource && vicJob;
}

void startVicCopy()
{
    if (!primary || !hwRotate)
        return;
    if (!vicJob)
        vicJob = IONewZero(VicScanoutJob, 1);
    scanoutRoutesRegister();
    IOLog("SwitchDisplay: scanout frames are copied on the GPU's copy engine%s, then on VIC into window A's blocks "
          "(armed on its syncpoint); the CPU copies one only while VIC is not up (the emulator)\n",
          gpuCopy ? "" : " (off)");
}

// The scan surface as a backing VIC maps: the display's own pages, never freed while the display runs.
switch_gpu_backing *vicTarget(int target)
{
    if (target < 0 || target >= int(kScanBuffers))
        return nullptr;
    if (!vicJob->targets[target]) {
        constexpr uint32_t pages = uint32_t(kNativeBlockBytes >> 12);
        uint64_t *list = IONew(uint64_t, pages);
        if (!list)
            return nullptr;
        for (uint32_t i = 0; i < pages; ++i)
            list[i] = uint64_t(surfaceAddress(target)) + (uint64_t(i) << 12);
        vicJob->targets[target] = switch_gpu_backing_register(list, pages, 0, nullptr, nullptr);
        IODelete(list, uint64_t, pages);
    }
    return vicJob->targets[target];
}

// The window B surfaces this flip also shows: the job orders after their producers.
SwitchVICPlaceReads vicAlsoReads() const
{
    SwitchVICPlaceReads also = {};
    if (!planeNextSet)
        return also;
    for (const auto &overlay : planeNext.window) {
        const auto *native = overlay.on ? planeSlots[overlay.slot].native : nullptr;
        if (native && native->lease && native->view.producer.engine && also.count < kSwitchVICPlaceReads) {
            switch_gpu_backing_retain(native->view.native);
            also.backings[also.count++] = native->view.native;
        }
    }
    return also;
}

// Under the gate, in the kick, the GPU's copy engine having declined: the frame goes to VIC. True: taken (or, every
// surface held, deferred to the frame end that frees one); false: VIC does not take this frame.
bool offerVicCopy(const OriginalLegacySwapRequest *request, OSObject *surface, uint64_t stride, uint64_t allocation,
                  uint32_t width, uint32_t height, int blockLog2, uint64_t kickAt)
{
    const TegraVICTraits *traits = vicRouteUp() ? tegra_vic_traits() : nullptr;
    if (!traits || copyStopping || vicCopyQueued || width != kNativeBlockWidth || height != kNativeBlockHeight)
        return false;
    const bool tiled = blockLog2 >= 0;
    if (tiled && blockLog2 != int(kScanBlockLog2))
        return false;
    if (!tiled && (!traits->blockLinearOut || stride % traits->sourcePitchAlign || allocation < stride * height)) {
        switch_refusal("display-scanout", "vic-stride",
                       "SwitchDisplay: VIC does not copy a %llu-byte-row frame into the scanout: it reads rows on "
                       "multiples of %u bytes",
                       stride, traits->sourcePitchAlign);
        return false;
    }
    const int target = ring.target();
    if (target == kNoBuffer) {
        kickDeferred = true;
        return true;
    }
    if (!vicTarget(target))
        return false;
    auto &job = *vicJob;
    job.surface = surface;
    surface->retain();
    job.target = target;
    job.tiled = tiled;
    job.stride = stride;
    job.layout = vicLayout(tiled);
    job.also = vicAlsoReads();
    ring.claim(target);
    gpuSequence += 1;
    gpuOfferedAt = mach_absolute_time();
    gpuRequest = request;
    gpuLength = uint64_t(width) * height * 4;
    __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
    vicScanout = true;
    vicDeadlines = 0;
    gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
    native_legacy_swap_stage(1);
    transferred = true;
    noteTaken(request, kickAt);
    if (!latchRetire && retireSwap(request, true)) {
        gpuRetired = true;
        native_legacy_early_retires = native_legacy_early_retires + 1;
    }
    armCompletionFlip(target);
    vicCopyQueued = true;
    copySource->interruptOccurred(nullptr, nullptr, 0);
    return true;
}

// Window A's layout (NativeScanout.h), or, for a source already in it, the same bytes as 4096-byte rows of BGRA.
static TegraVICImage vicLayout(bool tiled)
{
    TegraVICImage layout = {};
    layout.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    layout.span[0] = kNativeBlockBytes;
    if (tiled) {
        layout.width = 1024;
        layout.height = layout.rows[0] = uint32_t(kNativeBlockBytes / 4096);
        layout.pitch[0] = 4096;
        return layout;
    }
    layout.width = kNativeBlockWidth;
    layout.height = kNativeBlockHeight;
    layout.pitch[0] = uint32_t(kNativeBlockStride);
    layout.rows[0] = uint32_t(kNativeBlockBytes / kNativeBlockStride);
    layout.blockLinear = 1;
    layout.blockHeightLog2 = kScanBlockLog2;
    return layout;
}

// The source as VIC reads it: the surface's rows, or its block-linear bytes as the layout's rows.
TegraVICImage vicSource(const VicScanoutJob &job) const
{
    TegraVICImage image = job.layout;
    image.blockLinear = image.blockHeightLog2 = 0;
    image.base[0] = job.source.pitchOffset;
    if (!job.tiled) {
        image.pitch[0] = uint32_t(job.stride);
        image.rows[0] = image.height;
        image.span[0] = job.stride * image.height;
    }
    return image;
}

// The snapshot VIC reads is the bytes the swap's descriptor holds (the ones the CPU copy would read).
static bool vicSourceIsDescriptor(const VicScanoutJob &job)
{
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(job.surface);
    const uint64_t at = job.source.pitchOffset;
    IOByteCount length = 0;
    const uint64_t pa = memory ? memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone) : 0;
    const auto &pages = job.source.allocation;
    return pa && pages.pages && (at >> 12) < pages.page_count && pages.pages[at >> 12] + (at & 4095) == pa;
}

// The copy thread: the source snapshot (sleeping only for a CPU writer), the job, its fence. True: it was VIC's turn.
bool vicCopyWork()
{
    if (!__atomic_exchange_n(&vicCopyQueued, false, __ATOMIC_ACQ_REL))
        return false;
    auto &job = *vicJob;
    int error = __atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)
                    ? kIOReturnAborted
                    : switch_native_scanout_pitch_wait(job.surface, &job.source);
    if (!error && !vicSourceIsDescriptor(job)) {
        switch_refusal("display-scanout", "vic-snapshot",
                       "SwitchDisplay: the frame VIC would read is not the swap's own pages: it is not copied");
        error = -22;
    }
    if (!error) {
        const TegraVICImage image = vicSource(job);
        TegraVICOp operation = {};
        operation.srcW = uint64_t(image.width) << 16;
        operation.srcH = uint64_t(image.height) << 16;
        operation.dstW = job.layout.width;
        operation.dstH = job.layout.height;
        operation.nearest = 1;
        error = switch_vic_scanout_place(&job.source, &image, job.targets[job.target], &job.layout, &operation,
                                         job.also, &job.output);
    }
    if (!error)
        error = switch_host1x_token_fence(&job.output.view.producer, 1, native_legacy_gpu_complete, this);
    if (error)
        native_legacy_gpu_complete(this, error);
    else
        armCompletionFlipWaiter(job.output.view.producer);
    return true;
}

void releaseVicJob()
{
    auto &job = *vicJob;
    switch_native_scanout_release(&job.source);
    switch_native_scanout_release(&job.output);
    for (uint32_t i = 0; i < job.also.count; ++i)
        switch_gpu_backing_release(job.also.backings[i]);
    OSSafeReleaseNULL(job.surface);
    job.also = {};
}

// Under the gate: VIC's job ended (status 0: every byte of the target is in DRAM). Done, window A flips to it, as after
// a GPU copy; failed, the target is never flipped to and the frame is not shown. True: it was VIC's.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
bool finishVicCopy(int status)
{
    if (!vicScanout)
        return false;
    vicScanout = false;
    const OriginalLegacySwapRequest *request = gpuRequest;
    gpuDeadline->cancelTimeout();
    __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
    gpuRequest = nullptr;
    releaseVicJob();
    if (status && __atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        ring.release();
        transferred = false;
        return true;
    }
    if (status) {
        vicNotShown(request, status);
        return true;
    }
    noteScanoutRoute(0);
    gpuPresented = request;
    gpuPresentedEntry = kNoBuffer;
    if (hwSuspended) {
        gpuPresentDeferred = true;
        return true;
    }
    presentGpuCopy();
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void vicNotShown(const OriginalLegacySwapRequest *request, int status)
{
    if (status == SWITCH_GPU_BACKING_DEPENDENCY_FAILED || status == TEGRA_SCANOUT_SOURCE_LOST) {
        dropLostFrame(request, kNoBuffer);
        return;
    }
    switch_refusal("display-scanout", "vic-failed",
                   "SwitchDisplay: VIC did not copy frame %llu into the scanout (%d): it is not shown", gpuSequence,
                   status);
    noteScanoutRoute(2);
    ring.release();
    releaseIdleDirectReads();
    if (latchRetire && request)
        retireUnshown(seqOf(request));
    else if (!latchRetire && !gpuRetired && request && pendingSwapRequest() == request)
        retireSwap(request, true);
    gpuRetired = false;
    transferred = false;
    kickSwapQueue();
}

// The deadline of a job VIC holds: its completion is a syncpoint interrupt, after the GPU work its source waits for
// (the GPU driver ends a hung client's within its own deadline). One that never comes is a lost interrupt.
bool vicCopyLate()
{
    if (!vicScanout)
        return false;
    if (__atomic_load_n(&gpuDonePending, __ATOMIC_SEQ_CST) || ++vicDeadlines < kGpuCopyDeadlines) {
        gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
        return true;
    }
    panic("SwitchDisplay: VIC scanout copy %llu into %#x not completed in %u ms: no VIC syncpoint interrupt",
          gpuSequence, surfaceAddress(vicJob->target), vicDeadlines * kGpuCopyDeadlineMS);
}

// Under the gate, in the kick, before the CPU copy: with VIC's route up, a frame neither engine took is not shown (its swap
// retires as one the console holds back); without it the CPU copies the frame. True: the kick is done with it.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
bool cpuCopyRefused(const OriginalLegacySwapRequest *request, uint64_t kickAt)
{
    if (!vicRouteUp()) {
        noteScanoutRoute(1);
        return false;
    }
    switch_refusal("display-scanout", "no-engine",
                   "SwitchDisplay: a frame neither the GPU's copy engine nor VIC took is not shown: the CPU copies "
                   "only while VIC is not up");
    noteScanoutRoute(2);
    noteTaken(request, kickAt);
    retireUnshown(seqOf(request));
    if (!latchRetire) {
        transferred = true;
        finishTransfer(request, true);
        return true;
    }
    rekick = true;
    return true;
}

// From stopCopyLoop, under the gate: a job the copy thread never took goes, as if never offered; the targets go with
// no job out.
void stopVicCopy()
{
    if (__atomic_exchange_n(&vicCopyQueued, false, __ATOMIC_ACQ_REL)) {
        releaseVicJob();
        ring.release();
        gpuDeadline->cancelTimeout();
        __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
        gpuRequest = nullptr;
        vicScanout = transferred = false;
    }
    if (vicScanout || !vicJob)
        return;
    for (auto *target : vicJob->targets)
        if (target)
            switch_gpu_backing_release(target);
    IODelete(vicJob, VicScanoutJob, 1);
    vicJob = nullptr;
}
