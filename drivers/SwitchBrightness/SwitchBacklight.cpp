// SPDX-License-Identifier: BSD-2-Clause-Patent
// The built-in display's backlight for iPadOS 17.7 CoreBrightness, and the display's power, driving the Switch OLED panel
// through TegraPanel (DCS commands; there is no PWM backlight). Boot-args in SwitchBrightness.h.
//
// What CoreBrightness expects (21H461; addresses in its dyld cache image and the kernelcache):
//   * -[BLControl findBacklight] / -[CBDisplayContaineriOS findBacklight] look up a registered service with
//     backlight-control = true; handleDisplayArrival: makes the built-in display module (CBDisplayModuleiOS) only if that
//     service conforms to "AppleARMBacklight" (IOObjectConformsTo, i.e. metaCast by class name). An IOHIDDevice gets the
//     external-display module instead.
//   * The module reads IODisplayParameters.brightness {min, max, value} (32-bit integers) and fails without it. Apple's
//     AppleARMBacklight publishes brightness 0..65536 (value 32768 at start), rawBrightness 0..2047, BrightnessMicroAmps
//     and BrightnessMilliNits. thermalmonitord reads brightness and BrightnessMilliNits {value, max} from the backlight
//     ("Failed to find nit params" without the latter).
//   * DisplayCreateWithID also reads backlight-calibration-parameters, the panel's luminance range in nits (16.16 fixed
//     point; publishCalibration below). Without it the display's physical range is the slider's 0..1, read as nits:
//     every nits value CoreBrightness computes is clamped to 1 ("Lmax=1.000000 Lmin=0.000000"), and
//     AABC::_UpdateEsensorTrusted, which bounds the lux it acts on by the lux at which its curve reaches the panel's
//     maximum, caps it at about 8 lux.
//   * Without that range it writes with IORegistryEntrySetCFProperties(service, dict): "brightness" = ceil(fraction *
//     (max - min)) + min; "FadePeriod" = seconds * 65536 to ramp over; sometimes "backlight-level" (not used here). With
//     it, every change (the user's slider and auto-brightness) comes as "brightness-nits", nits * 65536, which
//     CoreBrightness steps itself (_SetBLDriverNitsValue from ___DisplaySetLogicalBrightnessInternal), and sometimes
//     "brightness-nits-cap". Without brightness-nits the panel stays at its start level while auto-brightness ramps, so
//     both are taken (SwitchBacklightMath.h). A failed write makes it drop that update. It also sets
//     IOMFBBrightnessLevel on the framebuffer unless the backlight carries ignore-IOMFB.
//   * DisplayCreateWithID returns no display in five places, and CoreBrightness then has no display module at all
//     ("failed to create DisplayRef", "Modules not ready" for every property after it): no CF object (0x1b62acdf4), no
//     IOObjectRetain of the service (0x1b62acf04), no IODisplayParameters (0x1b62acf74), no backlight-marketing-table
//     while new-backlight-architecture is set (0x1b62ae660..0x1b62ae678; the legacy architecture takes its built-in
//     three points instead), no properties dictionary (0x1b62aedac). The third and fourth are this service's, so both are
//     published before backlight-control, and backlight-control only with them (start()). The display matters beyond
//     brightness: libEDR's server (com.apple.libEDR.Notification, in backboardd) is started by __DisplayCheckEDRSupport
//     (0x1b62b2214), near the end of DisplayCreateWithID, and nowhere else, while Core Animation's render server opens a
//     libEDR client whenever a service has backlight-control (CADeviceHasBacklight's IOPropertyMatch: CA::EDRClient::client
//     calls EDRCreateClientForDisplayWithTimeoutAndCallback only then, 0x1896ba85c..0x1896ba8d0) and waits up to 5 s on
//     each message to that port. With new-backlight-architecture but no table there is no display and no server while
//     launchd holds the port, so the render server composites one frame per 5 s ("(ipc/send) timed out"; SpringBoard
//     hangs, touch looks dead).
// The original class lives in com.apple.driver.AppleARMPlatform, which is prelinked here (a dependency of
// IOMobileGraphicsFamily), so the name cannot be defined again (duplicate classes panic in OSMetaClass::postModLoad).
// SwitchBacklight is a plain IOService whose metaclass names the original AppleARMBacklight metaclass as its superclass
// (bound in original-bindings.S): metaCast and matching treat it as a subclass, and no AppleARMBacklight code ever runs on
// it. postModLoad only follows the superclass chain to count its depth, and the original metaclass is still zeroed
// storage when the kernel's own classes register, so this is not a late object. Kernel code that casts to
// AppleARMBacklight and touches its fields would break; in this kernelcache that is AppleARMPlatform itself, AppleSPU,
// AppleEmbeddedLightSensor and AppleM68Buttons, none of which is started here.
//
// Display power. On lock (power button, auto-lock) the render server in backboardd calls
// IOMobileFramebufferRequestPowerChange(fb, 0) (QuartzCore, through IOMobileFramebuffer's selector 12) and on wake (fb, 1).
// The original family only acts on that when a platform subclass has marked itself power capable, and then it only flips
// its own powered word; the hardware is the platform's. Here the framebuffer hands the request to
// switch_display_power_request(), and this work loop plays it: off = TegraPanel's L4T suspend sequence while the DC still
// scans out, then the display stops the DC (no fetches, no frame-end or vblank interrupts, so the cores sit in WFI); on =
// the display restarts the DC, the ambient light sensor samples from then on, and TegraPanel wakes the panel and
// restores the brightness. The touch controller goes down with the screen and comes back up while the panel wakes
// (SwitchTouch's display power). The rail Joy-Cons keep running, so a seated Joy-Con's Home can wake the console
// (../SwitchSuspend/SwitchWakeSources.h). On by default; boot-arg switch-no-sleep leaves the render server's requests to
// the original user client (the panel stays lit when locked).
//
// Waking (../SwitchSuspend/SwitchWake.h): every display off and on is one power-journal line with each step's time, and
// the journal goes to Data after every display off. A wake source with the panel off (power key, a Joy-Con's Home)
// arms the wake guarantee: if the render server has not asked for the panel on within SWITCH_WAKE_GUARD_MS, this loop
// lights it as if it had (one journal line and a stackshot of every task), so a hung SpringBoard or backboardd shows as
// a frozen screen instead of a Switch that does not wake, and says where it is stuck.
// QuartzCore ignores the request's return value (IOMFBDisplay::update_power_state_locked, 21H461), so answering it here
// changes nothing on its side.
//
// Requests (CoreBrightness's setProperties, the framebuffer's power requests) never wait for this work loop: they are
// left under a lock and the work timer is kicked (IOTimerEventSource::setTimeout takes no gate). The framebuffer's
// request comes from its own work loop, and this loop calls back into that one (native_legacy_display_set_scanout) and
// waits for frame ends it delivers, so waiting here would deadlock. The panel's 0x51 value (0..2047, hekate calls it
// candela) is taken as linear in luminance, so brightness maps linearly onto kFloorRaw..2047: the slider's bottom (and a
// brightness of 0) is dim but never dark, as on an iPad; only display power turns the panel off. A panel command takes
// four frames and blocks this loop, so at most one is ever out; a fade or a slider drag is played as a sequence of
// commands, at most one every three frames (50 ms), each toward the latest request: requests that arrive meanwhile only
// replace the target. The panel's own dimming (DCS 0x53 bit 3) smooths the steps between them.
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "TegraPanel.h"
#include "SwitchBacklightMath.h"
#include "SwitchBrightness.h"
#include "../SwitchSuspend/SwitchWake.h"
#include "../TegraGM20B/TegraGM20BSleep.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"

