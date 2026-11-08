// SPDX-License-Identifier: BSD-2-Clause-Patent
// The IOWatchdog that lets watchdogd run. launchd starts watchdogd when an IOWatchdog registers (its LaunchEvents match
// IOProviderClass IOWatchdog), and watchdogd os_crash()es without one. watchdogd is what notices a hung SpringBoard or
// backboardd: it pings their oswatchdog endpoints every 10 s; one that has not answered for 60 s gets a stackshot
// (stacks-*.ips) and is killed, and the third time it ends in a "userspace watchdog timeout" panic, whose panic stackshot
// SwitchPanicSave.c keeps.
// watchdogd's two hard requirements on us, each an os_crash() (SIGTRAP) when unmet, and its launchd job is _PanicOnCrash:
//   - its open of this service. Its sandbox names the original classes only: r7p4's watchdogd got "Protobox:
//     watchdogd(50) deny(1) iokit-open-service NativeSwitchWatchdog", IOServiceOpen failed, it crashed, and its
//     relaunch's crash panicked. NativeSandboxGrantProcess (IOUserClient.cpp) grants watchdogd the open instead.
//   - an IOService with IOPMUPrimary = 1 (the PMU, where it records the boot stage and the foreground app):
//     NativeSwitchPMU below, published with the watchdog.
// Should watchdogd still crash, kern_exit.c takes no panic for it and calls switch_watchdogd_crashed(): userspace
// monitoring is turned off here, so its relaunch reads "disabled" and exits 0, and launchd leaves it down. One line in the
// kernel log; only a hang a running watchdogd reports panics.
// The Switch has no SoC watchdog, so enable/disable/pet have no hardware to drive. The kernel's half of the userspace
// watchdog, the original check that panics when watchdogd stops checking in, runs from one one-shot timer at the check-in
// deadline instead of AppleARMWatchdogTimer's periodic interrupt. Check-ins happen in the original user client, which has
// no hook, but they only move the deadline later (except the first, from start +600 s to now +90 s: covered by never
// arming more than 90 s out), so the timer fires at the deadline it saw and re-arms for the one it finds then: about one
// wake a minute while watchdogd checks in. Nothing is armed before watchdogd connects (a boot where launchd never starts
// it keeps running), after it crashed, or between system sleep and wake, where the deadline is paused like the SoC
// watchdog in SC7.
// thermalmonitord (CLTM) is one of watchdogd's monitored services: its health check passes only while every
// temperature sensor of the J120AP thermal model answers (drivers/SwitchPower/SwitchSensors.cpp, CONTRACT.md); an
// unhealthy thermalmonitord is killed at 60 and 120 s and ends in a "userspace watchdog timeout" panic at ~199 s (r7t,
// r7u, before the sensors existed). So watchdogd's first open of this service waits (at most kSensorWaitMS, woken as
// sensors come up and as thermalmonitord first reads each one) for switch_sensors_wait_ready(): once thermalmonitord
// has read every sensor it reads through our hook and every source answers, monitoring stays on; otherwise it is
// turned off for the boot before watchdogd asks, with one line naming what is missing.
// thermalmonitord's first read pass can come a minute after watchdogd's open on a slow boot (38 s and 90 s of uptime,
// logo at 137 s), so the wait is long. Nothing is monitored while it lasts; after the verdict Apple's timeouts apply
// (60 s silent: killed; the third time: panic).
// A late object (static initializer staged): NativeSwitchWatchdogPublish.cpp registers both classes.
#include "OriginalIOWatchdog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include "../SwitchPower/SwitchPower.h"

class NativeSwitchWatchdog;
static NativeSwitchWatchdog *gWatchdog;  // set once its start succeeded; never released

class NativeSwitchWatchdog : public IOWatchdog {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(NativeSwitchWatchdog) IOWorkLoop *loop_;
    IOTimerEventSource *timer_;
    IONotifier *power_;
    uint64_t recheck_, overdue_, leeway_;  // 90 s, 1 s, 1 s in mach_absolute_time units
    // Gate-owned (loop_):
    bool connected_;           // watchdogd opened the user client: the deadline is checked
    bool off_;                 // watchdogd crashed: userspace monitoring is off for this boot, nothing armed
    bool asleep_;              // between the system sleep and wake messages: nothing armed
    uint64_t pausedDeadline_;  // the deadline at sleep and the time it had left, 0: none paused
    uint64_t pausedLeft_;
    IOLock *verdictLock_;  // thermalmonitord's verdict, taken once at watchdogd's first open
    bool verdictTaken_;
    static constexpr unsigned kSensorWaitMS = 180000;  // the most watchdogd's first open waits for the sensors

