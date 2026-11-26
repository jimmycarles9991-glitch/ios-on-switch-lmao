// SPDX-License-Identifier: GPL-2.0-only
// The BM92T36 alternate-mode engine on SwitchPD's work loop: SwitchPDAltModeSteps.h decides, this file does the I2C,
// the step timer and the hand-offs. DisplayPort's lanes go to SwitchDisplayPort; the SuperSpeed
// pair's fate and the dock's hub go to SwitchUSB (XusbLanes.h switch_usb_set_alt_lanes).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#include "../TegraI2C/TegraI2C.h"
#include "SwitchPDAltModeHook.h"
#include "SwitchPDAltModeSteps.h"
#include "SwitchDisplayPort.h"
#include "../SwitchUSB/XusbLanes.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;

enum : uint8_t {
    STATUS1 = 0x03,
    COMMAND = 0x05,
    DP_STATUS = 0x18,
    INCOMING_VDM = 0x50,
    OUTGOING_VDM = 0x60
};

constexpr bool kWantDisplayPort = true;

IOTimerEventSource *gTimer;
const void *gPD;
bool (*gAtRest)(const void *);
struct switch_pd_alt gAlt;
uint16_t gStatus1;
enum switch_pd_alt_lanes gToldLanes = SWITCH_PD_ALT_LANES_FREE;
bool gToldHub, gToldFlipped, gResumePending;
unsigned gToldDisplayPort;

void hand_off()
{
    if (gAlt.lanes != gToldLanes || gAlt.hub_enabled != gToldHub) {
        enum switch_usb3_lanes lanes = SWITCH_USB3_LANES_FREE;
        if (gAlt.lanes == SWITCH_PD_ALT_LANES_PENDING) {
            lanes = SWITCH_USB3_LANES_PENDING;
        } else if (gAlt.lanes == SWITCH_PD_ALT_LANES_TAKEN) {
            lanes = SWITCH_USB3_LANES_TAKEN;
        }
        switch_usb_set_alt_lanes(lanes, gAlt.hub_enabled);
    }
    gToldLanes = gAlt.lanes;
    gToldHub = gAlt.hub_enabled;
    const bool flipped = (gStatus1 & SWITCH_PD_ALT_STATUS1_CSIDE) != 0;
    const unsigned displayLanes = switch_pd_alt_display_lanes(&gAlt, gResumePending);
    if (displayLanes != gToldDisplayPort || (displayLanes && flipped != gToldFlipped)) {
        switch_displayport_alt_mode(displayLanes, flipped, gAlt.dock);
        gToldFlipped = flipped;
    }
    gToldDisplayPort = displayLanes;
}

#if DEVELOPMENT || DEBUG
void logIncomingVdm(const uint8_t *incoming, size_t maxLen)
{
    const size_t count = incoming[0];
    const size_t avail = count + 1 <= maxLen ? count : maxLen - 1;
    char words[96] = {};
    size_t used = 0;
    for (size_t i = 0; i + 4 <= avail && used + 1 < sizeof(words); i += 4) {
        const uint32_t word = static_cast<uint32_t>(incoming[1 + i]) |
                              (static_cast<uint32_t>(incoming[2 + i]) << 8U) |
                              (static_cast<uint32_t>(incoming[3 + i]) << 16U) |
                              (static_cast<uint32_t>(incoming[4 + i]) << 24U);
        const int n = snprintf(words + used, sizeof(words) - used, "%s%#010x", i ? " " : "", word);
        if (n < 0)
            break;
        used += static_cast<size_t>(n) < sizeof(words) - used ? static_cast<size_t>(n) : sizeof(words) - used - 1;
    }
    IOLog("SwitchPD: alt incoming VDM (%zu B): %s\n", count, words);
}
#endif

