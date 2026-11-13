// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's overlay composite (SwitchPlaneComposite.h): included in the class body after
// SwitchPlaneConversionDisplay.h. planeDecide asks planeComposeWaits whether planeStage would have to drop overlays of
// the set it built (planeReserveBandwidth refuses it); if so window B gets one BGRA plane VIC composed from all of
// them. The job is submitted from a thread call, since VIC's submission may sleep for queue or command room and the
// gate must not; its completion kicks the queue, and the next decision maps the composite with VIC's producer token, so
// the set's present is armed on VIC's syncpoint (SwitchMediaPlaneFence.h). All but planeCompositeRun and
// stopPlaneComposite runs under the work-loop gate.

struct ComposedPlane {
    switch_gpu_backing *backing = nullptr;
    switch_display::CompositeKey key = {};
    bool valid = false;
};

static constexpr unsigned kComposedPlanes = 3;  // one scanned, one armed, one VIC writes

struct PlaneCompositeJob {
    SwitchNativeScanout sources[2];
    SwitchVICComposeSource compose[2];
    unsigned count;
    int sourceSlot[2], target, status;
    TegraVICImage layout;
    TegraVICRect area;
    uint64_t bytes;
    ComposedPlane value;
    SwitchNativeScanout output;
    TegraVICJob check;
};

// Allocated at the first set that may need it: the display's instance is at the 16 KiB a kalloc_type class may have.
struct PlaneComposite {
    ComposedPlane plane[kComposedPlanes];
    PlaneCompositeJob job;
    thread_call_t call;
    bool queued, refusedSet, failedSet;
    switch_display::CompositeDemand refused;
    switch_display::CompositeKey failed;
    const char *why;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    bool forced;
    uint32_t forcedCount;
#endif
};

PlaneComposite *planeComposite = nullptr;
bool planeCompositeStopping = false;