    void arm();
    uint64_t snapshot(bool *monitoring);
    static void expired(OSObject *owner, IOTimerEventSource *timer);
    static IOReturn connectedGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn crashedGated(OSObject *owner, void *pid, void *signal, void *, void *);
    static IOReturn thermalOffGated(OSObject *owner, void *why, void *, void *, void *);
    static IOReturn sleepGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn wakeGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn powerMessage(void *target, void *refCon, UInt32 type, IOService *provider, void *argument,
                                 vm_size_t size);

public:
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;

    void enableWatchdog() override
    {}

    void disableWatchdog() override
    {}

    void petWatchdog() override
    {}

    void daemonCrashed(int pid, int signal);
};

OSDefineMetaClassAndStructors(NativeSwitchWatchdog, IOWatchdog)

    bool NativeSwitchWatchdog::start(IOService *provider)
{
    clock_interval_to_absolutetime_interval(90, kSecondScale, &recheck_);
    clock_interval_to_absolutetime_interval(1, kSecondScale, &overdue_);
    leeway_ = overdue_;
    loop_ = IOWorkLoop::workLoop();
    timer_ = IOTimerEventSource::timerEventSource(this, &NativeSwitchWatchdog::expired);
    if (!loop_ || !timer_ || loop_->addEventSource(timer_) != kIOReturnSuccess)
        return false;
    // Before it registers: watchdogd's sandbox would refuse its open of a class it does not name.
    setProperty("NativeSandboxGrantProcess", "watchdogd");
    // Registers the service (launchd starts watchdogd) and arms the first check-in deadline.
    if (!switch_original_watchdog_start(this, provider, nullptr, 0))
        return false;
    // Userspace monitoring only: no IOKit quiesce watch. Our registry has never been measured quiet within watchdogd's
    // 60 s, and that failure is a boot panic. Cleared before watchdogd can connect (launchd has yet to spawn it).
    setIOKitMonitoring(false);
    setProperty("IOKitQuiesceWatchdogEnabled", false);
    gWatchdog = this;
    verdictLock_ = IOLockAlloc();
    return true;
}

// The original user client (entitlement-checked). watchdogd's first connection starts the deadline checks. Before the
// first one, thermalmonitord's verdict (see the top of the file).
IOReturn NativeSwitchWatchdog::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                                             IOUserClient **handler)
{
    if (verdictLock_)
        IOLockLock(verdictLock_);
    if (!verdictTaken_) {
        verdictTaken_ = true;
        char why[256] = "";
        if (!switch_sensors_wait_ready(kSensorWaitMS, why, sizeof(why)))
            loop_->runAction(&NativeSwitchWatchdog::thermalOffGated, this, why);
    }
    if (verdictLock_)
        IOLockUnlock(verdictLock_);
    const IOReturn opened = IOWatchdog::newUserClient(owningTask, securityID, type, properties, handler);
    if (opened == kIOReturnSuccess)
        loop_->runAction(&NativeSwitchWatchdog::connectedGated, this);
    return opened;
}

// The monitoring flag and the deadline, as the original reads them.
uint64_t NativeSwitchWatchdog::snapshot(bool *monitoring)
{
    IOSimpleLock *lock = checkinLock();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    *monitoring = userspaceMonitoring();
    const uint64_t deadline = checkinDeadline();
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return deadline;
}

