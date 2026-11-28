// SPDX-License-Identifier: GPL-2.0-only
// AUX transactions, hotplug and scanout run from hardware interrupts; protocol deadlines use one-shot timers.
#include "SwitchDisplayPort.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>
#include <libkern/OSKextLib.h>
#include <mach/mach_time.h>

#include "../ArmGIC/SwitchInterrupts.h"
#include "SwitchDisplayPortService.h"

OSDefineMetaClassAndStructors(SwitchDisplayPort, IOService);

extern "C" void switch_displayport_audio_sink(const uint8_t *, unsigned, bool);
extern "C" bool switch_displayport_audio_eld(const uint8_t *, unsigned);
extern "C" OSReturn switch_kernel_classes(void (*initialize)());
extern "C" void switch_external_screen_static_init(void) __asm__("__GLOBAL__sub_I_SwitchExternalScreen.cpp");


namespace {
constexpr uint32_t kAuxLostMs = 50;  // DPAUX1 times out a silent sink itself within a millisecond
SwitchDisplayPort *gService;
bool gScreenClassRegistered;
thread_call_t gApply;
struct alignas(8) Attachment {
    uint32_t serial;
    uint16_t lanes;
    bool flipped, dock;
};
static_assert(sizeof(Attachment) == sizeof(uint64_t));
uint32_t gSerial;
Attachment gPublished;

void applyAltMode(thread_call_param_t, thread_call_param_t)
{
    if (gService == nullptr && switch_displayport_start())
        return;
    gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            Attachment state;
            __atomic_load(&gPublished, &state, __ATOMIC_ACQUIRE);
            static_cast<SwitchDisplayPort *>(owner)->setAltLanes(state.lanes, state.flipped, state.dock, state.serial);
            return kIOReturnSuccess;
        },
        gService);
}
}  // namespace

extern "C" int switch_displayport_start(void)
{
    if (gService)
        return 0;
    auto *service = OSTypeAlloc(SwitchDisplayPort);
    if (!service || !service->init() || !service->attach(IOService::getResourceService()) || !service->begin()) {
        OSSafeReleaseNULL(service);
        return -1;
    }
    gService = service;
    return 0;
}

extern "C" void switch_displayport_alt_mode(unsigned lanes, bool flipped, bool dock)
{
    // A detach and reattach can coalesce before the display loop runs, including two identical assignments.
    Attachment state{++gSerial, static_cast<uint16_t>(lanes), flipped, dock};
    __atomic_store(&gPublished, &state, __ATOMIC_RELEASE);
    if (!gApply)
        gApply = thread_call_allocate(applyAltMode, nullptr);
    thread_call_enter(gApply);
}

void SwitchDisplayPort::releaseResources()
{
    if (loop) {
        if (cecTimer)
            loop->removeEventSource(cecTimer);
        if (stepTimer)
            loop->removeEventSource(stepTimer);
        if (auxTimer)
            loop->removeEventSource(auxTimer);
        if (dcSource)
            loop->removeEventSource(dcSource);
        if (auxSource)
            loop->removeEventSource(auxSource);
        if (gate)
            loop->removeEventSource(gate);
    }
    OSSafeReleaseNULL(cecTimer);
    OSSafeReleaseNULL(stepTimer);
    OSSafeReleaseNULL(auxTimer);
    OSSafeReleaseNULL(dcSource);
    OSSafeReleaseNULL(auxSource);
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(screen);
    OSSafeReleaseNULL(loop);
}

void SwitchDisplayPort::stop(IOService *provider)
{
    if (loop) {
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                auto *self = static_cast<SwitchDisplayPort *>(owner);
                self->powerOff();
                if (self->screen)
                    self->screen->detach();
                return kIOReturnSuccess;
            },
            this);
    }
    releaseResources();
    IOService::stop(provider);
}

void SwitchDisplayPort::free()
{
    releaseResources();
    IOService::free();
}

void SwitchDisplayPort::wakeModeWaiters(bool abort)
{
    if (abort)
        ++modeGeneration;
    if (gate)
        gate->commandWakeup(&trainingEvent, false);
}