// The original AppleARMBacklight::gMetaClass (original-bindings.S), declared with a concrete type so that its address is
// a constant initializer. Nothing here constructs or calls it.
// NOLINTNEXTLINE(misc-use-internal-linkage): in an anonymous namespace it would make gOriginalAppleARMBacklightMeta internal
class OriginalBacklightMetaClass : public OSMetaClass {
public:
    OSObject *alloc() const override;
};

extern const OriginalBacklightMetaClass gOriginalAppleARMBacklightMeta __asm__("__ZN17AppleARMBacklight10gMetaClassE");

// The display driver's strong definition wins when it is linked; without it the panel still goes dark but the DC keeps
// scanning out.
extern "C" __attribute__((weak)) bool native_legacy_display_set_scanout(bool)
{
    return false;
}

// SwitchTouch's strong definition wins when it is linked (SwitchTouch.h).
extern "C" __attribute__((weak)) void switch_touch_display_power(bool)
{}

// SwitchMotion's likewise (SwitchMotion.h): its orientation detector runs only while the panel is lit.
extern "C" __attribute__((weak)) void switch_motion_display_power(bool)
{}

namespace {
using namespace switch_backlight;
static_assert(kPanelMax == TEGRA_PANEL_BRIGHTNESS_MAX, "SwitchBacklightMath.h's panel range");
constexpr uint64_t kLongestFadeNS = 10000000000ULL;
constexpr uint64_t kSendIntervalNS = 50000000;  // three frames at 60 Hz between panel commands
constexpr uint32_t kQuietMS = 1000;             // no request for this long: the change is over (its log line)

volatile bool gDisplayOn = true;
volatile bool gSawNits, gSawNitsCap, gSawDisplay;  // the first of each is logged once

OSDictionary *range(uint32_t max, uint32_t value)
{
    OSDictionary *dict = OSDictionary::withCapacity(3);

    const struct {
        const char *key;
        uint32_t value;
    } entries[] = {{"min", 0}, {"max", max}, {"value", value}};

    for (const auto &entry : entries) {
        const OSNumber *number = OSNumber::withNumber(entry.value, 32);
        if (dict && number)
            dict->setObject(entry.key, number);
        OSSafeReleaseNULL(number);
    }
    return dict;
}

// backlight-calibration-parameters (21H461 DisplayCreateWithID, 0x1b62ad1e4..0x1b62ad4e8): nine CFNumbers read as float and
// divided by 65536, each without a null check (a missing key would crash backboardd in CFNumberGetValue), and an
// optional tenth. lMaxPanel and lMinPanel become the physical maximum and minimum (_DisplayGetBrightnessMaxPhysical
// reads display+0x1d0 = lMaxPanel, ...MinPhysical display+0x18c = lMinPanel); the product values are its marketing range.
// The Switch OLED's luminance has not been measured: the maximum and middle are CoreBrightness's own for an
// uncalibrated display (its fallback when the dictionary is absent stores 400, 140 and 5 nits before scaling them by the
// slider table it does not have here), and the minimum is the panel's floor on that scale, 12.5 nits
// (SwitchBacklightMath.h), so that CoreBrightness never asks for less than the panel shows. The currents only feed its
// power model; they are the nits themselves, so every current ratio is the luminance ratio and milliAmps2NitsScaleFactor
// is left to its default, lMaxPanel. All values nits * 65536.

OSDictionary *calibration()
{
    const struct {
        const char *key;
        uint32_t value;
    } entries[] = {
        {"current-for-max-backlight", kNitsMax},
        {"current-for-mid-backlight", kNitsMid},
        {"hardware-max-current-limit", kNitsMax},
        {"lMaxProduct", kNitsMax},
        {"lMidProduct", kNitsMid},
        {"lMinProduct", kNitsMin},
        {"lMaxPanel", kNitsMax},
        {"lMidPanel", kNitsMid},
        {"lMinPanel", kNitsMin},
    };

    OSDictionary *dict = OSDictionary::withCapacity(sizeof(entries) / sizeof(entries[0]));
    for (const auto &entry : entries) {
        const OSNumber *number = OSNumber::withNumber(entry.value, 32);
        if (!dict || !number || !dict->setObject(entry.key, number)) {
            OSSafeReleaseNULL(number);
            OSSafeReleaseNULL(dict);
            return nullptr;
        }
        number->release();
    }
    return dict;
}

[[maybe_unused]] uint64_t msBetween(uint64_t from, uint64_t to)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(to - from, &ns);
    return ns / 1000000;
}