// The state and the thread call; false while either cannot be had.
bool planeCompositeReady()
{
    if (!planeComposite)
        planeComposite = IONewZero(PlaneComposite, 1);
    if (planeComposite && !planeComposite->call)
        planeComposite->call = thread_call_allocate_with_options(
            [](thread_call_param_t owner, thread_call_param_t) {
                auto *self = static_cast<NativeLegacyDisplay *>(owner);
                self->planeCompositeRun();
                self->release();
            },
            this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return planeComposite && planeComposite->call;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// HW Test display.planes_composite (SwitchPlaneCompositeProbe.h): armed, every overlay set is taken as refused, so the
// route runs on whatever Core Animation detaches; the report disarms and returns the frames composited meanwhile.
uint32_t planeCompositeProbe(bool arm)
{
    if (planeCompositeStopping || !planeCompositeReady())
        return 0;
    const uint32_t composited = planeComposite->forcedCount;
    planeComposite->forced = arm;
    planeComposite->forcedCount = 0;
    planeComposite->refusedSet = false;
    return composited;
}
#endif

bool planeCompositeUses(int slot) const
{
    return planeComposite && planeComposite->queued &&
           (planeComposite->job.sourceSlot[0] == slot || planeComposite->job.sourceSlot[1] == slot);
}

void planeCompositeRefuse(const char *why)
{
    if (why != planeComposite->why)
        IOLog("SwitchDisplay: overlays not composited: %s\n", why);
    planeComposite->why = why;
}

// Whether every window `set` shows is one of this route's composites (or off): a refusal next to such a set is the
// overlays' own, so it is remembered; one next to other overlays may pass once they retire.
bool planeCompositeShown(const PlaneSet &set) const
{
    for (const auto &overlay : set.window) {
        if (!overlay.on)
            continue;
        const auto *native = planeSlots[overlay.slot].native;
        bool composite = false;
        for (unsigned i = 0; native && i < kComposedPlanes; ++i)
            composite = composite ||
                        (planeComposite->plane[i].backing && native->view.native == planeComposite->plane[i].backing);
        if (!composite)
            return false;
    }
    return true;
}

// Whether planeStage arms the overlays as they are; reserving them here is what it would do a moment later.
bool planeAdmits(const switch_display::Overlay wanted[2])
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (planeComposite->forced)
        return false;
#endif
    const auto demand = switch_display::compositeDemand(wanted);
    if (planeComposite->refusedSet && switch_display::sameDemand(demand, planeComposite->refused))
        return false;
    const switch_display::WindowState windows[2] = {planeWindowState(switch_landscape::kB),
                                                    planeWindowState(switch_landscape::kC)};
    const auto assignment = switch_display::assign(wanted, windows);
    PlaneSet raw = {};
    for (unsigned index = 0; index != 2; ++index)
        raw.window[assignment.window[index]] = wanted[index];
    if (planeReserveBandwidth(raw))
        return true;
    if (planeCompositeShown(planeSetOf(planeLatch.active)) && planeCompositeShown(planeSetOf(planeLatch.armed)) &&
        planeCompositeShown(planeSetOf(planeLatch.superseded))) {
        planeComposite->refused = demand;
        planeComposite->refusedSet = true;
    }
    return false;
}

// A slot's source as VIC reads it: plane bases are offsets in the capture's allocation.
const char *planeCompositeSource(const SwitchNativeScanout &scanout, const native_planes::Layer &layer,
                                 const switch_display::CompositePart &part, SwitchVICComposeSource &out)
{
    out = {};
    out.scanout = &scanout;
    const auto &view = scanout.view;
    out.slot.srcX = part.srcX;
    out.slot.srcY = part.srcY;
    out.slot.srcW = part.srcW;
    out.slot.srcH = part.srcH;
    if (view.layout.plane_count) {
        if (const char *why = switch_native_vic_image(view, 0, &out.image))
            return why;
        const uint64_t right = uint64_t(view.crop.width) << 16, bottom = uint64_t(view.crop.height) << 16;
        if (part.srcX + part.srcW > right || part.srcY + part.srcH > bottom)
            return "composite source outside the native picture's crop";
        out.slot.srcX += uint64_t(view.crop.x) << 16;
        out.slot.srcY += uint64_t(view.crop.y) << 16;
    } else {
        TegraVICSurfaceDesc desc = {};
        const auto &s = layer.surface;
        desc.fourcc = layer.kind == native_planes::kKindBgra   ? TEGRA_VIC_FOURCC('B', 'G', 'R', 'A')
                      : layer.kind == native_planes::kKindRgba ? TEGRA_VIC_FOURCC('R', 'G', 'B', 'A')
                                                               : s.format;
        desc.width = s.width;
        desc.height = s.height;
        desc.planes = native_planes::isYuv(layer.kind) ? s.planes : 0;
        desc.length = s.descriptorBytes;
        desc.colorMatrix = s.colorMatrix;
        desc.transfer = s.transfer;
        desc.primaries = s.primaries;
        desc.chromaTop = s.chromaTop;
        desc.chromaBottom = s.chromaBottom;
        for (unsigned p = 0; p < (desc.planes ? desc.planes : 1u) && p < 2; ++p) {
            desc.planeOffset[p] = desc.planes ? s.offset[p] : 0;
            desc.planeBytesPerRow[p] = s.stride[p];
            desc.planeHeight[p] = s.planeHeight[p];
        }
        if (const char *why = tegra_vic_image_from(&desc, scanout.pitchOffset, &out.image))
            return why;
        const uint64_t bytes = uint64_t(scanout.allocation.page_count) << 12;
        for (unsigned p = 0; p < out.image.format->planes; ++p)
            out.image.span[p] = out.image.base[p] < bytes && out.image.span[p] > bytes - out.image.base[p]
                                    ? bytes - out.image.base[p]
                                    : out.image.span[p];
    }
    out.slot.blend = switch_display::compositeBlend(layer);
    out.slot.alpha10 = 1023;
    return nullptr;
}

// The composite of the overlays in wanted (wanted[1] lies under wanted[0]) mapped into a slot, with its image; -115
// while VIC's job is being submitted, -11 with every buffer scanned or a different composite on its way, -95 when VIC
// cannot compose these overlays (they go as planeStage leaves them).
int planeCompositeSlot(const switch_display::Overlay wanted[2], const int selected[2], uint32_t width, uint32_t height,
                       native_planes::Image &image)
{
    SwitchVICComposeSource compose[2];
    switch_display::CompositePart parts[2];
    switch_display::CompositeKey key = {};
    int sourceSlot[2] = {-1, -1};
    unsigned count = 0;
    for (unsigned index = 2; index-- > 0;) {
        if (!wanted[index].on)
            continue;
        const auto &layer = planeLayers[selected[index]].layer;
        const auto *native = planeSlots[wanted[index].slot].native;
        if (!native || !native->lease || !switch_display::compositePart(layer, width, height, parts[count]))
            return -95;
        if (const char *why = planeCompositeSource(*native, layer, parts[count], compose[count])) {
            planeCompositeRefuse(why);
            return -95;
        }
        key.source[count] = {native->allocation.allocation_id,
                             native->allocation.allocation_generation,
                             native->allocation.content_generation,
                             native->view.crop.x,
                             native->view.crop.y,
                             layer.surface.format,
                             layer.surface.colorMatrix,
                             layer.surface.chromaTop,
                             layer.surface.chromaBottom,
                             layer.src,
                             layer.dst};
        sourceSlot[count++] = wanted[index].slot;
    }
    const auto area = switch_display::compositeUnion(parts, count);
    const auto surface = switch_display::compositeSurface(area, tegra_vic_traits()->blockLinearOut != 0);
    key.count = count;
    key.target = area;
    if (planeComposite->failedSet && switch_display::sameComposite(planeComposite->failed, key))
        return -95;
    for (unsigned i = 0; i < count; ++i) {
        auto &slot = compose[i].slot;
        slot.dstX = parts[i].screen.x - area.x;
        slot.dstY = parts[i].screen.y - area.y;
        slot.dstW = parts[i].screen.w;
        slot.dstH = parts[i].screen.h;
    }
    const tgm_layout block = switch_display::compositeLayout(surface);
    if (!native_planes::imageOf(switch_display::compositeLayer(surface, area), width, height, image,
                                surface.block ? &block : nullptr))
        return -95;
    for (unsigned i = 0; i < kComposedPlanes; ++i) {
        auto &buffer = planeComposite->plane[i];
        if (!buffer.valid || !switch_display::sameComposite(buffer.key, key))
            continue;
        SwitchNativeScanout result = {};
        if (const int error = switch_gpu_backing_export_read(buffer.backing, &result.view.producer, &result.lease))
            return error;
        result.view.native = buffer.backing;
        switch_gpu_backing_retain(result.view.native);
        switch_gpu_backing_describe(result.view.native, &result.allocation);
        return planeMapSnapshot(result, image);
    }
    if (planeComposite->queued)
        return switch_display::sameComposite(planeComposite->job.value.key, key) ? -115 : -11;
    const TegraVICImage layout = switch_display::compositeImage(surface);
    const TegraVICRect target = {0, 0, area.w, area.h};
    if (const char *why = planeCompositeCheck(compose, count, layout, target)) {
        planeCompositeRefuse(why);
        return -95;
    }
    ComposedPlane *chosen = nullptr;
    for (unsigned i = 0; i < kComposedPlanes && !chosen; ++i)
        if (!planeComposite->plane[i].backing || !convertedBusy(planeComposite->plane[i].backing))
            chosen = &planeComposite->plane[i];
    if (!chosen)
        return -11;
    if (chosen->backing)
        convertedReleaseIdle(chosen->backing);
    chosen->valid = false;
    auto &work = planeComposite->job;
    work.count = count;
    work.target = int(chosen - planeComposite->plane);
    work.layout = layout;
    work.area = target;
    work.bytes = switch_display::compositeSurface({0, 0, width, height}, surface.block).bytes;
    work.value = *chosen;
    work.value.key = key;
    if (work.value.backing)
        switch_gpu_backing_retain(work.value.backing);
    for (unsigned i = 0; i < count; ++i) {
        work.sourceSlot[i] = sourceSlot[i];
        work.sources[i] = *planeSlots[sourceSlot[i]].native;
        switch_gpu_backing_retain(work.sources[i].view.native);
        if (work.sources[i].view.logical)
            switch_gpu_backing_retain(work.sources[i].view.logical);
        work.compose[i] = compose[i];
        work.compose[i].scanout = &work.sources[i];
    }
    planeComposite->queued = true;
    retain();
    if (thread_call_enter(planeComposite->call))
        release();
    return -115;
}

// The job tegra_vic_compose_build makes of these slots at stand-in addresses: whether VIC takes them at all.
const char *planeCompositeCheck(const SwitchVICComposeSource *compose, unsigned count, const TegraVICImage &layout,
                                const TegraVICRect &target)
{
    TegraVICComposeSlot slots[2];
    TegraVICImage images[2], out = layout;
    out.base[0] = 1u << 20;
    for (unsigned i = 0; i < count; ++i) {
        images[i] = compose[i].image;
        for (unsigned p = 0; p < images[i].format->planes; ++p)
            images[i].base[p] += 2u << 20;
        slots[i] = compose[i].slot;
        slots[i].source = &images[i];
    }
    return tegra_vic_compose_build(slots, count, &out, &target, tegra_vic_traits(), 0, 0, &planeComposite->job.check);
}

// Off the gate: VIC gets the job.
void planeCompositeRun()
{
    auto &work = planeComposite->job;
    work.status = kIOReturnAborted;
    if (!__atomic_load_n(&planeCompositeStopping, __ATOMIC_ACQUIRE)) {
        if (!work.value.backing)
            work.value.backing = switch_display_scanout_buffer(size_t(work.bytes));
        work.status = work.value.backing ? switch_vic_scanout_compose(work.compose, work.count, work.value.backing,
                                                                      work.layout, work.area, &work.output)
                                         : -12;
    }
    planeCompositeReleaseSources();
    getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->planeCompositeFinished();
            return kIOReturnSuccess;
        },
        this);
}

