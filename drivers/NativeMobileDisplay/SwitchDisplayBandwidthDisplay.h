// SPDX-License-Identifier: BSD-2-Clause-Patent
switch_display::Reservation displayBandwidth;
switch_display::AdmissionEnvelope planeAdmission;
switch_display::MemoryEnvelope displayMemory;

switch_display::Demand planeDemand(const native_planes::Image &image)
{
    switch_display::Demand demand = {image.position & 0xffff, image.position >> 16, image.size & 0xffff,
                                     image.size >> 16,
                                     uint64_t(displayMemory.pixelClock()) * native_planes::fetchBytesPerPixel(image)};
    const uint32_t sourceWidth = (image.prescaled & 0xffff) / native_planes::prescaledBytesPerPixel(image);
    demand.bytesPerSecond = switch_display::scaled(demand.bytesPerSecond, sourceWidth, demand.width);
    demand.bytesPerSecond = switch_display::scaled(demand.bytesPerSecond, image.prescaled >> 16, demand.height);
    return demand;
}

bool planeMemory(const native_planes::Image &image, switch_display_memory_window &out, bool conversion)
{
    out = {};
    if (!image.visible)
        return true;
    const auto demand = planeDemand(image);
    // Under SCAN_COLUMN PRESCALED_SIZE holds the source's height (H, bytes) and width (V): L4T's memory window takes
    // the source rectangle as it lies in memory (t21x/bandwidth.c calc_disp_params).
    const bool rotated = (image.options & switch_landscape::kScanColumn) != 0;
    const uint32_t lineSamples = (image.prescaled & 0xffff) / (image.yuv ? 1 : 4), lines = image.prescaled >> 16;
    if (!switch_display::memoryWindow(out, demand.bytesPerSecond, rotated ? lines : lineSamples,
                                      rotated ? lineSamples : lines, demand.width, demand.height, image.yuv,
                                      !image.surfaceKind, rotated))
        return false;
    if (conversion) {
        switch_display_memory_window rgb = {};
        const uint64_t bytes =
            image.yuv ? switch_display::add(demand.bytesPerSecond, demand.bytesPerSecond) : demand.bytesPerSecond;
        if (!switch_display::memoryWindow(rgb, bytes, demand.width, demand.height, demand.width, demand.height, false,
                                          true, false))
            return false;
        switch_display::mergeMemoryWindow(out, rgb);
    }
    return true;
}

static int bandwidthRecorded(uint64_t)
{
    return 0;
}

uint64_t planeBandwidth(const PlaneSet &p)
{
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    switch_display::Demand demand[3] = {{0, 0, width, height, uint64_t(displayMemory.pixelClock()) * 4}, {}, {}};
    for (unsigned window = 0; window != 2; ++window)
        if (p.window[window].on)
            demand[window + 1] = planeDemand(p.window[window].image);
    return switch_display::peak(demand, 3);
}

uint64_t planeAdmissionDemand(const native_planes::Image &image)
{
    const uint64_t bytes = planeDemand(image).bytesPerSecond;
    return image.yuv ? switch_display::add(bytes, bytes) : bytes;
}

// Window A's first latch, and its first again after SC7: the MC plan for window A alone, so the boot chain's latency
// allowance and PTSA do not stay until the first overlay. An overlay's own admission, arming or retirement owns the plan.
void applyUiMemoryPlan()
{
    if (!displayMemory.ready() || displayMemory.holding() || !planeWindowIdle())
        return;
    int error = 0;
    const auto reserve = switch_emc_display_memory_reserve;
    if (displayMemory.applyUiAlone([&error, reserve](const switch_display_memory_head *head) {
            error = reserve(head);
            return error;
        })) {
        IOLog("SwitchDisplay: memory plan for window A alone applied (%llu bytes/s)\n",
              static_cast<unsigned long long>(displayMemory.reservation().peak_bytes_per_second));
        return;
    }
    IOLog("SwitchDisplay: memory plan for window A alone refused (%d): its latency allowance and PTSA stay as the boot "
          "chain left them until an overlay's plan\n",
          error);
}

bool memoryAdmissionReady() const
{
    return displayMemory.ready() && switch_emc_display_memory_ready();
}

// Never refuses a swap (a refused swap comes back every frame): window A alone scans as the firmware set it up and needs
// no admission, and a detached layer whose admission fails here is dropped when its overlay set is armed (planeStage).
void admitPlaneBandwidth(const bool present[native_planes::kLayers], const native_planes::Layer *layers, uint32_t width,
                         uint32_t height)
{
    bool overlay = false;
    for (unsigned layer = 0; layer != native_planes::kLayers; ++layer)
        overlay = overlay || (present[layer] && !native_planes::isUi(layers[layer], width, height));
    if (overlay && memoryAdmissionReady())
        (void)validatePlaneBandwidth(present, layers, width, height);
}