[[maybe_unused]] uint64_t msSince(uint64_t start)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return ns / 1000000;
}
}  // namespace

// Display power is on unless switch-no-sleep (system sleep needs it too). NativeLegacyDisplay, the display probe and
// SwitchSuspend ask the same.
extern "C" bool switch_display_power_enabled(void)
{
    unsigned off = 0;
    PE_parse_boot_argn("switch-no-sleep", &off, sizeof(off));
    return off == 0;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchBacklight : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchBacklight) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *work = nullptr;
    IOTimerEventSource *quiet = nullptr;
    IOTimerEventSource *guardTimer = nullptr;  // the wake guarantee's deadline
    IOLock *requestLock = nullptr;

    // Requests, under requestLock.
    bool haveRequest = false, wantOn = true;
    uint32_t requestValue = 0;
    uint64_t requestFadeNS = 0;
    unsigned nitsRequests = 0, brightnessRequests = 0;  // since the last settle line
    switch_wake_guard guard = {};
    uint64_t guardTimeout = 0;  // SWITCH_WAKE_GUARD_MS in mach_absolute_time units

    // Work loop state.
    uint32_t current = 0, from = 0, target = 0;     // brightness, 0..kMax
    uint32_t lit = kMax / 2;                        // the last nonzero target, restored on wake
    uint64_t rampStart = 0, rampLength = 0;         // mach_absolute_time units
    uint64_t lastSend = 0, sendInterval = 0;        // mach_absolute_time units
    unsigned settleNits = 0, settleBrightness = 0;  // requests taken since the last settle line
    unsigned sentRaw = ~0u;
    bool panelOn = true, scanoutStopped = false, reportedFailure = false;

    static void workFired(OSObject *owner, IOTimerEventSource *sender);
    static void guardFired(OSObject *owner, IOTimerEventSource *sender);

    void kick()
    {
        work->setTimeoutUS(1);
    }

    void startRamp(uint32_t value, uint64_t fadeNS);
    void step();
    void powerOff();
    void powerOn();
    bool send(unsigned raw);
    void publishParameters();
    void publishWhitePointType();
    static void quietFired(OSObject *owner, IOTimerEventSource *sender);