void planeCompositeReleaseSources()
{
    auto &work = planeComposite->job;
    for (unsigned i = 0; i < work.count; ++i) {
        if (work.sources[i].view.logical)
            switch_gpu_backing_release(work.sources[i].view.logical);
        switch_gpu_backing_release(work.sources[i].view.native);
        work.sources[i] = {};
    }
    work.count = 0;
}

void planeCompositeFinished()
{
    auto &work = planeComposite->job;
    auto &target = planeComposite->plane[work.target];
    if (target.backing)
        switch_gpu_backing_release(target.backing);
    target = work.value;
    work.value.backing = nullptr;
    target.valid = !work.status;
    switch_native_scanout_release(&work.output);
    planeComposite->queued = false;
    if (planeCompositeStopping)
        return;
    if (work.status) {
        planeComposite->failed = target.key;
        planeComposite->failedSet = true;
        planeCompositeRefuse("a composite job could not be submitted");
    } else {
        if (stats)
            ++stPlaneComposited;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
        if (planeComposite->forced)
            ++planeComposite->forcedCount;
#endif
    }
    kickSwapQueue();
}

// planeDecide's question: true when the swap waits for the composite of the overlays in wanted (its completion kicks
// again); otherwise wanted is what goes out, the composite if VIC took the overlays.
bool planeComposeWaits(switch_display::Overlay wanted[2], const int selected[2], uint32_t width, uint32_t height)
{
    if ((!wanted[0].on && !wanted[1].on) || !memoryAdmissionReady() || planeCompositeStopping || !tegra_vic_ready() ||
        !tegra_vic_traits() || !planeCompositeReady() || planeAdmits(wanted))
        return false;
    native_planes::Image image;
    const int slot = planeCompositeSlot(wanted, selected, width, height, image);
    if (planeRetry(slot))
        return true;
    if (slot < 0)
        return false;
    wanted[0] = {true, slot, image, 1};
    wanted[1] = {};
    planeBuildingSlots |= 1u << unsigned(slot);
    return false;
}

// Outside the gate (stop, free): no job is queued once this returns.
void stopPlaneComposite()
{
    if (!planeComposite)
        return;
    if (auto *workLoop = getWorkLoop())
        workLoop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                __atomic_store_n(&static_cast<NativeLegacyDisplay *>(owner)->planeCompositeStopping, true,
                                 __ATOMIC_RELEASE);
                return kIOReturnSuccess;
            },
            this);
    auto &state = *planeComposite;
    if (state.call) {
        if (thread_call_cancel_wait(state.call)) {
            planeCompositeReleaseSources();
            if (state.job.value.backing)
                switch_gpu_backing_release(state.job.value.backing);
            state.queued = false;
            release();
        }
        thread_call_free(state.call);
    }
    switch_native_scanout_release(&state.job.output);
    for (auto &plane : state.plane)
        if (plane.backing)
            switch_gpu_backing_release(plane.backing);
    IODelete(planeComposite, PlaneComposite, 1);
    planeComposite = nullptr;
}
