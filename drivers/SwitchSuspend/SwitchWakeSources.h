// SPDX-License-Identifier: BSD-2-Clause-Patent
// What wakes the Switch OLED, as plain C on byte buffers and integers so that test/switch_wake_test drives it on the
// host: the wake sources, the Bluetooth controllers' Home button (an HID input report in an inbound HCI ACL packet), the
// wake guarantee's state machine, the power journal's ring, and the PMC wake events system sleep arms (SwitchSuspend).
// The kernel half is SwitchWake.cpp (SwitchWake.h).
//
// Sleep, as this port has it (r15):
//   * Display power (the lock): backboardd's render server asks for the panel off (lock button, auto-lock) and on
//     (SwitchBacklight). Until IOPMrootDomain sleeps (SwitchSuspend), every interrupt still reaches the CPU. A wake is
//     SpringBoard turning the panel on again because of an input event: the power key (Consumer Power), or the Home
//     button (Consumer Menu, J120's own Home button: one press wakes and goes home).
//   * The wake sources with the panel off, each of which ends in one of those two events:
//       power key        MAX77620 EN0 -> nIRQ -> INTID 118 (SwitchButtons) -> Consumer Power, as always;
//       rail Home        a seated Joy-Con (R)'s Home -> Consumer Menu on "Joy-Con System Buttons" (SwitchJoyConHID). The
//                        rails keep running with the panel off (r15: they used to pause, so a seated Joy-Con could not
//                        wake the console);
//       Bluetooth Home   a connected Joy-Con (R) or Pro Controller's Home, seen here in its HID input report on the way
//                        to bluetoothd (SwitchBluetooth) -> a Consumer Menu click on the console buttons
//                        (SwitchButtons) the moment it goes down. With the panel lit the same reports click Menu and
//                        Snapshot at their release ("Bluetooth controllers' Home and Capture" below, r17).
//       power in         a charger, dock or host plugged in (SwitchBattery: external power that is not our own boost
//                        appears) -> SpringBoard lights the screen with the charging UI itself (-[SBUIController
//                        updateBatteryState:] -> unlockUIFromSource:withOptions:, from the power-source change), as on
//                        an iPad; the wake guarantee covers it if it does not.
//   * The wake guarantee: any of them with the panel off arms a deadline (kSwitchWakeGuardMS). If the render server has
//     not asked for the panel on by then, the kernel lights it itself (one journal line, and a stackshot of every task
//     in /private/var/Switch/wake-<UTC>.kcdata), so a hung SpringBoard or backboardd shows as a frozen screen, not as
//     a Switch that will not wake, and the stackshot says where they are stuck. Holding POWER 6 s is the PMIC's own
//     power-off (hekate sets ONOFFCNFG1 MRT), whatever the kernel does.
//   * System sleep (SwitchSuspend, SC7) arms the PMC wake events in switch_wake_pmc_events.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_wake_source {
    SWITCH_WAKE_POWER_KEY = 0,  // MAX77620 EN0 (SwitchButtons)
    SWITCH_WAKE_RAIL_HOME = 1,  // a seated Joy-Con (R)'s Home (SwitchJoyConHID)
    SWITCH_WAKE_BT_HOME = 2,    // a Bluetooth Joy-Con (R) or Pro Controller's Home (SwitchBluetooth)
    SWITCH_WAKE_POWER_IN = 3,   // external power plugged in (SwitchBattery)
    SWITCH_WAKE_SOURCES = 4,
};

static inline const char *switch_wake_source_name(unsigned source)
{
    static const char *const names[SWITCH_WAKE_SOURCES] = {"power key", "rail Joy-Con Home", "Bluetooth Home",
                                                           "power in"};
    return source < SWITCH_WAKE_SOURCES ? names[source] : "unknown";
}