public:
    bool sleepEnabled = false;
    bool start(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *properties) override;
    void requestPower(bool on);
    void expectWake(unsigned source);
};

// OSDefineMetaClassAndStructors(SwitchBacklight, IOService), except that the metaclass's superclass is the original
// AppleARMBacklight metaclass (see above). The C++ base class stays IOService.
SwitchBacklight::MetaClass SwitchBacklight::gMetaClass;
const OSMetaClass *const SwitchBacklight::metaClass = &SwitchBacklight::gMetaClass;
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init): only its address, a link-time constant
const OSMetaClass *const SwitchBacklight::superClass = &gOriginalAppleARMBacklightMeta;

SwitchBacklight::SwitchBacklight(const OSMetaClass *meta) : IOService(meta)
{}

SwitchBacklight::~SwitchBacklight() = default;

const OSMetaClass *SwitchBacklight::getMetaClass() const
{
    return &gMetaClass;
}

OSMetaClassConstructorInit(SwitchBacklight, IOService, ) OSDefineDefaultStructors(SwitchBacklight, IOService)

    static SwitchBacklight *volatile gBacklight;

void SwitchBacklight::publishParameters()
{
    OSDictionary *parameters = OSDictionary::withCapacity(3);
    const OSDictionary *brightness = range(kMax, current);
    const OSDictionary *raw = range(TEGRA_PANEL_BRIGHTNESS_MAX, toRaw(current));
    const OSDictionary *milliNits = range(kMilliNitsMax, rawToMilliNits(toRaw(current)));
    if (parameters && brightness && raw && milliNits) {
        parameters->setObject("brightness", brightness);
        parameters->setObject("rawBrightness", raw);
        parameters->setObject("BrightnessMilliNits", milliNits);
        setProperty("IODisplayParameters", parameters);
    }
    setProperty("brightness-nits", rawToNits(toRaw(current)), 32);  // what the panel shows, nits * 65536
    OSSafeReleaseNULL(milliNits);
    OSSafeReleaseNULL(raw);
    OSSafeReleaseNULL(brightness);
    OSSafeReleaseNULL(parameters);
}

bool SwitchBacklight::send(unsigned raw)
{
    if (raw == sentRaw)
        return true;
    const int err = tegra_panel_set_brightness(raw);
    if (err) {
        if (!reportedFailure)
            IOLog("SwitchBacklight: panel brightness %u failed (%d)\n", raw, err);
        reportedFailure = true;
        return false;
    }
    reportedFailure = false;
    sentRaw = raw;
    return true;
}

// Work loop.
void SwitchBacklight::startRamp(uint32_t value, uint64_t fadeNS)
{
    from = current;
    target = value > kMax ? kMax : value;
    if (target)
        lit = target;
    rampStart = mach_absolute_time();
    nanoseconds_to_absolutetime(fadeNS > kLongestFadeNS ? kLongestFadeNS : fadeNS, &rampLength);
}

