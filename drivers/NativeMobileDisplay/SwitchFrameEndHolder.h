// SPDX-License-Identifier: BSD-2-Clause-Patent
// What kept DC A's FRAME_END routed to the CPU at a frame end (debug kernels count them per stats window, in the
// "SwitchDisplay locks" line): the first of the display's own reasons (frameEndHolder), else a debug display
// experiment, a frame-end hold (TegraPanel's commands, the colour unit), or a panel driver that never lets it be
// masked. Only the console tape, the probes and the experiments are debug-only; a release kernel keeps the rest.
#pragma once
#include <stdint.h>

namespace switch_frame_end {

enum Holder : unsigned {
    kNone,
    kFirst,       // the first two frame ends prove the interrupt alive
    kVsync,       // a vsync client is registered
    kTape,        // the console tape scrolls or settles
    kKick,        // a swap waits for a frame end (kickDeferred)
    kPresent,     // a present waits for one (presentDeferred)
    kRing,        // a window A flip is asked for, replaced, being drawn or held
    kRotation,    // the rotated scanout has not latched yet
    kProbe,       // the SMMU or plane probe
    kPlanes,      // an overlay set is out, held or recovering
    kLandscape,   // a change of mode waits
    kCompletion,  // a completion flip is out
    kOffer,       // a GPU offer is out, its age looked at per frame end
    kAttach,      // the scanout attach counts frame ends
    kBlank,       // a colour-blanked swap waits for its frame end
    kExperiment,  // a debug display experiment runs (column scan, window probe, host1x flip, landscape sample)
    kHold,        // native_legacy_frame_end_hold: a TegraPanel command or a colour change in flight
    kPanel,       // the panel driver does not hold its own frame ends (tegra_panel_holds_frame_end false)
    kHolders
};

inline constexpr const char *kHolderNames[kHolders] = {
    "none",   "first",     "vsync",      "tape",  "kick",   "present", "ring",       "rotation", "probe",
    "planes", "landscape", "completion", "offer", "attach", "blank",   "experiment", "hold",     "panel"};

inline bool debugOnly(Holder h)
{
    return h == kTape || h == kProbe || h == kExperiment;
}

// Frame ends routed by each holder over one stats window; release() is what a release kernel would have routed too.
struct RoutedCounts {
    uint32_t by[kHolders] = {};

    void add(Holder h)
    {
        ++by[h < kHolders ? h : kNone];
    }

    uint32_t routed() const
    {
        uint32_t n = 0;
        for (unsigned h = kFirst; h < kHolders; ++h)
            n += by[h];
        return n;
    }

    uint32_t release() const
    {
        uint32_t n = 0;
        for (unsigned h = kFirst; h < kHolders; ++h)
            if (!debugOnly(static_cast<Holder>(h)))
                n += by[h];
        return n;
    }
};

}  // namespace switch_frame_end