// Gated. The one timer, at the earliest time the original check can act.
void NativeSwitchWatchdog::arm()
{
    if (!connected_ || off_ || asleep_) {
        timer_->cancelTimeout();
        return;
    }
    bool monitoring = false;
    const uint64_t deadline = snapshot(&monitoring);
    const uint64_t now = mach_absolute_time();
    if (monitoring && deadline > now) {
        timer_->wakeAtTime(kIOTimeOptionsWithLeeway, deadline - now < recheck_ ? deadline : now + recheck_, leeway_);
    } else if (monitoring && IOPMRootDomainGetWillShutdown()) {
        // Past due during shutdown: the check's own later limits (91/600 s since the last check-in, 121 s after vfs
        // shutdown) need it re-run every second, as the SoC watchdog's interrupt does. Until the power-off.
        timer_->wakeAtTime(now + overdue_);
    } else {
        // Turned off by watchdogd (system sleep): its re-enable sets now +90 s without a call into us. Or past due and
        // no panic: defanged (debugger, single user). Looked at again when a check-in could have come.
        timer_->wakeAtTime(kIOTimeOptionsWithLeeway, now + recheck_, leeway_);
    }
}

void NativeSwitchWatchdog::expired(OSObject *owner, IOTimerEventSource *timer)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    switch_original_watchdog_check(self);  // panics if the deadline passed, else returns
    self->arm();
}

IOReturn NativeSwitchWatchdog::connectedGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    if (self->connected_ || self->off_)
        return kIOReturnSuccess;  // off: the relaunch reads "disabled" and exits
    self->connected_ = true;
    // The root domain's last kernel clients: watchdogd has already turned monitoring off for sleep (and back on at
    // wake) by the time these arrive.
    self->power_ = registerPrioritySleepWakeInterest(&NativeSwitchWatchdog::powerMessage, self);
    IOLog("SwitchWatchdog: watchdogd connected; userspace watchdog armed%s\n",
          self->power_ ? "" : " (no sleep notifications)");
    self->arm();
    return kIOReturnSuccess;
}

// watchdogd crashed (kern_exit.c, which took no panic for it): userspace monitoring off, as watchdogd's own disable
// (method 3) leaves it, so the original check stays quiet and watchdogd's relaunch, asking whether monitoring is enabled
// (method 0), exits 0 instead of reconnecting. Off for the rest of the boot.
IOReturn NativeSwitchWatchdog::crashedGated(OSObject *owner, void *pid, void *signal, void *, void *)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    if (self->off_)
        return kIOReturnSuccess;
    self->off_ = true;
    IOSimpleLock *lock = self->checkinLock();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    self->setUserspaceMonitoring(false);
    self->setCheckinDeadline(0);
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    self->timer_->cancelTimeout();
    IOLog("SwitchWatchdog: watchdogd[%d] crashed (signal %d): no panic for it; userspace watchdog off for this boot\n",
          static_cast<int>(reinterpret_cast<intptr_t>(pid)), static_cast<int>(reinterpret_cast<intptr_t>(signal)));
    return kIOReturnSuccess;
}

// thermalmonitord cannot report healthy this boot (a sensor it checks does not answer): userspace monitoring off before
// watchdogd's first question (method 0), as after a watchdogd crash, so watchdogd reads "disabled" and exits 0 and an
// unhealthy thermalmonitord never ends in a watchdog panic. One line with what is missing.
IOReturn NativeSwitchWatchdog::thermalOffGated(OSObject *owner, void *why, void *, void *, void *)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    if (self->off_)
        return kIOReturnSuccess;
    self->off_ = true;
    IOSimpleLock *lock = self->checkinLock();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    self->setUserspaceMonitoring(false);
    self->setCheckinDeadline(0);
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    self->timer_->cancelTimeout();
    IOLog("SwitchWatchdog: userspace watchdog off for this boot: thermalmonitord's sensors do not all answer (%s)\n",
          static_cast<const char *>(why));
    return kIOReturnSuccess;
}

void NativeSwitchWatchdog::daemonCrashed(int pid, int signal)
{
    loop_->runAction(&NativeSwitchWatchdog::crashedGated, this, reinterpret_cast<void *>(static_cast<intptr_t>(pid)),
                     reinterpret_cast<void *>(static_cast<intptr_t>(signal)));
}

// Like the SoC watchdog in SC7: the deadline keeps the time it had left (mach_absolute_time may or may not count sleep).
IOReturn NativeSwitchWatchdog::sleepGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    if (self->asleep_)
        return kIOReturnSuccess;
    self->asleep_ = true;
    self->timer_->cancelTimeout();
    bool monitoring = false;
    const uint64_t deadline = self->snapshot(&monitoring);
    const uint64_t now = mach_absolute_time();
    self->pausedDeadline_ = monitoring && deadline > now ? deadline : 0;
    self->pausedLeft_ = self->pausedDeadline_ ? deadline - now : 0;
    return kIOReturnSuccess;
}