// Work loop. The DC still scans out here; it is stopped only once the panel is asleep.
void SwitchBacklight::powerOff()
{
    const uint64_t start = mach_absolute_time();
    // The ALS first: it shares LDO6 with the touch panel, which turns it off, and its 2 s frame-end deadline must be
    // disarmed before the scanout stops.
    switch_als_set_active(false);
    const uint64_t als = mach_absolute_time();
    switch_touch_display_power(false);
    switch_motion_display_power(false);  // stores and kicks its work loop, never waits
    const uint64_t touch = mach_absolute_time();
    const int err = tegra_panel_power_off();
    if (err)
        IOLog("SwitchBacklight: panel off failed (%d): the display is not running\n", err);
    const uint64_t panel = mach_absolute_time();
    scanoutStopped = native_legacy_display_set_scanout(false);
    tegra_gm20b_display_power(false);
    panelOn = false;
    sentRaw = ~0u;
    switch_power_journal(
        "SwitchBacklight: display off in %llu ms (als %llu, touch %llu, panel %llu %s, scanout %llu %s)",
        msSince(start), msBetween(start, als), msBetween(als, touch), msBetween(touch, panel),
        err ? "not running" : "asleep", msSince(panel), scanoutStopped ? "stopped" : "still running");
    switch_power_journal_flush(false);  // to Data now, while nothing is wrong yet
}

// Work loop. TegraPanel's first step waits for a frame end of the restarted DC, and panics if none comes.
void SwitchBacklight::powerOn()
{
    const uint64_t start = mach_absolute_time();
    tegra_gm20b_display_power(true);   // the GPU's rail comes up on its own thread call while the panel wakes
    switch_touch_display_power(true);  // the controller comes up on its own work loop while the panel wakes
    const uint64_t touch = mach_absolute_time();
    if (scanoutStopped && !native_legacy_display_set_scanout(true)) {
        panic("SwitchBacklight: the display did not restart DC A's scanout for wake");
    }
    scanoutStopped = false;
    // The ALS samples through the panel's half-second wake, so CoreBrightness's first sample comes within its 250 ms
    // display-on timeout; without one it takes 450 lux (AABC::FirstALSSampleTimeout, 141 nits on 11-06).
    switch_als_set_active(true);
    const uint64_t scanout = mach_absolute_time();
    const int level = static_cast<int>(toRaw(target ? target : lit));
    const int err = tegra_panel_power_on(level);
    if (err)
        IOLog("SwitchBacklight: panel on failed (%d): the display is not running\n", err);
    const uint64_t panel = mach_absolute_time();
    panelOn = true;
    sentRaw = err || level < 0 ? ~0u : static_cast<unsigned>(level);
    if (target == 0 && level >= 0)
        target = current = lit;
    gDisplayOn = true;
    switch_motion_display_power(true);
    switch_power_journal("SwitchBacklight: display on in %llu ms (touch %llu, scanout and als %llu, panel %llu %s; "
                         "level %d)",
                         msSince(start), msBetween(start, touch), msBetween(touch, scanout), msBetween(scanout, panel),
                         err ? "not running" : "awake", level);
}

// Work loop: the latest requests, display power first, then one step of the current ramp. Re-arms itself until the
// ramp reaches its target; a panel command blocks this loop for four frames.
void SwitchBacklight::step()
{
    IOLockLock(requestLock);
    const bool on = wantOn;
    const bool fresh = haveRequest;
    const uint32_t value = requestValue;
    const uint64_t fadeNS = requestFadeNS;
    haveRequest = false;
    settleNits += nitsRequests;
    settleBrightness += brightnessRequests;
    nitsRequests = brightnessRequests = 0;
    IOLockUnlock(requestLock);

    if (fresh)
        startRamp(value, fadeNS);
    if (on != panelOn) {
        if (on) {
            powerOn();
        } else {
            powerOff();
        }
    }
    gDisplayOn = panelOn;  // also when a request to go dark was taken back before it ran
    // Dark: the request is only recorded, and played on wake.
    if (!panelOn) {
        current = target;
        publishParameters();
        return;
    }
    uint32_t next = target;
    const uint64_t elapsed = mach_absolute_time() - rampStart;
    if (rampLength && elapsed < rampLength) {
        next = static_cast<uint32_t>(static_cast<int64_t>(from) +
                                     (static_cast<int64_t>(target) - static_cast<int64_t>(from)) *
                                         static_cast<int64_t>(elapsed) / static_cast<int64_t>(rampLength));
    }
    const unsigned raw = toRaw(next);
    if (raw != sentRaw) {
        const uint64_t now = mach_absolute_time();
        if (lastSend && now - lastSend < sendInterval) {
            work->wakeAtTime(lastSend + sendInterval);  // the latest target is taken then
            return;
        }
        lastSend = now;
    }
    if (!send(raw))
        next = target;  // give up on this ramp, keep the request
    current = next;
    if (current != target) {
        work->setTimeoutMS(1);
    } else {
        publishParameters();
        if (settleNits || settleBrightness)
            quiet->setTimeoutMS(kQuietMS);  // the line once the requests stop
    }
}

