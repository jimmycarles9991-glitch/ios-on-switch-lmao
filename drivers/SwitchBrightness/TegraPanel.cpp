// SPDX-License-Identifier: BSD-2-Clause-Patent
// DCS commands to the Switch OLED panel while the display controller scans out; see TegraPanel.h.
// DC and DSI offsets are 32-bit word indices as in hekate bdk/display/di.h (DISPLAY_A(off) is 0x54200000 + off * 4,
// DSI(off) is 0x54300000 + off * 4); the packet sequence is hekate di.c display_dsi_vblank_write(), the panel commands
// are hekate's PANEL_SAM_AMS699VC01 paths and switchroot L4T's panel-s-720p-7-0.dtsi (init and suspend command lists).
//
// Timing. Hekate polls DC_CMD_INT_STATUS and makes every write of the sequence microseconds after a DC A FRAME_END. A
// thread that sleeps on frame ends cannot: the display's interrupt action runs under its work-loop gate, often many
// milliseconds late, and a frame end that latched before the enable could release the packet in the same frame. So the
// thread only hands the packet over and sleeps; the display calls tegra_panel_frame_end_filter() from its DC A interrupt
// filter (primary interrupt context) at every FRAME_END, and the filter plays one step per frame end:
//   1  DSI_VIDEO_MODE_CONTROL = CMD_PKT_VID_ENABLE | LINE_TYPE(4): host packets ride the video stream in a blanking line
//      (hekate _display_dsi_wait_vblank(true))
//   2  the packet words into DSI_WR_DATA, back to back, a frame end after the enable as in hekate
//   3  nothing: one frame more than hekate, so that a write that interrupt latency pushed past the blanking lines has
//      gone out by the reset as well
//   4  14 us (L4T tegra_dsi_wait_frame_end: v_ref_to_sync lines), the DSI syncpoint block soft reset, 300 us, clear,
//      300 us, DSI_VIDEO_MODE_CONTROL = 0 (hekate _display_dsi_wait_vblank(false), L4T tegra_dsi_syncpt_reset)
// Step 4's waits are hardware-timer callouts (timer_call, SYS_CRITICAL: no coalescing), not spins in the filter, so no
// CPU sits with interrupts off for them. Each wait is a minimum: a late callout only holds the syncpoint reset, or the
// command packets' video mode with no packet queued, a little longer. DC_DISP_DISPLAY_DBG_TIMING (L4T dc_reg.h, its
// tegra_dc_get_v_count) shows whether the raster is still in vertical blanking 14 us after the frame end; if latency let
// it leave, the step waits for the next frame end, at most twice, and then goes ahead (the first command that had to
// logs one line). The OLED's 1000 x 1300 pixel frame at 78 MHz has 20 blanking lines of 12.8 us (256 us), so a callout
// some tens of microseconds late still reads blanking; step 4 lasts about 614 us, so the reset is released and the
// video mode restored in the first active lines, as in hekate. A command takes four frames (67 ms).
// TegraPanelSteps.h has the order of the steps.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
extern "C" {
#include <kern/timer_call.h>  // no C linkage of its own
}
#include <pexpert/pexpert.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "SwitchBrightness.h"
#include "TegraPanel.h"
#include "TegraPanelSteps.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchMMIO/SwitchMMIOWords.h"

// The DC A interrupt's owner (NativeLegacyDisplay) keeps FRAME_END unmasked while any holder has it; any context. These
// defaults stand for a display that never masks it.
extern "C" __attribute__((weak)) void native_legacy_frame_end_hold(void)
{}

extern "C" __attribute__((weak)) void native_legacy_frame_end_release(void)
{}

extern "C" bool native_legacy_frame_end_held(void);