void perform(struct switch_pd_alt_out out, enum switch_pd_alt_step from)
{
    if (out.act == SWITCH_PD_ALT_READ) {
        from = gAlt.step;
        uint8_t incoming[SWITCH_PD_ALT_INCOMING_MAX] = {};
        if (tegra_i2c_read(kBus, kPD, INCOMING_VDM, incoming, sizeof(incoming)) != 0) {
            if (gAlt.step == SWITCH_PD_ALT_DRAIN_VDM) {
                out = switch_pd_alt_reply(&gAlt, incoming, sizeof(incoming));
            } else {
                out = switch_pd_alt_finish(&gAlt, SWITCH_PD_ALT_FAILED, "INCOMING_VDM could not be read");
            }
        } else {
#if DEVELOPMENT || DEBUG
            logIncomingVdm(incoming, sizeof(incoming));
#endif
            out = switch_pd_alt_reply(&gAlt, incoming, sizeof(incoming));
        }
    }
    if (out.act == SWITCH_PD_ALT_COMMAND && out.command == SWITCH_PD_ALT_CMD_DR_SWAP) {
        from = gAlt.step;
        uint8_t raw[2] = {};
        if (tegra_i2c_read(kBus, kPD, STATUS1, raw, sizeof(raw)) != 0) {
            out = switch_pd_alt_finish(&gAlt, SWITCH_PD_ALT_FAILED, "STATUS1 could not be read before the swap");
        } else {
            out = switch_pd_alt_before_swap(&gAlt, out,
                                            static_cast<uint16_t>(raw[0] | static_cast<unsigned>(raw[1]) << 8U));
        }
    }
    if (out.act == SWITCH_PD_ALT_COMMAND) {
        from = gAlt.step;
        const uint8_t command[2] = {static_cast<uint8_t>(out.command), static_cast<uint8_t>(out.command >> 8)};
        const bool written =
            (out.message_len == 0 || tegra_i2c_write(kBus, kPD, OUTGOING_VDM, out.message, out.message_len) == 0) &&
            tegra_i2c_write(kBus, kPD, COMMAND, command, sizeof(command)) == 0;
        if (!written) {
            out = switch_pd_alt_finish(&gAlt, SWITCH_PD_ALT_FAILED, "the command could not be written");
        }
    }
    if (out.line != nullptr) {
        if (gAlt.step == SWITCH_PD_ALT_FAILED && from != SWITCH_PD_ALT_FAILED) {
            IOLog("SwitchPD: alternate mode (failed): %s: %s\n", switch_pd_alt_step_name(from), out.line);
        } else {
            IOLog("SwitchPD: alternate mode (%s): %s\n", switch_pd_alt_step_name(gAlt.step), out.line);
        }
    }
#if DEVELOPMENT || DEBUG
    if (gAlt.step != from)
        IOLog("SwitchPD: alt step %s -> %s (cmd %#x timer %u ms)\n", switch_pd_alt_step_name(from),
              switch_pd_alt_step_name(gAlt.step), out.command, out.timer_ms);
#endif
    if (out.timer_ms != 0) {
        gTimer->setTimeoutMS(out.timer_ms);
    } else {
        gTimer->cancelTimeout();
    }
    hand_off();
}

void timerFired(OSObject *, IOTimerEventSource *)
{
    const enum switch_pd_alt_step before = gAlt.step;
    perform(switch_pd_alt_timer(&gAlt, gAtRest(gPD)), before);
}
}  // namespace

void switch_pd_alt_alert(IOService *pd, IOWorkLoop *loop, bool (*at_rest_of)(const void *), uint16_t alert,
                         uint16_t status1, bool port_session, bool dock_power, bool is_source)
{
    if (gTimer == nullptr) {
        switch_pd_alt_init(&gAlt, kWantDisplayPort);
        gTimer = IOTimerEventSource::timerEventSource(pd, &timerFired);
        if (gTimer == nullptr || loop->addEventSource(gTimer) != kIOReturnSuccess) {
            panic("SwitchPD: cannot add the alternate-mode timer to the PD work loop");
        }
        gPD = pd;
        gAtRest = at_rest_of;
    }
    const bool at_rest = at_rest_of(pd);
    gStatus1 = status1;
    uint16_t dp_status = 0;
    if (gAlt.step == SWITCH_PD_ALT_IDLE && port_session && at_rest) {
        uint8_t raw[2] = {};
        (void)tegra_i2c_read(kBus, kPD, DP_STATUS, raw, sizeof(raw));
        dp_status = static_cast<uint16_t>(raw[0] | static_cast<unsigned>(raw[1]) << 8U);
    }
    const struct switch_pd_alt_in in = {alert, status1, dp_status, port_session, at_rest, dock_power, is_source};
    const enum switch_pd_alt_step before = gAlt.step;
    const struct switch_pd_alt_out out = switch_pd_alt_event(&gAlt, &in);
#if DEVELOPMENT || DEBUG
    if (gAlt.step != before ||
        (alert & (SWITCH_PD_ALT_ALERT_CMD_DONE | SWITCH_PD_ALT_ALERT_VDM_RECEIVED)) != 0) {
        IOLog("SwitchPD: alt event %s -> %s (alert %#x status1 %#x dp %#x last %u)\n",
              switch_pd_alt_step_name(before), switch_pd_alt_step_name(gAlt.step), alert, status1, dp_status,
              switch_pd_alt_last_command(status1));
    }
#endif
    if (!port_session || (alert & SWITCH_PD_ALT_ALERT_PLUGPULL) != 0)
        gResumePending = false;
    if (gResumePending && at_rest && (status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) == 0 &&
        gAlt.step == SWITCH_PD_ALT_DONE) {
        switch_pd_alt_resume();
        return;
    }
    if (out.act == SWITCH_PD_ALT_NONE && out.line == nullptr && out.timer_ms == 0 && gAlt.step == before) {
        hand_off();
        return;
    }
    perform(out, before);
}

bool switch_pd_alt_partner_is_usb_host()
{
    return gAlt.partner_is_usb_host;
}

void switch_pd_alt_resume()
{
    gResumePending = false;
    if (!gAlt.dock || !gAlt.dp_lanes || gAlt.step != SWITCH_PD_ALT_DONE)
        return;
    if (!gAtRest(gPD) || (gStatus1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) != 0) {
        gResumePending = true;
        hand_off();
        return;
    }
    const enum switch_pd_alt_step before = gAlt.step;
    perform(switch_pd_alt_go(&gAlt, SWITCH_PD_ALT_DOCK_DP_SLEEP), before);
}

void switch_pd_alt_leave()
{
    if (gTimer != nullptr)
        gTimer->cancelTimeout();
    gResumePending = false;
    switch_pd_alt_init(&gAlt, kWantDisplayPort);
    hand_off();
}