// Work loop, kQuietMS after the panel last reached a requested level: one line per change, to the log only.
// CoreBrightness's own ramps arrive as many brightness-nits steps (auto-brightness and the slider alike), so this is
// one line per ramp, with the level it ended at.
void SwitchBacklight::quietFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = OSDynamicCast(SwitchBacklight, owner);
    if (!self || (!self->settleNits && !self->settleBrightness))
        return;
    const unsigned raw = toRaw(self->current);
    const uint32_t nits = rawToNits(raw);
    os_log(OS_LOG_DEFAULT,
           "SwitchBacklight: panel %u (%u.%02u nits) after %u brightness-nits and %u brightness requests", raw,
           nits >> 16, ((nits & 0xffff) * 100) >> 16, self->settleNits, self->settleBrightness);
    self->settleNits = self->settleBrightness = 0;
}

void SwitchBacklight::workFired(OSObject *owner, IOTimerEventSource *)
{
    if (auto *self = OSDynamicCast(SwitchBacklight, owner))
        self->step();
}

// Work loop, at the wake guarantee's deadline: with the panel still off and no display-on asked for, light it as the
// render server would have (wantOn), then play it.
void SwitchBacklight::guardFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchBacklight, owner);
    if (!self)
        return;
    IOLockLock(self->requestLock);
    const unsigned source = self->guard.source;
    const int verdict = switch_wake_guard_check(&self->guard, mach_absolute_time(), self->panelOn, self->wantOn);
    if (verdict == 1)
        self->wantOn = true;
    IOLockUnlock(self->requestLock);
    if (verdict < 0) {
        sender->setTimeoutMS(50);  // a timer that came early: look again
        return;
    }
    if (verdict == 0)
        return;
    switch_power_event("SwitchBacklight: %s with the panel off and no display-on request from the render server in "
                       "%u ms: the kernel lights the panel (SpringBoard or backboardd not answering?)",
                       switch_wake_source_name(source), SWITCH_WAKE_GUARD_MS);
    switch_wake_stackshot(switch_wake_source_name(source));
    self->step();
}

IOReturn SwitchBacklight::setProperties(OSObject *properties)
{
    const OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict)
        return kIOReturnBadArgument;
    // DisplayBrightnessNitsNVRAM (lMidPanel, nits * 65536): DisplayCreateWithID's first write here under the new
    // architecture (0x1b62b0f44, straight through IORegistryEntrySetCFProperty while display+0x78 is still NULL), past all
    // five of its failures and just before __DisplayCheckEDRSupport starts libEDR's server, so the first one says
    // CoreBrightness has its display (logged once). Nothing else is done with it.
    if (const OSNumber *nvram = OSDynamicCast(OSNumber, dict->getObject("DisplayBrightnessNitsNVRAM"))) {
        if (!__atomic_exchange_n(&gSawDisplay, true, __ATOMIC_RELAXED)) {
            IOLog("SwitchBacklight: CoreBrightness made its display (new backlight architecture, NVRAM %u nits): "
                  "libEDR's server starts next\n",
                  nvram->unsigned32BitValue() >> 16);
        }
    }
    // brightness-nits-cap: CoreBrightness's ceiling for EDR headroom; its SDR targets already respect its caps, so it is
    // only recorded (the first one logged).
    if (OSNumber *cap = OSDynamicCast(OSNumber, dict->getObject("brightness-nits-cap"))) {
        setProperty("brightness-nits-cap", cap);
        if (!__atomic_exchange_n(&gSawNitsCap, true, __ATOMIC_RELAXED)) {
            IOLog("SwitchBacklight: first brightness-nits-cap %u nits (recorded only)\n",
                  cap->unsigned32BitValue() >> 16);
        }
    }
    const OSNumber *nits = OSDynamicCast(OSNumber, dict->getObject("brightness-nits"));
    const OSNumber *brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"));
    if (!nits && !brightness)
        return kIOReturnSuccess;  // FadePeriod / backlight-level alone: nothing to do
    uint32_t value = 0;
    uint64_t fadeNS = 0;
    if (nits) {
        // nits * 65536, a CFNumber SInt32; CoreBrightness steps its own ramps, so no fade here.
        const int32_t requested = static_cast<int32_t>(nits->unsigned32BitValue());
        value = nitsToBrightness(requested);
        if (!__atomic_exchange_n(&gSawNits, true, __ATOMIC_RELAXED)) {
            // NOLINTBEGIN(bugprone-signed-bitwise): signed 16.16 nits, as CoreBrightness sends them
            IOLog("SwitchBacklight: CoreBrightness sets brightness-nits (first %d.%02d nits, panel %u)\n",
                  requested >> 16, (((requested & 0xffff) * 100) >> 16), toRaw(value));
            // NOLINTEND(bugprone-signed-bitwise)
        }
    } else {
        const OSNumber *fade = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));
        value = brightness->unsigned32BitValue();
        fadeNS = fade ? fade->unsigned64BitValue() * 1000000000ULL / 65536 : 0;
    }
    IOLockLock(requestLock);
    requestValue = value;
    requestFadeNS = fadeNS;
    ++*(nits ? &nitsRequests : &brightnessRequests);
    haveRequest = true;
    IOLockUnlock(requestLock);
    kick();
    return kIOReturnSuccess;
}