namespace {
using namespace tegra_panel;
constexpr vm_offset_t kCarPA = 0x60006000, kDisplayAPA = 0x54200000, kDsiPA = 0x54300000;
constexpr vm_size_t kDcMapBytes = 0x2000;  // through DC_DISP_DISPLAY_DBG_TIMING
constexpr uint32_t kDisplayAInterrupt =
    105;  // GIC SPI 73 (hekate irq.h IRQ_DISPLAY, mainline tegra210.dtsi dc@54200000)

// Clock and reset controller, byte offsets (hekate bdk/soc/clock.h): DISP1 is L27, DSI is H16.
enum : uint32_t {
    RST_DEVICES_L = 0x004,
    RST_DEVICES_H = 0x008,
    CLK_OUT_ENB_L = 0x010,
    CLK_OUT_ENB_H = 0x014
};

constexpr uint32_t kDisp1 = 1u << 27, kDsiClock = 1u << 16;

// Display controller A, word indices. DISPLAY_COMMAND bits 6:5 are the controller mode, 0 = stopped.
enum : uint32_t {
    DC_CMD_DISPLAY_COMMAND = 0x32,
    DC_CMD_INT_STATUS = 0x37,
    DC_CMD_INT_MASK = 0x38,
    DC_CMD_INT_ENABLE = 0x39,
    DC_COM_SCRATCH_REGISTER_B = 0x326,
    DC_DISP_DISPLAY_DBG_TIMING = 0x4f6,
};

constexpr uint32_t kDisplayModeMask = 3u << 5, kFrameEndInt = 1u << 1;
constexpr uint32_t kDbgVBlank = 1u << 15;  // DBG_TIMING DBG_V_BLANK; V_COUNT is bits 14:0, H_COUNT bits 30:16

// DSI A, word indices.
enum : uint32_t {
    DSI_INCR_SYNCPT_CNTRL = 0x01,
    DSI_WR_DATA = 0x0a,
    DSI_POWER_CONTROL = 0x0b,
    DSI_CONTROL = 0x10,
    DSI_STATUS = 0x15,
    DSI_VIDEO_MODE_CONTROL = 0x4e,
};

constexpr uint32_t kSyncptSoftReset = 1u << 0, kDsiPowerEnable = 1u << 0, kDsiVideoEnable = 1u << 1;
constexpr uint32_t kCommandsInVideo = 1u << 0, kOnLine4 = 4u << 1;  // DSI_CMD_PKT_VID_ENABLE, DSI_DSI_LINE_TYPE(4)
constexpr uint32_t kLineBufferErrors = 3u << 8;  // DSI_STATUS LB_OVERFLOW (8), LB_UNDERFLOW (9), L4T dsi_regs.h

// MIPI DSI data types and DCS commands (hekate di.h, Linux include/video/mipi_display.h).
constexpr uint32_t kShortWrite = 0x05, kShortWriteParam = 0x15, kLongWrite = 0x39;
constexpr uint32_t kEnterSleep = 0x10, kExitSleep = 0x11, kDisplayOff = 0x28, kDisplayOn = 0x29;
constexpr uint32_t kSetBrightness = 0x51, kControlDisplay = 0x53;
constexpr uint32_t kBrightnessControl = 1u << 5, kDimmingControl = 1u << 3;  // 0x28, as hekate and L4T send it

constexpr unsigned kFrameTimeoutUS = 50000;  // per frame end, and per step-4 callout: three frames at 60 Hz

// The one command out, shared with the filter and the step timer. The thread fills it and publishes step = kQueued;
// from then on only the filter and the timer write it, one at a time (a step moves at a frame end or at the timer,
// never both), until one of them publishes kDone.
struct Command {
    uint32_t step;
    uint32_t words[2];
    unsigned count;  // 0: no packet, the command is the next frame end
    unsigned deferrals;
    uint32_t timing;  // DC_DISP_DISPLAY_DBG_TIMING at step 4
    uint32_t status;  // DSI_STATUS at step 4
};

Command gCommand;
uint64_t gFrames;       // frame ends the filter took while a command was out (atomic)
bool gFrameEndsDriven;  // the display calls the filter (atomic)
timer_call_data_t gStepTimer;
uint64_t gStepDeadline;  // mach absolute time the step timer was last armed for

IOLock *volatile gLock;  // serializes commands
volatile uint8_t *gCar;
SwitchMMIOWords gDc, gDsi;
bool gControlSent, gHaveLevel;
unsigned gLevel;

IOLock *makeLock(IOLock *volatile *slot)
{
    if (!*slot) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, slot))
            IOLockFree(lock);
    }
    return *slot;
}

void stepTimerFired(timer_call_param_t, timer_call_param_t);

