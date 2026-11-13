// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test's display.x1.landscape_video, debug kernels only (SwitchLandscapeRunABI.h): included in the class body after
// SwitchLandscapeDisplay.h and DisplayWindowProbeDisplay.h. A SAMPLE records the next frame ends as they end: DC A's
// CRC (the CRC unit on for the sample, as the probes read it), whether the UI was on window B, a change of mode out,
// a composition latched; and over the sample the changes of mode, latches and underflows per window, VIC's jobs and
// recorded turns, and the CPU the display's kicks and frame ends and TegraVIC took. The frame ends are held routed
// while it runs. Its state is static: the instance is at the 16 KiB a kalloc_type class may have.

#if SWITCH_FRAME_TIMELINE
struct LandscapeRunState {
    SwitchDisplayLandscapeReply reply;
    uint32_t want, savedCrc;
    bool holding, overlayLatched, charging;
    SwitchVICTurnCounters vicAt;
    uint64_t displayTicks;
};

static LandscapeRunState landscapeRun;

static bool landscapeRunSampling()
{
    return landscapeRun.reply.state == SWITCH_DISPLAY_LANDSCAPE_SAMPLING;
}

// From TegraVIC's user-client thread.
IOReturn landscapeRunCall(const SwitchDisplayLandscapeRequest &request, SwitchDisplayLandscapeReply &reply)
{
    if (!getWorkLoop())
        return kIOReturnNotReady;
    if (request.op != SWITCH_DISPLAY_LANDSCAPE_SAMPLE && request.op != SWITCH_DISPLAY_LANDSCAPE_QUERY)
        return kIOReturnBadArgument;
    windowProbeOnLoop([&] {
        const int error = request.op == SWITCH_DISPLAY_LANDSCAPE_SAMPLE ? landscapeRunStart(request.value) : 0;
        reply = landscapeRun.reply;
        reply.error = error ? error : reply.error;
    });
    return kIOReturnSuccess;
}

// 0 once the sample runs; a sample running already is left alone.
int landscapeRunStart(uint32_t frames)
{
    LandscapeRunState &s = landscapeRun;
    if (landscapeRunSampling())
        return EBUSY;
    s.reply = {};
    s.reply.magic = SWITCH_DISPLAY_LANDSCAPE_RUN_MAGIC;
    if (!frames || frames > SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES)
        return s.reply.error = EINVAL;
    if (!rotationLatched || hwSuspended || scanoutStopped)
        return s.reply.error = EAGAIN;
    s.want = frames;
    s.displayTicks = 0;
    s.overlayLatched = false;
    switch_vic_turn_counters(&s.vicAt);
    s.savedCrc = dc[kDcCrcControl];
    dc[kDcCrcControl] = kCrcOn;
    dc[kDcStateControl] = kGeneralUpdate;
    dc[kDcStateControl] = kGeneralActReq;
    s.holding = true;
    native_legacy_frame_end_hold();
    s.reply.state = SWITCH_DISPLAY_LANDSCAPE_SAMPLING;
    return 0;
}

void landscapeRunFinish(int error)
{
    LandscapeRunState &s = landscapeRun;
    dc[kDcCrcControl] = s.savedCrc;
    if (!hwSuspended) {
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
    }
    SwitchVICTurnCounters vic = {};
    switch_vic_turn_counters(&vic);
    for (unsigned i = 0; i != 3; ++i) {
        s.reply.vicJobs[i] = vic.jobs[i] - s.vicAt.jobs[i];
        s.reply.vicTurns[i] = vic.turns[i] - s.vicAt.turns[i];
        s.reply.vicEventTurns[i] = vic.eventTurns[i] - s.vicAt.eventTurns[i];
    }
    absolutetime_to_nanoseconds(s.displayTicks, &s.reply.displayNs);
    absolutetime_to_nanoseconds(vic.ticks - s.vicAt.ticks, &s.reply.vicNs);
    s.reply.error = error;
    s.reply.state = SWITCH_DISPLAY_LANDSCAPE_DONE;
    if (s.holding) {
        s.holding = false;
        native_legacy_frame_end_release();
    }
}

// Every DC A frame end, last.
void landscapeRunFrameEnd()
{
    LandscapeRunState &s = landscapeRun;
    if (!landscapeRunSampling())
        return;
    SwitchDisplayLandscapeReply &r = s.reply;
    const uint32_t at = r.frames;
    r.crc[at] = dc[kDcCrcLatched];
    r.mode[at] =
        uint8_t((landscapeOn() ? SWITCH_DISPLAY_LANDSCAPE_FRAME_LANDSCAPE : 0) |
                (landscapeState.switching || landscapeState.wanted ? SWITCH_DISPLAY_LANDSCAPE_FRAME_SWITCHING : 0) |
                (s.overlayLatched ? SWITCH_DISPLAY_LANDSCAPE_FRAME_OVERLAY : 0));
    s.overlayLatched = false;
    const SwitchDisplayTraits traits = switch_display_traits();
    r.traits = (traits.overlayBlockYuv ? SWITCH_DISPLAY_TRAIT_OVERLAY_BLOCK_YUV : 0) |
               (traits.columnScanYuv ? SWITCH_DISPLAY_TRAIT_COLUMN_SCAN_YUV : 0) |
               (traits.offsetCrop ? SWITCH_DISPLAY_TRAIT_OFFSET_CROP : 0);
    if (++r.frames >= s.want)
        landscapeRunFinish(0);
}

// The display stops, sleeps or goes off mid-sample: what was recorded is the reply.
void landscapeRunAbort()
{
    if (landscapeRunSampling())
        landscapeRunFinish(ECANCELED);
}

void landscapeRunUnderflow(uint32_t status)
{
    if (!landscapeRunSampling())
        return;
    for (unsigned w = 0; w != 3; ++w)
        landscapeRun.reply.underflows[w] += (status & switch_landscape::underflow(Window(w))) ? 1 : 0;
}

void landscapeRunLatched(bool overlay, bool modeChanged)
{
    if (!landscapeRunSampling())
        return;
    if (overlay) {
        ++landscapeRun.reply.overlayLatches;
        landscapeRun.overlayLatched = true;
    } else {
        ++landscapeRun.reply.uiLatches;
    }
    landscapeRun.reply.switches += modeChanged ? 1 : 0;
}

// The display's CPU over a sample: a kick made inside a frame end is that frame end's.
static uint64_t landscapeRunClock()
{
    if (!landscapeRunSampling() || landscapeRun.charging)
        return 0;
    landscapeRun.charging = true;
    return mach_absolute_time();
}

void landscapeRunCharge(uint64_t since)
{
    if (!since)
        return;
    landscapeRun.charging = false;
    if (landscapeRunSampling())
        landscapeRun.displayTicks += mach_absolute_time() - since;
}
#else
void landscapeRunFrameEnd()
{}

void landscapeRunAbort()
{}

void landscapeRunUnderflow(uint32_t)
{}

void landscapeRunLatched(bool, bool)
{}

static uint64_t landscapeRunClock()
{
    return 0;
}

void landscapeRunCharge(uint64_t)
{}
#endif