IOReturn NativeSwitchWatchdog::wakeGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(owner);
    if (!self->asleep_)
        return kIOReturnSuccess;
    self->asleep_ = false;
    if (self->pausedDeadline_) {
        // Unless watchdogd changed it meanwhile, the deadline resumes with the time it had left.
        IOSimpleLock *lock = self->checkinLock();
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
        if (self->userspaceMonitoring() && self->checkinDeadline() == self->pausedDeadline_)
            self->setCheckinDeadline(mach_absolute_time() + self->pausedLeft_);
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        self->pausedDeadline_ = self->pausedLeft_ = 0;
    }
    self->arm();
    return kIOReturnSuccess;
}

IOReturn NativeSwitchWatchdog::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    auto *self = static_cast<NativeSwitchWatchdog *>(target);
    if (type == kIOMessageSystemWillSleep) {
        self->loop_->runAction(&NativeSwitchWatchdog::sleepGated, self);
    } else if (type == kIOMessageSystemWillNotSleep || type == kIOMessageSystemWillPowerOn ||
               type == kIOMessageSystemHasPoweredOn) {
        self->loop_->runAction(&NativeSwitchWatchdog::wakeGated, self);
    }
    return kIOReturnSuccess;  // acknowledged
}

extern "C" IOService *switch_watchdog_allocate(void)
{
    return new NativeSwitchWatchdog;
}

// kern_exit.c: watchdogd ended other than by exit(0). A process context (proc_exit), so the gate may block.
extern "C" void switch_watchdogd_crashed(int pid, int signal)
{
    NativeSwitchWatchdog *watchdog = gWatchdog;
    if (watchdog) {
        watchdog->daemonCrashed(pid, signal);
    } else {
        IOLog("SwitchWatchdog: watchdogd[%d] crashed (signal %d) with no NativeSwitchWatchdog: no panic for it\n", pid,
              signal);
    }
}

// The primary PMU watchdogd requires (IOService with IOPropertyMatch {IOPMUPrimary: 1}, else os_crash()). The Switch's
// PMIC (MAX77620) keeps no boot stage across a reset, so this only holds what watchdogd writes: IOPMUBootStage and
// IOPMUBootAppName (the foreground app), readable with ioreg. Its name is not AppleDialogSPMIPMU, which bluetoothd looks
// for; DumpPanic finds it, reads no IOPMUBootError* fault data from it, then writes IOPMUBootErrorClear as it does on
// J120's PMU: with no fault record here there is nothing to clear, so the write is taken as done.
// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class NativeSwitchPMU : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(NativeSwitchPMU)

    public : bool start(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;
};

OSDefineMetaClassAndStructors(NativeSwitchPMU, IOService)

    bool NativeSwitchPMU::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    setProperty("IOPMUPrimary", 1ULL, 32);  // an OSNumber: watchdogd matches @1, which a boolean would not equal
    // Their property writes: the sandbox profiles name J120's PMU class, not this one.
    setProperty("NativeSandboxGrantProcess", "watchdogd,DumpPanic");
    registerService();
    return true;
}

IOReturn NativeSwitchPMU::setProperties(OSObject *properties)
{
    const OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict)
        return kIOReturnBadArgument;
    static const char *const keys[] = {"IOPMUBootStage", "IOPMUBootAppName"};
    unsigned taken = dict->getObject("IOPMUBootErrorClear") ? 1 : 0;
    for (const char *key : keys) {
        OSObject *value = dict->getObject(key);
        if (!value)
            continue;
        if (!OSDynamicCast(OSNumber, value) && !OSDynamicCast(OSString, value) && !OSDynamicCast(OSData, value))
            return kIOReturnBadArgument;
        setProperty(key, value);
        ++taken;
    }
    return taken == dict->getCount() ? kIOReturnSuccess : kIOReturnUnsupported;
}

extern "C" IOService *switch_pmu_allocate(void)
{
    return new NativeSwitchPMU;
}