// --- Bluetooth controllers' Home and Capture ------------------------------------------------------------------------
// An inbound HCI ACL packet from its 4-byte header (handle and flags, length; no H4 type byte), `length` bytes of it
// (it may be cut short: only the first SWITCH_WAKE_BT_PEEK bytes are looked at). A Nintendo controller's input report
// rides one unfragmented L2CAP frame on the HID interrupt channel (a dynamic CID) behind the HIDP header 0xA1 (DATA |
// INPUT): report 0x30 (full, every 15 ms) and 0x21 (subcommand reply), 49 bytes with the ID, and 0x31 (NFC/IR), 362,
// carry the shared button byte at report offset 4 (Minus 0x01, Plus 0x02, sticks, Home 0x10, Capture 0x20); 0x3F
// (simple HID, on button changes), 12 bytes, its second button byte at offset 2 with the same bits (dekuNukem
// Nintendo_Switch_Reverse_Engineering bluetooth_hid_notes.md; the 0x3F layout is the Joy-Con's own SDP descriptor,
// 16 buttons then the hat; the 49 and 362 bytes are what SwitchJoyConHID.cpp's rail reports and descriptor carry). The
// lengths are exact so that other pads' reports with the same IDs (a DualSense's 0x31 is 78 bytes) never count.
// Returns SWITCH_BT_HOME | SWITCH_BT_CAPTURE as held in this report (0: neither), or -1 (not a Nintendo controller
// input report); *handle and *report (its ID) are set for >= 0. A Joy-Con (L) has only Capture, (R) only Home; a Pro
// Controller both.
#define SWITCH_WAKE_BT_PEEK 16u
#define SWITCH_BT_HOME      0x01u
#define SWITCH_BT_CAPTURE   0x02u

static inline int switch_wake_bt_buttons(const uint8_t *acl, size_t length, uint16_t *handle, uint8_t *report)
{
    if (!acl || length < 4)
        return -1;
    const unsigned flags = (unsigned)(acl[1] >> 4) & 3u;  // packet boundary: 1 = continuation
    const unsigned aclLength = (unsigned)(acl[2] | acl[3] << 8);
    if (flags == 1u || length < 4 + 4 + 2 || aclLength < 4 + 2)
        return -1;
    const unsigned l2capLength = (unsigned)(acl[4] | acl[5] << 8), cid = (unsigned)(acl[6] | acl[7] << 8);
    if (cid < 0x40u || l2capLength + 4u != aclLength)
        return -1;  // signalling/fixed channels, or a fragmented frame
    const uint8_t *hid = acl + 8;
    const size_t have = length - 8 < l2capLength ? length - 8 : l2capLength;
    if (hid[0] != 0xa1)
        return -1;
    const uint8_t id = hid[1];
    size_t at;
    if (id == 0x30 || id == 0x21) {
        if (l2capLength != 1 + 49)
            return -1;  // HIDP header + the standard 49-byte report
        at = 1 + 4;
    } else if (id == 0x31) {
        if (l2capLength != 1 + 362)
            return -1;  // HIDP header + the NFC/IR report
        at = 1 + 4;
    } else if (id == 0x3f) {
        if (l2capLength != 1 + 12)
            return -1;  // HIDP header + 0x3F's 12 bytes
        at = 1 + 2;
    } else {
        return -1;
    }
    if (have <= at)
        return -1;
    *handle = (uint16_t)((unsigned)(acl[0] | acl[1] << 8) & 0x0fffu);
    *report = id;
    return (int)(((hid[at] & 0x10) ? SWITCH_BT_HOME : 0u) | ((hid[at] & 0x20) ? SWITCH_BT_CAPTURE : 0u));
}

// Home and Capture of every connected Nintendo controller as the console's Home button (Consumer Menu) and screenshot
// key (Consumer Snapshot), as a rail Joy-Con's are (SwitchJoyConHID.cpp), with the panel lit or off. iPadOS's own
// handling does nothing system-wide for them: JoyConHIDServicePlugin turns either into an AC Home click that
// SpringBoard gives the focused app, and 21H461's GameController gives Nintendo devices (_GCNintendoJoyConDevice) no
// game-intent component, so GamepadHIDServiceFilter's Home gesture reaches nobody (it does for _GCDefaultPhysicalDevice
// pads). The reports still go to bluetoothd unchanged, so games keep both buttons. A press clicks the system button
// when it is released (press and release together), not while held:
//   * Home and Capture held together (any controllers) is GameController's gesture that combines two Joy-Cons, so
//     neither clicks;
//   * a press held SWITCH_BT_CLICK_LIMIT_NS or longer clicks nothing (holds belong to the controller's own gestures);
//   * with the panel off, Home clicks at once when it goes down (it wakes the console, as on Horizon) and not again at
//     its release.
// Per connection, the buttons its last report held; a table full of other handles takes the oldest entry's slot. A
// disconnection drops a connection's buttons without a click.
#define SWITCH_BT_BUTTON_LINKS   8u
#define SWITCH_BT_CLICK_LIMIT_NS 1000000000ull

