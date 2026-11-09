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

struct Sequence {
    bool pendingDirty = false;    // the pending tables are not all in DC A yet
    bool activeValid = false;     // the written tables are what DC A holds
    bool enableAsked = false;     // the CMU_ENABLE last written
    bool bypassLatching = false;  // CMU_ENABLE cleared, its GENERAL_ACT_REQ still out
    bool bypassWanted = false;    // LUT words to change while the CMU may be scanning them
};

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
inline Write atFrameEnd(Sequence &s, const Tables &pending, const Tables *written, ActRequestOut actRequestOut,
                        InBlank inBlank)
{
    if (s.bypassLatching && !actRequestOut())
        s.bypassLatching = false;
    if (!s.pendingDirty)
        return Write::kNothing;
    // A disabled CMU's tables are never written: the ones it scans stay until the disable latches.
    if (!pending.enabled) {
        s.pendingDirty = false;
        s.bypassWanted = false;
        return Write::kNothing;
    }
    if (!s.enableAsked && !s.bypassLatching) {
        s.pendingDirty = false;
        s.bypassWanted = false;
        s.activeValid = true;
        return Write::kAll;
    }
    s.bypassWanted = written == nullptr || lutsDiffer(pending, *written);
    if (s.bypassWanted || !inBlank())
        return Write::kNothing;
    s.pendingDirty = false;
    return Write::kCsc;
}

// The action at a frame end, once the filter has run: the CMU_ENABLE to write, with its GENERAL_ACT_REQ. mayRequest
// false: another request is out and owns GENERAL_ACT_REQ for now.
inline Enable atFrameEndAction(const Sequence &s, bool want, bool mayRequest)
{
    const bool target = want && !s.bypassWanted;
    if (!mayRequest || target == s.enableAsked)
        return Enable::kKeep;
    if (target && (s.pendingDirty || !s.activeValid))
        return Enable::kKeep;
    return target ? Enable::kOn : Enable::kOff;
}

// After the action wrote CMU_ENABLE and its GENERAL_ACT_REQ (in that order: the filter reads the request's bit to
// know when the bypass has latched).
inline void enableWritten(Sequence &s, bool on)
{
    s.enableAsked = on;
    s.bypassLatching = !on;
}

inline bool settled(const Sequence &s, bool want)
{
    return !s.pendingDirty && !s.bypassWanted && want == s.enableAsked;
}

}  // namespace switch_color