bool SwitchDisplayPort::begin()
{
    if (!gScreenClassRegistered) {
        if (OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily") != KERN_SUCCESS)
            return false;
        if (switch_kernel_classes(switch_external_screen_static_init) != kOSReturnSuccess)
            return false;
        gScreenClassRegistered = true;
    }
    screen = OSTypeAlloc(SwitchExternalScreen);
    if (!screen || !screen->init()) {
        OSSafeReleaseNULL(screen);
        return false;
    }
    loop = screen->getWorkLoop();
    if (!loop) {
        OSSafeReleaseNULL(screen);
        return false;
    }
    loop->retain();
    gate = IOCommandGate::commandGate(this);
    IOService *auxNub = switch_interrupt_nub(TEGRA_DPAUX1_INTID, false);
    IOService *dcNub = switch_interrupt_nub(TEGRA_DISPLAY_B_INTID, false);
    auxSource = auxNub ? IOInterruptEventSource::interruptEventSource(this, &auxInterrupt, auxNub, 0) : nullptr;
    dcSource = dcNub
                   ? IOFilterInterruptEventSource::filterInterruptEventSource(this, &dcInterrupt, &dcFilter, dcNub, 0)
                   : nullptr;
    auxTimer = IOTimerEventSource::timerEventSource(this, &auxTimerFired);
    stepTimer = IOTimerEventSource::timerEventSource(this, &stepTimerFired);
    cecTimer = IOTimerEventSource::timerEventSource(this, &cecTimerFired);
    if (gate == nullptr || auxSource == nullptr || dcSource == nullptr || auxTimer == nullptr || stepTimer == nullptr ||
        cecTimer == nullptr || loop->addEventSource(gate) != kIOReturnSuccess ||
        loop->addEventSource(auxSource) != kIOReturnSuccess || loop->addEventSource(dcSource) != kIOReturnSuccess ||
        loop->addEventSource(auxTimer) != kIOReturnSuccess || loop->addEventSource(stepTimer) != kIOReturnSuccess ||
        loop->addEventSource(cecTimer) != kIOReturnSuccess) {
        panic("SwitchDisplayPort: cannot attach the DPAUX1 (INTID %u) or display controller B (INTID %u) interrupt",
              TEGRA_DPAUX1_INTID, TEGRA_DISPLAY_B_INTID);
    }
    if (!screen->begin(this, loop)) {
        releaseResources();
        return false;
    }
    setName("SwitchDisplayPort");
    registerService();
    return true;
}

void SwitchDisplayPort::setAltLanes(unsigned lanes, bool plugFlipped, bool dock, uint32_t attachment)
{
    if (lanes == altLanes && plugFlipped == flipped && attachment == activeAttachment) {
        return;
    }
    const bool replaced = altLanes && attachment != activeAttachment;
    activeAttachment = attachment;
    if (lanes == 0 || replaced) {
        IOLog("SwitchDisplayPort: DisplayPort alternate mode ended\n");
        // Alternate-mode exit can remove AUX before this notification, so a remote CEC disable is no longer reliable.
        powerOff();
        screen->detach();
        wantedMode = -1;
        altLanes = 0;
        if (lanes == 0)
            return;
    }
    if (stage != Stage::Off) {
        cecRelease();
        powerOff();
    }
    altLanes = lanes;
    flipped = plugFlipped;
    nintendoDock = dock;
    if (sleeping)
        return;
    stage = stage == Stage::Off ? Stage::Off : Stage::Waiting;
    char why[128] = "";
    if (stage == Stage::Off) {
        if (!aux.powerUp(why, sizeof(why))) {
            fail("DPAUX1", why);
            return;
        }
        auxSource->enable();
        stage = Stage::Waiting;
    }
    IOLog("SwitchDisplayPort: DisplayPort on %u lanes (pin assignment %c, plug %s), HPD %s\n", lanes,
          lanes == 2 ? 'D' : 'C', flipped ? "flipped" : "straight", aux.hpdHigh() ? "high" : "low");
    cecReset();
    if (nintendoDock)
        switch_dp_cec_start(&cec);
    if (aux.hpdHigh())
        plugged();
    scheduleAux();
}

void SwitchDisplayPort::auxInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t cause = self->aux.takeInterrupt();
    const bool completed = (cause & SWITCH_DPAUX_INTR_AUX_DONE) != 0 && self->auxPending;
    const bool unplug = (cause & SWITCH_DPAUX_INTR_UNPLUG_EVENT) != 0;
    if (unplug) {
        self->auxDiscard = self->auxPending;
        self->unplugged();
    }
    if (completed) {
        self->auxTimer->cancelTimeout();
        self->auxPending = false;
        self->auxDone();
    }
    if ((cause & SWITCH_DPAUX_INTR_PLUG_EVENT) != 0 && (!unplug || self->aux.hpdHigh())) {
        self->plugged();
    }
    if ((cause & SWITCH_DPAUX_INTR_IRQ_EVENT) != 0) {
        self->irqHpd();
    }
}