struct switch_bt_buttons {
    uint16_t handle[SWITCH_BT_BUTTON_LINKS];
    uint8_t held[SWITCH_BT_BUTTON_LINKS];
    uint8_t used[SWITCH_BT_BUTTON_LINKS];
    uint32_t age[SWITCH_BT_BUTTON_LINKS];
    uint32_t clock;
    uint8_t down;     // the buttons any connection holds
    uint8_t spoiled;  // of those, the ones whose release clicks nothing
    uint64_t since[2];
};

struct switch_bt_buttons_step {
    uint8_t click;    // the system buttons to click now
    uint8_t pressed;  // the buttons that went down (any connection's first)
    bool fresh;       // the report was its connection's first
};

static inline void switch_bt_buttons_settle(struct switch_bt_buttons *b, uint64_t now, bool dark,
                                            struct switch_bt_buttons_step *step)
{
    uint8_t down = 0;
    for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS; ++i)
        if (b->used[i])
            down = (uint8_t)(down | b->held[i]);
    const uint8_t pressed = (uint8_t)(down & ~b->down), released = (uint8_t)(b->down & ~down);
    for (unsigned k = 0; k < 2; ++k) {
        const uint8_t bit = (uint8_t)(1u << k);
        if (released & bit) {
            if (!(b->spoiled & bit) && now - b->since[k] < SWITCH_BT_CLICK_LIMIT_NS)
                step->click |= bit;
            b->spoiled = (uint8_t)(b->spoiled & ~bit);
        }
        if (pressed & bit) {
            b->since[k] = now;
            b->spoiled = (uint8_t)(b->spoiled & ~bit);
            if (bit == SWITCH_BT_HOME && dark) {
                step->click |= bit;
                b->spoiled |= bit;
            }
        }
    }
    if ((down & (SWITCH_BT_HOME | SWITCH_BT_CAPTURE)) == (SWITCH_BT_HOME | SWITCH_BT_CAPTURE))
        b->spoiled |= SWITCH_BT_HOME | SWITCH_BT_CAPTURE;
    step->pressed |= pressed;
    b->down = down;
}

// One controller report (switch_wake_bt_buttons >= 0) at `now` (nanoseconds, monotonic); dark: the panel is off.
static inline struct switch_bt_buttons_step switch_bt_buttons_report(struct switch_bt_buttons *b, uint16_t handle,
                                                                     uint8_t held, uint64_t now, bool dark)
{
    struct switch_bt_buttons_step step = {0, 0, false};
    unsigned slot = SWITCH_BT_BUTTON_LINKS;
    for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS && slot == SWITCH_BT_BUTTON_LINKS; ++i)
        if (b->used[i] && b->handle[i] == handle)
            slot = i;
    if (slot == SWITCH_BT_BUTTON_LINKS) {
        for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS && slot == SWITCH_BT_BUTTON_LINKS; ++i)
            if (!b->used[i])
                slot = i;
        if (slot == SWITCH_BT_BUTTON_LINKS) {
            slot = 0;
            for (unsigned i = 1; i < SWITCH_BT_BUTTON_LINKS; ++i)
                if (b->age[i] < b->age[slot])
                    slot = i;
            b->used[slot] = 0;  // the oldest connection's buttons go, without a click
            struct switch_bt_buttons_step none = {0, 0, false};
            switch_bt_buttons_settle(b, now, dark, &none);
        }
        b->used[slot] = 1;
        b->handle[slot] = handle;
        step.fresh = true;
    }
    b->age[slot] = ++b->clock;
    b->held[slot] = (uint8_t)(held & (SWITCH_BT_HOME | SWITCH_BT_CAPTURE));
    switch_bt_buttons_settle(b, now, dark, &step);
    return step;
}

// HCI Disconnection Complete for `handle`: its buttons go without a click.
static inline void switch_bt_buttons_gone(struct switch_bt_buttons *b, uint16_t handle)
{
    for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS; ++i) {
        if (!b->used[i] || b->handle[i] != handle)
            continue;
        b->used[i] = 0;
        b->held[i] = 0;
    }
    uint8_t down = 0;
    for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS; ++i)
        if (b->used[i])
            down = (uint8_t)(down | b->held[i]);
    b->spoiled = (uint8_t)(b->spoiled & down);
    b->down = down;
}

