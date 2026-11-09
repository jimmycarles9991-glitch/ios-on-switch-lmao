// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's hardware-plane diagnostics, debug kernels only. Included in the class body after
// NativePlanesDisplay.h.
//  - Counters since boot: kern.switch_planes_{flips,latched,maps,dropped}, which hw_test_app's display.planes reads around a
//    video it plays. The pacing line's counters restart every stats window (noteStatsWindow); these add each closed window,
//    and a read adds the open one's.
//  - A detached layer refused for its format or layout: one line with its surface as the kernel's IOSurface describes it,
//    once per format, reason and geometry (Core Animation resends a refused swap every frame).

#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kPlaneTotals = 4;
static inline uint32_t planeTotals[kPlaneTotals] = {};
static inline struct sysctl_oid planeTotalOids[kPlaneTotals] = {};

void planeTotalsWindow(uint32_t (&window)[kPlaneTotals]) const
{
    window[0] = stPlaneFlips;
    window[1] = stPlaneLatched;
    window[2] = stPlaneMaps;
    window[3] = stPlaneDropped;
}

void planeTotalsClose() const
{
    uint32_t window[kPlaneTotals];
    planeTotalsWindow(window);
    for (unsigned i = 0; i < kPlaneTotals; ++i)
        planeTotals[i] += window[i];
}

static int planeTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    uint32_t open[kPlaneTotals] = {};
    if (const NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE))
        display->planeTotalsWindow(open);
    const unsigned i = static_cast<unsigned>(which) % kPlaneTotals;
    const int value = static_cast<int>(planeTotals[i] + open[i]);
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void planeTotalsRegister()
{
    static const char *const names[kPlaneTotals] = {"switch_planes_flips", "switch_planes_latched",
                                                    "switch_planes_maps", "switch_planes_dropped"};
    for (unsigned i = 0; i < kPlaneTotals; ++i) {
        if (planeTotalOids[i].oid_name)
            continue;
        switch_sysctl_oid(&planeTotalOids[i], &sysctl__kern_children, names[i],
                          CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, planeTotalSysctl, "I",
                          "window B since boot: sets written, sets latched, slot mappings, detached layers not shown");
        planeTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&planeTotalOids[i]);
    }
}

static void planeNoteRefused(unsigned reason, uint32_t updated, const bool hasSurface[native_planes::kLayers],
                             const native_planes::Layer layers[native_planes::kLayers], uint32_t width, uint32_t height)
{
    if (reason != native_planes::kFormat && reason != native_planes::kSurface && reason != native_planes::kAlignment)
        return;
    for (unsigned i = 0; i < native_planes::kLayers; ++i) {
        const native_planes::Layer &l = layers[i];
        if (!(updated & (1u << i)) || !hasSurface[i] || native_planes::isUi(l, width, height) ||
            native_planes::checkDetached(l, width, height) != reason)
            continue;
        const native_planes::Surface &s = l.surface;
        uint64_t key = switch_refusal_key_with(switch_refusal_key("display", "plane-layout"),
                                               static_cast<uint64_t>(reason) << 32 | s.format);
        key = switch_refusal_key_with(key, static_cast<uint64_t>(s.stride[1]) << 32 | s.stride[0]);
        key = switch_refusal_key_with(key, s.allocation);
        switch_refusal_k(key, "display", "plane-layout",
                         "layer %u refused (reason %u): format %#x '%c%c%c%c' %ux%u planes %u strides %u/%u plane "
                         "heights %u/%u offsets %#llx/%#llx allocation %llu",
                         i, reason, s.format, static_cast<char>(s.format >> 24), static_cast<char>(s.format >> 16),
                         static_cast<char>(s.format >> 8), static_cast<char>(s.format), s.width, s.height, s.planes,
                         s.stride[0], s.stride[1], s.planeHeight[0], s.planeHeight[1],
                         static_cast<unsigned long long>(s.offset[0]), static_cast<unsigned long long>(s.offset[1]),
                         static_cast<unsigned long long>(s.allocation));
        return;
    }
}
#else
void planeTotalsClose() const
{}

static void planeTotalsRegister()
{}

static void planeNoteRefused(unsigned, uint32_t, const bool *, const native_planes::Layer *, uint32_t, uint32_t)
{}
#endif