// Primary interrupt context (the DC A filter or the step timer). Publishes the step, then arms its timer: a callout
// that fires at once still reads the step it is for.
void advance(Move move)
{
    __atomic_store_n(&gCommand.step, move.next, __ATOMIC_RELEASE);
    if (!move.timerUS)
        return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(move.timerUS, kMicrosecondScale, &deadline);
    __atomic_store_n(&gStepDeadline, deadline, __ATOMIC_RELAXED);
    timer_call_enter(&gStepTimer, deadline, TIMER_CALL_SYS_CRITICAL | TIMER_CALL_LOCAL);
}

// Under the lock. Reading a gated or reset block would stall the bus, so the clocks come first. A stopped DC
// (NativeLegacyDisplay's display power off) sends no stream for a packet to ride.
bool displayRunning()
{
    if (!gCar) {
        timer_call_setup(&gStepTimer, stepTimerFired, nullptr);
        gDc = SwitchMMIOWords(switch_mmio_map(kDisplayAPA, kDcMapBytes));
        gDsi = SwitchMMIOWords(switch_mmio_map(kDsiPA, 0x1000));
        gCar = switch_mmio_map(kCarPA, 0x1000);
    }
    if (!gCar || !gDc || !gDsi)
        return false;
    if (!(switch_mmio_read32(gCar, CLK_OUT_ENB_L) & kDisp1) || (switch_mmio_read32(gCar, RST_DEVICES_L) & kDisp1) ||
        !(switch_mmio_read32(gCar, CLK_OUT_ENB_H) & kDsiClock) ||
        (switch_mmio_read32(gCar, RST_DEVICES_H) & kDsiClock)) {
        return false;
    }
    return ((gDsi[DSI_POWER_CONTROL] & kDsiPowerEnable) != 0u) && ((gDsi[DSI_CONTROL] & kDsiVideoEnable) != 0u) &&
           ((gDc[DC_CMD_DISPLAY_COMMAND] & kDisplayModeMask) != 0u);
}

Step step()
{
    return static_cast<Step>(__atomic_load_n(&gCommand.step, __ATOMIC_ACQUIRE));
}

uint64_t frames()
{
    return __atomic_load_n(&gFrames, __ATOMIC_ACQUIRE);
}

uint64_t frameDeadline()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kFrameTimeoutUS, kMicrosecondScale, &deadline);
    return deadline;
}

[[noreturn]] void lostFrameEnd(const char *what)
{
    const Step now = step();
    if (movesAtTimer(now)) {
        panic("TegraPanel: %s: step %u's timer callout, due at %llu, had not run 50 ms later (now %llu)", what,
              static_cast<unsigned>(now), __atomic_load_n(&gStepDeadline, __ATOMIC_RELAXED), mach_absolute_time());
    }
    char gic[192];
    switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
    panic("TegraPanel: %s: no DC A frame end in 50 ms (step %u): INT_STATUS %#x MASK %#x ENABLE %#x CMD %#x DBG_TIMING "
          "%#x, %s",
          what, static_cast<unsigned>(now), static_cast<uint32_t>(gDc[DC_CMD_INT_STATUS]),
          static_cast<uint32_t>(gDc[DC_CMD_INT_MASK]), static_cast<uint32_t>(gDc[DC_CMD_INT_ENABLE]),
          static_cast<uint32_t>(gDc[DC_CMD_DISPLAY_COMMAND]), static_cast<uint32_t>(gDc[DC_DISP_DISPLAY_DBG_TIMING]),
          gic);
}