// --- The wake guarantee --------------------------------------------------------------------------------------------
// Armed by a wake source while the panel is off; disarmed by a display-on request; fires when its deadline passes with
// the panel still off and no display-on asked for. Times in any monotonic unit.
#define SWITCH_WAKE_GUARD_MS 2500u

struct switch_wake_guard {
    uint64_t deadline;
    uint8_t source;
    bool armed;
};

// A wake source with the panel off (panelOn false): arms, keeping an earlier deadline. True when newly armed.
static inline bool switch_wake_guard_arm(struct switch_wake_guard *g, unsigned source, uint64_t now, uint64_t timeout,
                                         bool panelOn)
{
    if (panelOn || g->armed)
        return false;
    g->armed = true;
    g->source = (uint8_t)source;
    g->deadline = now + timeout;
    return true;
}

// A display-on request (the render server's, or the guard's own).
static inline void switch_wake_guard_display_on(struct switch_wake_guard *g)
{
    g->armed = false;
}

// At or after the deadline: 1 fire (the guard disarms; light the panel), 0 nothing to do (disarmed: the panel came on,
// or on was asked for), -1 not yet (still armed: look again at the deadline).
static inline int switch_wake_guard_check(struct switch_wake_guard *g, uint64_t now, bool panelOn, bool onRequested)
{
    if (!g->armed)
        return 0;
    if (panelOn || onRequested) {
        g->armed = false;
        return 0;
    }
    if (now < g->deadline)
        return -1;
    g->armed = false;
    return 1;
}

// --- The power journal's ring --------------------------------------------------------------------------------------
// The last SWITCH_POWER_LINES lines (each cut at SWITCH_POWER_LINE - 1 bytes), oldest first when rendered.
#define SWITCH_POWER_LINES 64u
#define SWITCH_POWER_LINE  192u

struct switch_power_ring {
    char line[SWITCH_POWER_LINES][SWITCH_POWER_LINE];
    uint32_t next;   // the slot the next line goes to
    uint32_t total;  // lines ever appended
};

static inline void switch_power_ring_append(struct switch_power_ring *r, const char *text)
{
    char *slot = r->line[r->next % SWITCH_POWER_LINES];
    size_t i = 0;
    for (; text[i] && text[i] != '\n' && i < SWITCH_POWER_LINE - 1; ++i)
        slot[i] = text[i];
    slot[i] = 0;
    r->next = (r->next + 1) % SWITCH_POWER_LINES;
    ++r->total;
}

// Every kept line, oldest first, one per '\n', into out (NUL-terminated, cut at capacity). Returns the length.
static inline size_t switch_power_ring_render(const struct switch_power_ring *r, char *out, size_t capacity)
{
    if (!capacity)
        return 0;
    size_t used = 0;
    const uint32_t kept = r->total < SWITCH_POWER_LINES ? r->total : SWITCH_POWER_LINES;
    const uint32_t first = (r->next + SWITCH_POWER_LINES - kept) % SWITCH_POWER_LINES;
    for (uint32_t n = 0; n < kept; ++n) {
        const char *line = r->line[(first + n) % SWITCH_POWER_LINES];
        for (size_t i = 0; line[i] && used + 1 < capacity; ++i)
            out[used++] = line[i];
        if (used + 1 < capacity)
            out[used++] = '\n';
    }
    out[used] = 0;
    return used;
}

// --- PMC wake events (system sleep, SwitchSuspend) -----------------------------------------------------------------
// The OLED DTB's nvidia,pmc-wakeup events (switchroot tegra210b01-nx-common.dtsi / -nx-pmic.dtsi). PMC_WAKE_* hold
// events 0-31, PMC_WAKE2_* events 32-63; a LVL bit set is active high. armed: what SwitchSuspend arms for SC7 always.
// Wi-Fi wake (8, PH2 WL_HOST_WAKE) and Bluetooth host wake (52, PH5 BT_HOST_WAKE, active low as the DTB's commented
// host-wakeup-gpios) are commented out there (switchroot powers the chip down in suspend); here each is armed when its
// driver has set the chip up to wake the host for this sleep.
enum {
    SWITCH_WAKE_LOW = 0,
    SWITCH_WAKE_HIGH = 1
};

