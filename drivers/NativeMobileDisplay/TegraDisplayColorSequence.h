// SPDX-License-Identifier: BSD-2-Clause-Patent
// When DC A's colour management unit may be written. Its LUT1, CSC and LUT2 have no assembly copy: a word written while
// the CMU scans changes the pixels from that point of the frame on. Only CMU_ENABLE (DISP_COLOR_CONTROL) latches, at the
// frame start after its GENERAL_ACT_REQ. So:
//   * with the CMU bypassed (CMU_ENABLE clear and latched) every word may be written at any time;
//   * a CSC-only change (a white point ramp, a brightness scale, a blank) is nine words, written at a frame end while
//     DBG_TIMING still shows vertical blanking, or at the next frame end if interrupt latency let the raster leave it;
//   * a LUT change with the CMU on (contrast, gamma, inversion) first bypasses the CMU, writes once the bypass has
//     latched and enables again at a later frame end, as L4T's _tegra_dc_update_cmu does.
// C++, header only, for the driver and its host test.
#pragma once
#include <stdint.h>
#include <string.h>
#include "TegraDisplayColorMath.h"

namespace switch_color {

// The filter's own: what it adopted and has written.
struct FilterSequence {
    bool pendingDirty = false;  // the adopted tables are not all in DC A yet
    bool activeValid = false;   // the written tables are what DC A holds
    bool bypassWanted = false;  // LUT words to change while the CMU may be scanning them
    uint16_t latchedEpoch = 0;  // the last bypass whose GENERAL_ACT_REQ the filter saw taken
};

// The action's own: the CMU_ENABLE it wrote.
struct ActionSequence {
    bool enableAsked = false;  // the CMU_ENABLE last written
    uint16_t bypassEpoch = 0;  // one more for each CMU_ENABLE clear written
};

// CMU_ENABLE cleared, its GENERAL_ACT_REQ not yet seen taken.
inline bool bypassLatching(const FilterSequence &f, const ActionSequence &a)
{
    return a.bypassEpoch != f.latchedEpoch;
}

enum class Write : uint8_t {
    kNothing,
    kAll,
    kCsc
};
enum class Enable : uint8_t {
    kKeep,
    kOn,
    kOff
};

inline bool lutsDiffer(const Tables &a, const Tables &b)
{
    return memcmp(a.lut1, b.lut1, sizeof(a.lut1)) != 0 || memcmp(a.lut2, b.lut2, sizeof(a.lut2)) != 0;
}

// The filter at a frame end: which of the pending words go to DC A now. `written` is what DC A holds (null when
// unknown); actRequestOut(): STATE_CONTROL's GENERAL_ACT_REQ reads set; inBlank(): DBG_TIMING's V_BLANK reads set.
template <typename ActRequestOut, typename InBlank>
inline Write atFrameEnd(FilterSequence &f, const ActionSequence &a, const Tables &pending, const Tables *written,
                        ActRequestOut actRequestOut, InBlank inBlank)
{
    if (bypassLatching(f, a) && !actRequestOut())
        f.latchedEpoch = a.bypassEpoch;
    if (!f.pendingDirty)
        return Write::kNothing;
    // A disabled CMU's tables are never written: the ones it scans stay until the disable latches.
    if (!pending.enabled) {
        f.pendingDirty = false;
        f.bypassWanted = false;
        return Write::kNothing;
    }
    if (!a.enableAsked && !bypassLatching(f, a)) {
        f.pendingDirty = false;
        f.bypassWanted = false;
        f.activeValid = true;
        return Write::kAll;
    }
    f.bypassWanted = written == nullptr || lutsDiffer(pending, *written);
    if (f.bypassWanted || !inBlank())
        return Write::kNothing;
    f.pendingDirty = false;
    return Write::kCsc;
}

// The action at a frame end, once the filter has run: the CMU_ENABLE to write, with its GENERAL_ACT_REQ. mayRequest
// false: another request is out and owns GENERAL_ACT_REQ for now.
inline Enable atFrameEndAction(const FilterSequence &f, const ActionSequence &a, bool want, bool mayRequest)
{
    const bool target = want && !f.bypassWanted;
    if (!mayRequest || target == a.enableAsked)
        return Enable::kKeep;
    if (target && (f.pendingDirty || !f.activeValid))
        return Enable::kKeep;
    return target ? Enable::kOn : Enable::kOff;
}

// After the action wrote CMU_ENABLE and its GENERAL_ACT_REQ (in that order: the filter reads the request's bit to
// know when the bypass has latched).
inline void enableWritten(ActionSequence &a, bool on)
{
    a.enableAsked = on;
    if (!on)
        a.bypassEpoch = static_cast<uint16_t>(a.bypassEpoch + 1);
}

inline bool settled(const FilterSequence &f, const ActionSequence &a, bool want)
{
    return !f.pendingDirty && !f.bypassWanted && want == a.enableAsked;
}

}  // namespace switch_color
