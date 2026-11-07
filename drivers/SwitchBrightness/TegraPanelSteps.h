// SPDX-License-Identifier: BSD-2-Clause-Patent
// The order of a DCS command's steps (TegraPanel.cpp's file comment), header-only so the host test (test/) runs this
// exact code. Steps 1-3 and the start of step 4 move at DC A frame ends; the rest of step 4 moves at timer expiries.
#ifndef TEGRA_PANEL_STEPS_H
#define TEGRA_PANEL_STEPS_H
#include <stdint.h>

namespace tegra_panel {

enum Step : uint32_t {
    kIdle,
    kQueued,
    kEnabled,
    kWritten,
    kSettled,
    kBlankCheck,     // 14 us after the frame end: still in vertical blanking?
    kResetHeld,      // the DSI syncpoint block in soft reset for 300 us
    kResetReleased,  // released, 300 us before plain video again
    kDone
};

constexpr uint32_t kBlankCheckUS = 14;    // L4T tegra_dsi_wait_frame_end: v_ref_to_sync lines
constexpr uint32_t kResetSettleUS = 300;  // hekate _display_dsi_wait_vblank(false), each side of the release
constexpr unsigned kResetDeferrals = 2;   // frame ends kBlankCheck may wait for vertical blanking

struct Move {
    Step next;
    uint32_t timerUS;  // 0: no timer; the next move is a frame end's (or the command is done)
};

constexpr bool movesAtTimer(Step step)
{
    return step == kBlankCheck || step == kResetHeld || step == kResetReleased;
}

// At a frame end. `words`: the packet's length (0: the command is this frame end). Steps that move at a timer, or
// that no command is in, stay as they are.
constexpr Move atFrameEnd(Step step, unsigned words)
{
    switch (step) {
    case kQueued:
        return {words ? kEnabled : kDone, 0};
    case kEnabled:
        return {kWritten, 0};
    case kWritten:
        return {kSettled, 0};
    case kSettled:
        return {kBlankCheck, kBlankCheckUS};
    default:
        return {step, 0};
    }
}

// At the step's timer. `inBlanking`: DC_DISP_DISPLAY_DBG_TIMING showed vertical blanking at kBlankCheck; `deferrals`:
// frame ends kBlankCheck already waited. Out of blanking it waits for the next frame end, at most kResetDeferrals
// times, and then goes ahead.
constexpr Move atTimer(Step step, bool inBlanking, unsigned deferrals)
{
    switch (step) {
    case kBlankCheck:
        if (!inBlanking && deferrals < kResetDeferrals)
            return {kSettled, 0};
        return {kResetHeld, kResetSettleUS};
    case kResetHeld:
        return {kResetReleased, kResetSettleUS};
    case kResetReleased:
        return {kDone, 0};
    default:
        return {step, 0};
    }
}

}  // namespace tegra_panel
#endif