// Under the lock, the display running. Hands the filter `count` packet words (none: the command is the next frame end)
// and sleeps until it has played them. Each frame end the filter takes wakes this thread (thread_wakeup is safe in
// primary interrupt context; the wait is asserted before the state is checked, so none is missed).
void play(const uint32_t *words, unsigned count, const char *what)
{
    if (!__atomic_load_n(&gFrameEndsDriven, __ATOMIC_ACQUIRE)) {
        panic("TegraPanel: %s: the display reports no DC A frame ends (it does not call tegra_panel_frame_end_filter)",
              what);
    }
    native_legacy_frame_end_hold();
    if (!native_legacy_frame_end_held()) {
        panic("TegraPanel: %s: DC A FRAME_END is not enabled and unmasked (INT_ENABLE %#x MASK %#x)", what,
              static_cast<uint32_t>(gDc[DC_CMD_INT_ENABLE]), static_cast<uint32_t>(gDc[DC_CMD_INT_MASK]));
    }
    for (unsigned i = 0; i < count; ++i)
        gCommand.words[i] = words[i];
    gCommand.count = count;
    gCommand.deferrals = 0;
    const uint32_t before = gDsi[DSI_STATUS];
    uint64_t seen = frames();
    uint64_t deadline = frameDeadline();
    __atomic_store_n(&gCommand.step, kQueued, __ATOMIC_RELEASE);
    for (;;) {
        assert_wait_deadline(reinterpret_cast<event_t>(&gCommand), THREAD_UNINT, deadline);
        wait_result_t result = THREAD_AWAKENED;
        if (step() == kDone || frames() != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        } else {
            result = thread_block(THREAD_CONTINUE_NULL);
        }
        if (step() == kDone)
            break;
        if (frames() != seen) {
            seen = frames();
            deadline = frameDeadline();
        } else if (result == THREAD_TIMED_OUT) {
            lostFrameEnd(what);
        }
    }
    __atomic_store_n(&gCommand.step, kIdle, __ATOMIC_RELAXED);
    native_legacy_frame_end_release();
    if (!count)
        return;
    static bool lateReset;
    static bool lineBuffer;
    if (!(gCommand.timing & kDbgVBlank) && !lateReset) {
        lateReset = true;
        IOLog("TegraPanel: %s: step 4 ran outside vertical blanking after %u frame ends (DBG_TIMING %#x)\n", what,
              gCommand.deferrals, gCommand.timing);
    }
    if ((gCommand.status & ~before & kLineBufferErrors) && !lineBuffer) {
        lineBuffer = true;
        IOLog("TegraPanel: %s: DSI line buffer over/underflow (DSI_STATUS %#x before, %#x after)\n", what, before,
              gCommand.status);
    }
}

// hekate display_dsi_vblank_write() for an already encoded packet.
int sendPacket(const uint32_t *words, unsigned count, const char *what)
{
    if (!displayRunning())
        return -1;
    play(words, count, what);
    return 0;
}

int sendCommand(uint32_t command, const char *what)
{
    const uint32_t word = command << 8 | kShortWrite;
    return sendPacket(&word, 1, what);
}

int sendCommand(uint32_t command, uint32_t parameter, const char *what)
{
    const uint32_t word = (command | parameter << 8) << 8 | kShortWriteParam;
    return sendPacket(&word, 1, what);
}

// DCS long write: header (3 bytes: command + 2 parameter bytes), then the payload, big-endian level.
int sendBrightness(unsigned level)
{
    const uint32_t words[2] = {3u << 8 | kLongWrite, kSetBrightness | (level >> 8) << 8 | (level & 0xff) << 16};
    return sendPacket(words, 2, "brightness");
}

// Under the lock.
int setBrightness(unsigned level)
{
    int result = 0;
    if (!gControlSent) {
        result = sendCommand(kControlDisplay, kBrightnessControl | kDimmingControl, "brightness control");
        gControlSent = result == 0;
    }
    if (!result)
        result = sendBrightness(level);
    return result;
}

int bootLevel()
{
    if (!displayRunning())
        return -1;
    const uint32_t duty = gDc[DC_COM_SCRATCH_REGISTER_B];
    if (!duty || duty > 255)
        return -1;
    return static_cast<int>(duty * TEGRA_PANEL_BRIGHTNESS_MAX / 255);
}

template <typename F> int locked(F body)
{
    IOLock *lock = makeLock(&gLock);
    if (!lock)
        return -1;
    IOLockLock(lock);
    const int result = body();
    IOLockUnlock(lock);
    return result;
}
}  // namespace

extern "C" int tegra_panel_set_brightness(unsigned level)
{
    if (level > TEGRA_PANEL_BRIGHTNESS_MAX)
        level = TEGRA_PANEL_BRIGHTNESS_MAX;
    return locked([level] {
        const int result = setBrightness(level);
        if (!result) {
            gLevel = level;
            gHaveLevel = true;
        }
        return result;
    });
}

extern "C" int tegra_panel_power_off(void)
{
    return locked([] {
        // Remember what was showing before the brightness goes to 0, for tegra_panel_power_on(-1).
        if (!gHaveLevel) {
            const int boot = bootLevel();
            if (boot >= 0) {
                gLevel = static_cast<unsigned>(boot);
                gHaveLevel = true;
            }
        }
        int result = sendBrightness(0);
        if (result)
            return result;
        IOSleep(100);  // hekate _display_dsi_backlight_brightness: the panel's dimming ramp
        result = sendCommand(kDisplayOff, "display off");
        if (result)
            return result;
        IOSleep(40);
        result = sendCommand(kEnterSleep, "sleep in");
        if (result)
            return result;
        IOSleep(120);
        gControlSent = false;  // sent again on wake, as L4T's init list does
        return 0;
    });
}