struct switch_wake_pmc_event {
    uint8_t event, level, armed;
    const char *name;
};
static const struct switch_wake_pmc_event switch_wake_pmc_events[] = {
    {51, SWITCH_WAKE_LOW, 1, "pwrbtn"},   // pwr_int_n: MAX77620 nIRQ: power key EN0 (and RTC ALARM1: switch_rtc_resume)
    {14, SWITCH_WAKE_LOW, 1, "usb-c"},    // gpio_pk4: BM92T36 USB-PD ALERT (armed only while the line is high)
    {27, SWITCH_WAKE_LOW, 1, "battery"},  // gpio_py0: MAX17050 ALRT, the battery's charge-zone edges only during sleep
                                          // (switch_battery_suspend; the DTB's battery-gauge@36 nvidia,pmc-wakeup 27)
                                          // (armed only while the line is high)
    {16, SWITCH_WAKE_HIGH, 0, "soc-rtc"},  // rtc_irq: the PMC's own RTC (the MAX77620's is used)
    {35, SWITCH_WAKE_LOW, 0, "sd-cd"},     // gpio_pz1: SD card detect (both edges in the DTB)
    {52, SWITCH_WAKE_LOW, 0, "bt"},        // bt_wake_ap (PH5): SwitchBluetooth's sleep mode (switch_bluetooth_suspend)
    {8, SWITCH_WAKE_HIGH, 0, "wifi"},      // WiFi Wake Ap (PH2): SwitchWLAN's wake-on-WLAN
    {39, SWITCH_WAKE_HIGH, 0, "usb"},      // UTMIP port 0: a USB device's remote wake through the PMC's sleepwalk
                                           // (SwitchUSB XusbSleepwalk.h), armed when a device is on the host port;
                                           // after usb-c, so an unplug (its ALERT) is never taken for a key press
};
#define SWITCH_WAKE_PMC_EVENTS (sizeof(switch_wake_pmc_events) / sizeof(switch_wake_pmc_events[0]))

// The registers for a set of events (bit n of `events` = event n): mask[0]/mask[1] = PMC_WAKE_MASK/PMC_WAKE2_MASK,
// highSet/lowClear[0..1] = LVL bits to set (active high) and to clear (active low).
static inline void switch_wake_pmc_registers(uint64_t events, uint32_t mask[2], uint32_t highSet[2],
                                             uint32_t lowClear[2])
{
    mask[0] = mask[1] = highSet[0] = highSet[1] = lowClear[0] = lowClear[1] = 0;
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        const unsigned e = switch_wake_pmc_events[i].event;
        if (!(events >> e & 1u))
            continue;
        const unsigned bank = e / 32, bit = e % 32;
        mask[bank] |= 1u << bit;
        if (switch_wake_pmc_events[i].level == SWITCH_WAKE_HIGH)
            highSet[bank] |= 1u << bit;
        else
            lowClear[bank] |= 1u << bit;
    }
}

// The armed events, as a bit set; usbHigh / gaugeHigh: the USB-PD ALERT / gauge ALRT line reads high (a level-low line
// already low would wake at once, and both are stuck low when their chip misbehaves); btWake / wifiWake: the radio was
// set up to wake the host. A host-wake line already asserted is armed anyway: the radio has something for the host,
// so the console wakes at once. usbDevice: a USB device on the host port has the PMC's sleepwalk for its remote wake.
static inline uint64_t switch_wake_pmc_armed(bool usbHigh, bool gaugeHigh, bool btWake, bool wifiWake, bool usbDevice)
{
    uint64_t events = 0;
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        const unsigned e = switch_wake_pmc_events[i].event;
        const bool wanted = switch_wake_pmc_events[i].armed ? (e != 14 || usbHigh) && (e != 27 || gaugeHigh)
                            : e == 52                       ? btWake
                            : e == 8                        ? wifiWake
                            : e == 39                       ? usbDevice
                                                            : false;
        if (wanted)
            events |= (uint64_t)1 << e;
    }
    return events;
}

// The first listed event among those set in the wake status registers (status[0] events 0-31, status[1] 32-63) and
// armed, by table order; "unknown" if none.
static inline const char *switch_wake_pmc_reason(const uint32_t status[2], uint64_t armed)
{
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        const unsigned e = switch_wake_pmc_events[i].event;
        if ((armed >> e & 1u) && (status[e / 32] >> (e % 32) & 1u))
            return switch_wake_pmc_events[i].name;
    }
    return "unknown";
}