bool validatePlaneBandwidth(const bool present[native_planes::kLayers], const native_planes::Layer *layers,
                            uint32_t width, uint32_t height)
{
    uint64_t updated[4] = {}, active[2] = {};
    switch_display_memory_window memoryUpdated[4] = {}, memoryActive[2] = {};
    for (unsigned layer = 0; layer != native_planes::kLayers; ++layer) {
        if (!present[layer])
            continue;
        native_planes::Image image;
        if (!native_planes::imageOf(layers[layer], width, height, image))
            return false;
        if (image.visible) {
            updated[layer] = planeAdmissionDemand(image);
            if (!planeMemory(image, memoryUpdated[layer], true))
                return false;
        }
    }
    const int states[] = {planeLatch.active, planeLatch.armed, planeLatch.superseded};
    for (int state : states) {
        if (state < 0)
            continue;
        const auto &set = planeSetOf(state);
        for (unsigned window = 0; window != 2; ++window)
            if (set.window[window].on) {
                switch_display_memory_window memory = {};
                if (!planeMemory(set.window[window].image, memory, true))
                    return false;
                switch_display::mergeMemoryWindow(memoryActive[window], memory);
                const uint64_t bytes = planeAdmissionDemand(set.window[window].image);
                if (bytes > active[window])
                    active[window] = bytes;
            }
    }
    // Updated masks omit inherited layers. Reserve any two accepted streams, including a later RGB conversion,
    // until the complete accepted queue retires; bank reassignment cannot introduce a new demand at arm time.
    return planeAdmission.admit(updated, active, uint64_t(displayMemory.pixelClock()) * 4, [&](uint64_t bytes) {
        return displayMemory.admit(memoryUpdated, memoryActive, bytes, switch_emc_display_memory_reserve) &&
               displayBandwidth.raise(bytes, bandwidthRecorded);
    });
}

// The memory windows A, B and C need while `set` is scanned.
bool planeWindowsMemory(const PlaneSet &set, switch_display_memory_window memory[3])
{
    memory[0] = memory[1] = memory[2] = {};
    memory[switch_landscape::windowsOf(set.landscape).ui] = displayMemory.uiWindow();
    for (unsigned index = 0; index != 2; ++index)
        if (set.window[index].on && !planeMemory(set.window[index].image, memory[planeWindowOf(set, index)], false))
            return false;
    return true;
}

bool planeReserveBandwidth(const PlaneSet &p)
{
    if (!p.on() && p.landscape == landscapeOn())
        return true;
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    const switch_display::Demand ui = {0, 0, width, height, uint64_t(displayMemory.pixelClock()) * 4};
    switch_display::Demand windows[2][4] = {};
    switch_display_memory_window memory[3] = {};
    unsigned count = 0;
    const int ids[] = {planeLatch.active, planeLatch.armed, planeLatch.superseded};
    const PlaneSet *states[] = {&p, &planeSetOf(ids[0]), &planeSetOf(ids[1]), &planeSetOf(ids[2])};
    for (const auto *state : states) {
        for (unsigned window = 0; window != 2; ++window)
            if (state->window[window].on)
                windows[window][count] = planeDemand(state->window[window].image);
        if (count == 0 || count == 1 || ids[count - 1] >= 0) {
            switch_display_memory_window candidate[3];
            if (!planeWindowsMemory(*state, candidate))
                return false;
            for (unsigned w = 0; w != 3; ++w)
                switch_display::mergeMemoryWindow(memory[w], candidate[w]);
        }
        ++count;
    }
    const uint64_t bytes = switch_display::transitionPeak(ui, windows[0], count, windows[1], count);
    return displayMemory.armWindows(memory, bytes, switch_emc_display_memory_reserve) &&
           displayBandwidth.raise(bytes, bandwidthRecorded);
}

void planeRetireBandwidth()
{
    if (planeRecovering || planeLatch.requested() || planeHeldSet || gpuScanout || firstUntaken())
        return;
    for (bool pending : entryPlaneSet)
        if (pending)
            return;
    planeRetireBandwidthTo();
}

// Down to what the latched windows B and C scan now.
void planeRetireBandwidthTo()
{
    const auto &active = planeSetOf(planeLatch.active);
    switch_display_memory_window memory[3] = {};
    if (!planeWindowsMemory(active, memory))
        return;
    const uint64_t bytes = planeBandwidth(active);
    if (displayMemory.retireWindows(memory, bytes, switch_emc_display_memory_reserve) &&
        displayBandwidth.retire(bytes, bandwidthRecorded))
        planeAdmission.clear();
}