void SwitchBacklight::requestPower(bool on)
{
    if (!on)
        gDisplayOn = false;  // input is ignored from the request on, not only once the panel is dark
    IOLockLock(requestLock);
    wantOn = on;
    if (on)
        switch_wake_guard_display_on(&guard);
    IOLockUnlock(requestLock);
    kick();
}

// Any thread context (a wake source with the panel off, SwitchWake.cpp). Never blocks on this loop.
void SwitchBacklight::expectWake(unsigned source)
{
    IOLockLock(requestLock);
    const bool armed = switch_wake_guard_arm(&guard, source, mach_absolute_time(), guardTimeout, gDisplayOn || wantOn);
    IOLockUnlock(requestLock);
    if (armed) {
        guardTimer->setTimeoutMS(SWITCH_WAKE_GUARD_MS);
        tegra_gm20b_display_expected();
    }
}

// The IPSW's iPad's /backlight default-whitepoint-type, which adt_edit.py carries in /chosen: CoreBrightness takes its
// white point reference from it (DisplayCreateWithCapabilitiesAndID and -[CBColorModuleShared setWhitePointType] read
// the backlight service and its parents), and without it falls back to its legacy 7059 K white.
void SwitchBacklight::publishWhitePointType()
{
    const IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    uint32_t type = 0;
    if (chosen && switch_dt_value(chosen, "switch-whitepoint-type", &type))
        setProperty("default-whitepoint-type", &type, sizeof(type));
    OSSafeReleaseNULL(chosen);
}