// Primary interrupt context: clear and read back DC_CMD_INT_STATUS so the level line drops before EOI, then wake the
// work loop only on FRAME_END without GIC mask/unmask per frame.
bool SwitchDisplayPort::dcFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (!__atomic_load_n(&self->dcbUp, __ATOMIC_ACQUIRE))
        return false;
    const uint32_t cause = self->dcb.takeInterrupt();
    if (!(cause & (1u << 1)))
        return false;
    __atomic_store_n(&self->dcFeTime, mach_absolute_time(), __ATOMIC_RELAXED);
    __atomic_store_n(&self->dcFeContinuous, mach_continuous_time(), __ATOMIC_RELAXED);
    __atomic_store_n(&self->dcFeTaken, __atomic_load_n(&self->dcFeTaken, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
    source->signalInterrupt();
    return false;
}

void SwitchDisplayPort::dcInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t taken = __atomic_load_n(&self->dcFeTaken, __ATOMIC_ACQUIRE);
    if (taken == self->dcFeHandled)
        return;
    self->dcFeHandled = taken;
    if (!self->dcbUp)
        return;
    const uint64_t stamp = __atomic_load_n(&self->dcFeTime, __ATOMIC_RELAXED);
    const uint64_t continuous = __atomic_load_n(&self->dcFeContinuous, __ATOMIC_RELAXED);
    (void)self->screen->frameEnd(stamp, continuous);
}

void SwitchDisplayPort::auxStart(const struct switch_dp_aux_request &transaction, Next then)
{
    request = transaction;
    next = then;
    attempt = 0;
    auxIssue();
}

void SwitchDisplayPort::auxNative(enum switch_dp_aux_op op, uint32_t address, const uint8_t *bytes, uint8_t size,
                                  Next then)
{
    struct switch_dp_aux_request native = switch_dp_aux_make(op, address, size);
    for (uint8_t i = 0; bytes != nullptr && i < size; ++i) {
        native.data[i] = bytes[i];
    }
    auxStart(native, then);
}

void SwitchDisplayPort::auxIssue()
{
    auxRetrying = false;
    auxPending = true;
    aux.start(request);
    switch_check_after_ms(auxTimer, kAuxLostMs);
}

void SwitchDisplayPort::auxDone()
{
    const enum switch_dp_aux_result result = aux.finish(request);
    if (auxDiscard || (sleeping && next != Next::Cec)) {
        auxDiscard = false;
        scheduleAux();
        return;
    }
    if (result == SWITCH_DP_AUX_ACK) {
        dispatch();
        scheduleAux();
        return;
    }
    const uint32_t retryUs = switch_dp_aux_retry_us(result, attempt++);
    if (retryUs != 0) {
        auxRetrying = true;
        auxTimer->setTimeoutUS(retryUs);
        return;
    }
    if (next == Next::Cec) {
        cecComplete(false);
        scheduleAux();
        return;
    }
    static const char *const names[] = {"ack", "NACK", "defer", "I2C NACK", "I2C defer", "timeout", "error", "short"};
    char why[96];
    snprintf(why, sizeof(why), "AUX %s at %#x after %u tries (AUXSTAT %#x)", names[result], request.address, attempt,
             aux.auxstat());
    fail("the sink", why);
}

void SwitchDisplayPort::auxTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->auxRetrying) {
        self->auxIssue();
        return;
    }
    if (!self->auxPending) {
        return;
    }
    char gic[192];
    switch_interrupt_describe(TEGRA_DPAUX1_INTID, gic, sizeof(gic));
    const uint32_t auxstat = self->aux.auxstat();
    IOLog("SwitchDisplayPort: DPAUX1 interrupt lost: no AUX_DONE %u ms after a transaction "
          "(AUXCTL %#x, AUXSTAT %#x); %s\n",
          kAuxLostMs, switch_dp_auxctl(&self->request), auxstat, gic);
    self->auxPending = false;
    self->auxDiscard = false;
    self->aux.resetAux();
    if (self->next == Next::Cec) {
        self->cecComplete(false);
        self->scheduleAux();
        return;
    }
    char why[96];
    snprintf(why, sizeof(why), "AUX_DONE timeout after %u ms (AUXSTAT %#x)", kAuxLostMs, auxstat);
    self->fail("DPAUX1", why);
}

void SwitchDisplayPort::stepTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->stage == Stage::Training) {
        self->auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_LANE0_1_STATUS, nullptr, self->train.tps3 ? 11 : 6,
                        Next::TrainingStatus);
    }
}