// --- Bluetooth across SC7 -------------------------------------------------------------------------------------------
// Broadcom HCI_VS 0xFC27 Set Sleep Mode (Linux hci_bcm.c struct bcm_set_sleep_mode): sleep_mode, idle_host, idle_dev
// (x 300 ms), bt_wake_active, host_wake_active (1 high, 0 low), allow_host_sleep (during SCO), combine_modes (sleep and
// LPM), tristate_control, usb_auto_sleep, usb_resume_timeout, break_to_host, pulsed_host_wake. On: UART sleep with
// both lines active low as the OLED wires them (device-wakeup PH3 and host-wakeup PH5, GPIO_ACTIVE_LOW), and a level
// host wake rather than hci_bcm's pulse, which the PMC's level-low wake event 52 needs held. Off: twelve zeros, as at
// bring-up (BT_WAKE stays asserted, host wake unused).
#define SWITCH_BT_SLEEP_PARAMS 12u

static inline void switch_bt_sleep_mode_params(uint8_t out[SWITCH_BT_SLEEP_PARAMS], bool on)
{
    for (unsigned i = 0; i < SWITCH_BT_SLEEP_PARAMS; ++i)
        out[i] = 0;
    if (!on)
        return;
    out[0] = 1;  // UART
    out[1] = 2;  // idle_host
    out[2] = 2;  // idle_dev
    out[5] = 1;  // allow_host_sleep
    out[6] = 1;  // combine_modes
}

// Horizon drops its controllers' links when it sleeps and a Joy-Con's button press pages the console back: a
// controller left connected would stream reports and wake it at once. After a wake by the Bluetooth host wake, the
// first report of a controller that connects within SWITCH_BT_RECONNECT_MS is the console's Home press (it lights
// the panel), as Horizon wakes on a reconnecting controller. Times in nanoseconds; *until 0 is closed.
#define SWITCH_BT_RECONNECT_MS 15000u

static inline void switch_bt_reconnect_open(uint64_t *until, uint64_t now)
{
    *until = now + (uint64_t)SWITCH_BT_RECONNECT_MS * 1000000u;
}

// A controller report: true when it is the reconnect the window waits for (the window closes); a window past its end
// closes without one.
static inline bool switch_bt_reconnect_take(uint64_t *until, uint64_t now, bool fresh, bool dark)
{
    if (!*until)
        return false;
    if (now >= *until) {
        *until = 0;
        return false;
    }
    if (!fresh || !dark)
        return false;
    *until = 0;
    return true;
}

// --- Time and charge asleep (SwitchSuspend's wake line) ------------------------------------------------------------
// The Tegra RTC (always-on PMC block, 32.768 kHz from the MAX77620) keeps counting through SC7, when the timebase
// mach_absolute_time() reads stops (XNU cpu_timebase_init rebases it at wake). Reading MILLI_SECONDS latches
// SHADOW_SECONDS (Linux rtc-tegra.c tegra_rtc_read_time), so one sample is (shadow seconds, milliseconds).
static inline uint64_t switch_sleep_rtc_ms(uint32_t seconds, uint32_t millis)
{
    return (uint64_t)seconds * 1000u + (millis % 1000u);
}

// Elapsed RTC milliseconds; 0 when the clock went backwards (an RTC write in between).
static inline uint64_t switch_sleep_elapsed_ms(uint64_t before, uint64_t after)
{
    return after > before ? after - before : 0;
}

// The average current over a sleep from the fuel gauge's coulomb counter: `before` and `after` are its 16-bit QH
// register (it wraps; counts down while the battery discharges), each count `lsb_uah` uAh. mA, negative while
// discharging, as the gauge's Current register; 0 for an empty interval.
static inline int32_t switch_sleep_average_ma(uint16_t before, uint16_t after, uint32_t lsb_uah, uint64_t elapsed_ms)
{
    if (!elapsed_ms)
        return 0;
    const int64_t counts = (int16_t)(uint16_t)(after - before);
    return (int32_t)(counts * (int64_t)lsb_uah * 3600 / (int64_t)elapsed_ms);
}

#ifdef __cplusplus
}
#endif