bool SwitchBacklight::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    nanoseconds_to_absolutetime(kSendIntervalNS, &sendInterval);
    sleepEnabled = switch_display_power_enabled();
    nanoseconds_to_absolutetime(SWITCH_WAKE_GUARD_MS * 1000000ULL, &guardTimeout);
    requestLock = IOLockAlloc();
    loop = IOWorkLoop::workLoop();
    work = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBacklight::workFired) : nullptr;
    quiet = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBacklight::quietFired) : nullptr;
    guardTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBacklight::guardFired) : nullptr;
    if (!requestLock || !work || !quiet || !guardTimer || loop->addEventSource(work) != kIOReturnSuccess ||
        loop->addEventSource(quiet) != kIOReturnSuccess || loop->addEventSource(guardTimer) != kIOReturnSuccess) {
        IOLog("SwitchBacklight: work loop setup failed\n");
        return false;
    }
    // Start from what the boot chain left on the panel; nothing is sent until CoreBrightness asks.
    const int boot = tegra_panel_boot_brightness();
    current = target = boot >= 0 ? toBrightness(static_cast<unsigned>(boot)) : kMax / 2;
    if (current)
        lit = current;
    setName("backlight");
    // Everything DisplayCreateWithID needs from this service first, backlight-control last and only with it (see the
    // top): a backlight that CoreBrightness cannot make a display of leaves libEDR's server down while Core Animation's
    // render server waits on it. No backlight-control instead: neither sees a backlight, and the render server makes no
    // libEDR client.
    publishParameters();
    publishWhitePointType();
    // CoreBrightness reads ignore-IOMFB (CFData, nonzero) from the backlight and its parents and then clears its use-IOMFB
    // flag (display module init, 0x1b62b55fc): the framebuffer here is a stand-in, so it should not get
    // IOMFBBrightnessLevel writes.
    uint32_t ignore = 1;
    setProperty("ignore-IOMFB", &ignore, sizeof(ignore));
    // All nine keys or none: a partial dictionary would crash backboardd (calibration()).
    if (OSDictionary *parameters = calibration()) {
        setProperty("backlight-calibration-parameters", parameters);
        parameters->release();
    } else {
        IOLog("SwitchBacklight: no memory for backlight-calibration-parameters: auto-brightness stays at 0..1 nits\n");
    }
    // new-backlight-architecture = true: DisplayCreateWithID (0x1b62ad074..0x1b62ad1bc) then marks the display new
    // (display+0x20) instead of starting its legacy brightness thread (display+0x24). Every brightness change, its own
    // fades included, then goes through ___DisplaySetLogicalBrightnessInternal as brightness-nits: the fade callback
    // (___DisplayFadeCallback, 0x1b61b424c..0x1b61b4258) takes the legacy path only while display+0x24 is set. Without
    // it each fade step goes to ___DisplaySetLogicalBrightnessLegacy and never reaches the panel: only a fade's unfaded
    // first step and the slider, which set the logical brightness directly, do. The new architecture fails the display
    // without backlight-marketing-table (0x1b62ae660), so the two go together.
    OSData *table = OSData::withBytes(kMarketingTable, sizeof(kMarketingTable));
    const bool marketing = (table != nullptr) && setProperty("backlight-marketing-table", table);
    OSSafeReleaseNULL(table);
    if (marketing) {
        setProperty("new-backlight-architecture", kOSBooleanTrue);
    } else {
        IOLog("SwitchBacklight: no memory for backlight-marketing-table: legacy backlight architecture (faded "
              "auto-brightness steps do not reach the panel)\n");
    }
    if (OSDynamicCast(OSDictionary, getProperty("IODisplayParameters"))) {
        setProperty("backlight-control", kOSBooleanTrue);
    } else {
        IOLog("SwitchBacklight: no memory for IODisplayParameters: no backlight-control, CoreBrightness and Core "
              "Animation see no backlight (brightness stays as booted; display power still works)\n");
    }
    // backboardd sets FadePeriod/brightness properties on this service; its sandbox only allows that on the original
    // class, so our XNU grants it by name (IOUserClient.cpp native_sandbox_grant).
    setProperty("NativeSandboxGrantProcess", "backboardd");
    registerService();
    IOLog("SwitchBacklight: started (display power %s)\n", sleepEnabled ? "on" : "off");
    return true;
}

void SwitchBacklight::free()
{
    if (work) {
        work->cancelTimeout();
        if (loop)
            loop->removeEventSource(work);
        work->release();
    }
    if (quiet) {
        quiet->cancelTimeout();
        if (loop)
            loop->removeEventSource(quiet);
        quiet->release();
    }
    if (guardTimer) {
        guardTimer->cancelTimeout();
        if (loop)
            loop->removeEventSource(guardTimer);
        guardTimer->release();
    }
    OSSafeReleaseNULL(loop);
    if (requestLock)
        IOLockFree(requestLock);
    IOService::free();
}

extern "C" int switch_backlight_start(void)
{
    if (gBacklight)
        return 0;
    const OSSymbol *name = OSSymbol::withCStringNoCopy("AppleARMBacklight");
    const OSMetaClass *original = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (original != static_cast<const OSMetaClass *>(&gOriginalAppleARMBacklightMeta)) {
        IOLog("SwitchBacklight: AppleARMBacklight is %p, binding %p (AppleARMPlatform not loaded?)\n", original,
              &gOriginalAppleARMBacklightMeta);
        return -1;
    }
    IOService *resources = IOService::getResourceService();
    SwitchBacklight *candidate = OSTypeAlloc(SwitchBacklight);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchBacklight: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    __atomic_store_n(&gBacklight, candidate, __ATOMIC_RELEASE);
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): gBacklight keeps the started backlight for good
    return 0;
}

extern "C" bool switch_display_power_request(bool on)
{
    SwitchBacklight *backlight = __atomic_load_n(&gBacklight, __ATOMIC_ACQUIRE);
    if (!backlight || !backlight->sleepEnabled)
        return false;
    backlight->requestPower(on);
    return true;
}

extern "C" bool switch_display_is_on(void)
{
    return gDisplayOn;
}

extern "C" void switch_backlight_expect_wake(unsigned source)
{
    SwitchBacklight *backlight = __atomic_load_n(&gBacklight, __ATOMIC_ACQUIRE);
    if (backlight && backlight->sleepEnabled)
        backlight->expectWake(source);
}