void SwitchDisplayPort::fail(const char *what, const char *why)
{
    IOLog("SwitchDisplayPort: %s: %s\n", what, why);
    const bool probed = stage == Stage::Training || stage == Stage::Running;
    teardownLink();
    if (stage != Stage::Off) {
        stage = probed ? Stage::Ready : Stage::Waiting;
    }
    wakeModeWaiters(true);
    if (probed)
        screen->modeFailed();
}

void SwitchDisplayPort::teardownLink()
{
    if (next != Next::Cec) {
        if (auxPending) {
            auxDiscard = true;
        } else {
            auxTimer->cancelTimeout();
            auxRetrying = false;
        }
    }
    stepTimer->cancelTimeout();
    modePending = false;
    if (sorUp) {
        sor.audioEnable(false);
        sor.audioPresence(nullptr, 0, false);
        switch_displayport_audio_sink(nullptr, 0, false);
    }
    const bool wasDcbUp = dcbUp;
    if (wasDcbUp) {
        dcSource->disable();
        __atomic_store_n(&dcbUp, false, __ATOMIC_RELEASE);
        dcb.windowsOff();
    }
    if (sorUp)
        sor.detachHead();
    if (wasDcbUp) {
        dcb.enableSor(false);
    }
    // DCB's DISP2 clock is muxed to PLL_D2; power DCB down before SOR1 disables PLL_D2.
    if (wasDcbUp) {
        dcb.powerDown();
    }
    if (sorUp) {
        sor.powerDown();
        sorUp = false;
    }
    screen->linkDown();
}

void SwitchDisplayPort::powerOff()
{
    cecReset();
    auxTimer->cancelTimeout();
    auxPending = auxRetrying = auxDiscard = false;
    probePending = modePending = irqPending = false;
    teardownLink();
    if (stage != Stage::Off) {
        auxSource->disable();
        aux.powerDown();
    }
    stage = Stage::Off;
    wakeModeWaiters(true);
}

void SwitchDisplayPort::plugged()
{
    if (stage != Stage::Waiting) {
        return;
    }
    cecReleasePending = cecRestartPending = nintendoDock;
    probePending = true;
    scheduleAux();
}

void SwitchDisplayPort::unplugged()
{
    if (stage == Stage::Off) {
        return;
    }
    IOLog("SwitchDisplayPort: the sink went away (HPD low)\n");
    teardownLink();
    screen->detach();
    wantedMode = -1;
    probePending = modePending = irqPending = false;
    cec.physical = SWITCH_DP_CEC_INVALID_PHYSICAL;
    cecReleasePending = true;
    cecRestartPending = false;
    stage = Stage::Waiting;
    wakeModeWaiters(true);
    scheduleAux();
}

void SwitchDisplayPort::irqHpd()
{
    if (stage == Stage::Running || cec.enabled) {
        irqPending = true;
        scheduleAux();
    }
}

IOReturn SwitchDisplayPort::selectMode(int index, bool enable)
{
    if (!gate)
        return kIOReturnNotReady;
    struct Request {
        int index;
        bool enable;
    } req = {index, enable};
    return gate->runAction(
        [](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchDisplayPort *>(owner);
            const auto &r = *static_cast<Request *>(argument);
            if (!r.enable) {
                if (self->stage == Stage::Running || self->stage == Stage::Training) {
                    self->teardownLink();
                    self->stage = Stage::Ready;
                }
                self->modePending = false;
                self->wakeModeWaiters(true);
                self->scheduleAux();
                return kIOReturnSuccess;
            }
            if (self->sleeping || r.index < 0 || static_cast<unsigned>(r.index) >= self->modes.count ||
                (self->stage != Stage::Ready && self->stage != Stage::Running && self->stage != Stage::Training))
                return kIOReturnNotReady;
            if (self->stage == Stage::Running && self->wantedMode == r.index && !self->modePending)
                return kIOReturnSuccess;
            if (self->stage != Stage::Ready)
                self->teardownLink();
            self->stage = Stage::Ready;
            self->wakeModeWaiters(true);
            const uint64_t myGen = self->modeGeneration;
            self->wantedMode = r.index;
            self->modePending = true;
            self->scheduleAux();
            uint64_t deadline = 0;
            clock_interval_to_deadline(2500, kMillisecondScale, &deadline);
            while (myGen == self->modeGeneration && (self->modePending || self->stage == Stage::Training)) {
                const IOReturn wait = self->gate->commandSleep(&self->trainingEvent, deadline, THREAD_UNINT);
                if (wait == THREAD_TIMED_OUT || wait == THREAD_INTERRUPTED || wait == THREAD_RESTART) {
                    if (myGen == self->modeGeneration && (self->modePending || self->stage == Stage::Training)) {
                        self->teardownLink();
                        if (self->stage != Stage::Off)
                            self->stage = Stage::Ready;
                        self->wakeModeWaiters(true);
                        self->screen->modeFailed();
                    }
                    return wait == THREAD_TIMED_OUT ? kIOReturnNotReady : kIOReturnAborted;
                }
            }
            if (myGen == self->modeGeneration && self->stage == Stage::Running && self->wantedMode == r.index)
                return kIOReturnSuccess;
            return myGen != self->modeGeneration ? kIOReturnAborted : kIOReturnNotReady;
        },
        &req);
}