extern "C" int tegra_panel_power_on(int level)
{
    return locked([level] {
        if (!displayRunning())
            return -1;
        play(nullptr, 0, "scanout restart");
        int result = sendCommand(kExitSleep, "sleep out");
        if (result)
            return result;
        IOSleep(180);
        result = sendCommand(kControlDisplay, kBrightnessControl | kDimmingControl, "brightness control");
        if (result)
            return result;
        gControlSent = true;
        result = sendCommand(kDisplayOn, "display on");
        if (result)
            return result;
        IOSleep(20);
        unsigned target = TEGRA_PANEL_BRIGHTNESS_MAX / 2;
        if (level >= 0) {
            target = static_cast<unsigned>(level);
        } else if (gHaveLevel) {
            target = gLevel;
        }
        if (target > TEGRA_PANEL_BRIGHTNESS_MAX)
            target = TEGRA_PANEL_BRIGHTNESS_MAX;
        result = sendBrightness(target);
        if (result)
            return result;
        gLevel = target;
        gHaveLevel = true;
        return 0;
    });
}

extern "C" int tegra_panel_boot_brightness(void)
{
    return locked([] {
        return bootLevel();
    });
}

extern "C" int tegra_panel_brightness(void)
{
    return __atomic_load_n(&gHaveLevel, __ATOMIC_RELAXED) ? (int)__atomic_load_n(&gLevel, __ATOMIC_RELAXED) : -1;
}

extern "C" bool tegra_panel_holds_frame_end(void)
{
    return true;
}

extern "C" __attribute__((weak)) bool native_legacy_frame_end_held(void)
{
    return (gDc[DC_CMD_INT_ENABLE] & gDc[DC_CMD_INT_MASK] & kFrameEndInt) != 0;
}

// Primary interrupt context, at every DC A FRAME_END; one step of the command out, if any (see the top of this file).
extern "C" void tegra_panel_frame_end_filter(void)
{
    if (!__atomic_load_n(&gFrameEndsDriven, __ATOMIC_RELAXED))
        __atomic_store_n(&gFrameEndsDriven, true, __ATOMIC_RELEASE);
    const Step now = step();
    if (now == kIdle || now == kDone || movesAtTimer(now))
        return;
    if (now == kQueued && gCommand.count) {
        gDsi[DSI_VIDEO_MODE_CONTROL] = kCommandsInVideo | kOnLine4;
    } else if (now == kEnabled) {
        for (unsigned i = 0; i < gCommand.count; ++i)
            gDsi[DSI_WR_DATA] = gCommand.words[i];
    }
    __atomic_fetch_add(&gFrames, 1, __ATOMIC_RELEASE);
    advance(atFrameEnd(now, gCommand.count));
    thread_wakeup((event_t)&gCommand);
}

namespace {
// Primary interrupt context, the step timer's callout: the rest of step 4.
void stepTimerFired(timer_call_param_t, timer_call_param_t)
{
    const Step now = step();
    if (!movesAtTimer(now))
        return;
    bool inBlanking = false;
    if (now == kBlankCheck) {
        gCommand.timing = gDc[DC_DISP_DISPLAY_DBG_TIMING];
        inBlanking = (gCommand.timing & kDbgVBlank) != 0;
    }
    const Move move = atTimer(now, inBlanking, gCommand.deferrals);
    switch (move.next) {
    case kSettled:
        ++gCommand.deferrals;
        break;
    case kResetHeld:
        gCommand.status = gDsi[DSI_STATUS];
        gDsi[DSI_INCR_SYNCPT_CNTRL] = kSyncptSoftReset;
        break;
    case kResetReleased:
        gDsi[DSI_INCR_SYNCPT_CNTRL] = 0;
        break;
    case kDone:
        gDsi[DSI_VIDEO_MODE_CONTROL] = 0;
        break;
    default:
        return;
    }
    advance(move);
    if (move.next == kDone)
        thread_wakeup((event_t)&gCommand);
}
}  // namespace