bool SwitchDisplayPort::gpuComplete(void *display, int status)
{
    if (display != screen)
        return false;
    screen->gpuComplete(status);
    return true;
}

extern "C" bool switch_displayport_gpu_complete(void *display, int status)
{
    return gService && gService->gpuComplete(display, status);
}

void SwitchDisplayPort::systemSleep(bool entering)
{
    if (entering == sleeping)
        return;
    sleeping = entering;
    if (entering) {
        cecRelease();
        powerOff();
        screen->detach();
        wantedMode = -1;
        return;
    }
    Attachment state;
    __atomic_load(&gPublished, &state, __ATOMIC_ACQUIRE);
    altLanes = 0;
    if (state.lanes)
        setAltLanes(state.lanes, state.flipped, state.dock, state.serial);
}

namespace {
void setSleeping(bool entering)
{
    if (!gService)
        return;
    gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *entering, void *, void *, void *) -> IOReturn {
            static_cast<SwitchDisplayPort *>(owner)->systemSleep(entering != nullptr);
            return kIOReturnSuccess;
        }, gService, entering ? gService : nullptr);
}
}

extern "C" void switch_displayport_suspend(void) { setSleeping(true); }
extern "C" void switch_displayport_resume(void) { setSleeping(false); }

bool SwitchDisplayPort::audioFormat(uint32_t rate, uint32_t channels, uint32_t bits)
{
    return stage == Stage::Running && sor.audioFormat(rate, channels, bits);
}

void SwitchDisplayPort::audioEnable(bool enable)
{
    if (stage == Stage::Running)
        sor.audioEnable(enable);
}

void SwitchDisplayPort::audioPresence(const uint8_t *eld, uint32_t bytes, bool present)
{
    if (stage == Stage::Running)
        sor.audioPresence(eld, bytes, present);
}

extern "C" bool switch_displayport_audio_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (!gService)
        return false;
    const uint32_t format[] = {rate, channels, bits};
    return gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *value, void *, void *, void *) -> IOReturn {
            const auto *format = static_cast<const uint32_t *>(value);
            return static_cast<SwitchDisplayPort *>(owner)->audioFormat(format[0], format[1], format[2])
                       ? kIOReturnSuccess : kIOReturnUnsupported;
        }, gService, const_cast<uint32_t *>(format)) == kIOReturnSuccess;
}

extern "C" void switch_displayport_audio_enable(bool enable)
{
    if (gService)
        gService->getWorkLoop()->runAction(
            [](OSObject *owner, void *enabled, void *, void *, void *) -> IOReturn {
                static_cast<SwitchDisplayPort *>(owner)->audioEnable(enabled != nullptr);
                return kIOReturnSuccess;
            }, gService, enable ? gService : nullptr);
}

extern "C" void switch_displayport_audio_presence(const uint8_t *eld, uint32_t bytes, bool present)
{
    if (gService)
        gService->getWorkLoop()->runAction(
            [](OSObject *owner, void *eld, void *bytes, void *present, void *) -> IOReturn {
                static_cast<SwitchDisplayPort *>(owner)->audioPresence(static_cast<const uint8_t *>(eld),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bytes)), present != nullptr);
                return kIOReturnSuccess;
            }, gService, const_cast<uint8_t *>(eld), reinterpret_cast<void *>(static_cast<uintptr_t>(bytes)),
            present ? gService : nullptr);
}

bool SwitchDisplayPort::audioPreparePresence()
{
    return stage == Stage::Running && !sleeping && switch_displayport_audio_eld(edid, validEdidBlocks);
}

extern "C" bool switch_displayport_audio_prepare_presence(void)
{
    return gService && gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            return static_cast<SwitchDisplayPort *>(owner)->audioPreparePresence()
                       ? kIOReturnSuccess : kIOReturnNotReady;
        }, gService) == kIOReturnSuccess;
}
