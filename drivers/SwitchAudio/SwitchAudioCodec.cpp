// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the RT5639 on its own work loop, the jack, the headphone level and the stats line.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

rt5639::Route SwitchAudio::wantedRoute() const
{
    return jackIn && !speakerOnly ? rt5639::kRouteHeadphones : rt5639::kRouteSpeaker;
}

// A stream starts: the codec powered (bias, DAC, capture path) and routed to where the jack says, still muted.
void SwitchAudio::codecPowerUp()
{
    idleTimer->cancelTimeout();
#if DEVELOPMENT || DEBUG
    statsResume();
#endif
    tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");  // before trusting the jack state
    if (!codecOn) {
        codec.biasStandby();
        codec.powerDac();
        codecOn = true;
    }
    const rt5639::Route want = wantedRoute();
    if (codec.route() != want) {
        codec.routeDown();
        codec.routeUp(want);
    }
}

// Route, DAC and references off, outputs muted first (rt5640.c's PRE_PMD steps, then bias OFF).
// The headset microphone's path (BST1, the REC mixers, the ADC, MICBIAS1 for a headset) only while input runs.
void SwitchAudio::captureCodec(bool on)
{
    if (on && tegraCapture && codecOn && !captureCodecOn) {
        codec.micBias(headset);
        codec.powerCapture();
        captureCodecOn = true;
    } else if (!on && captureCodecOn) {
        codec.powerCaptureOff();
        codec.micBias(false);
        captureCodecOn = false;
    }
}

void SwitchAudio::codecPowerDown()
{
    if (codecOn)
        codec.routeDown();
    if (codec.biasIsOn())
        codec.biasOff();
    codecOn = false;
    captureCodecOn = false;
}

IOReturn SwitchAudio::codecGated(OSObject *owner, void *step, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->codecOff)
        return kIOReturnSuccess;
    switch (reinterpret_cast<uintptr_t>(step)) {
    case kCodecPowerUp:
        self->codecPowerUp();
        break;
    case kCodecUnmute:
        if (self->control->state)
            self->codec.mute(false);
        break;
    case kCodecMute:
        self->codec.mute(true);
        break;
    case kCodecPause:
        self->codec.mute(true);
        self->idleTimer->setTimeoutMS(kIdleMS);
        break;
    case kCodecIdleNow:
        self->idleTimer->cancelTimeout();
        codecIdle(self, self->idleTimer);
        break;
    case kCodecHeadphoneLevel:
        self->codec.setHeadphoneAttenuation(__atomic_load_n(&self->headphoneSteps, __ATOMIC_RELAXED));
        if (self->control->state)
            self->codec.mute(false);
        break;
    case kCodecNoMicrophone:
        self->captureCodec(false);
        self->codec.micBias(false);
        self->publishJack();
        break;
    case kCodecCaptureOn:
        self->captureCodec(true);
        break;
    case kCodecCaptureOff:
        self->captureCodec(false);
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}

void SwitchAudio::codecStep(CodecStep step)
{
    codecGate->runAction(&SwitchAudio::codecGated, reinterpret_cast<void *>(step));
}

void SwitchAudio::codecIdle(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->control->state || self->codecOff || !self->codec.biasIsOn())
        return;
    tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");
    self->codecPowerDown();
}

// PBB4, both edges (rt5640_irq: IRQF_TRIGGER_RISING | FALLING): wait for the contacts to settle.
void SwitchAudio::jackEdge(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchAudio *>(owner)->jackTimer->setTimeoutMS(kJackSettleMS);
}

// Codec work loop, after the jack settled: whether the plug carries a microphone (rt5640_detect_headset needs the
// references, which come up for it and go down kIdleMS later), then the jack's state to the plug-in.
void SwitchAudio::noteJack()
{
    if (tegraCapture && jackIn && !codec.biasIsOn()) {
        codec.biasStandby();
        if (!control->state)
            idleTimer->setTimeoutMS(kIdleMS);
    }
    headset = tegraCapture && codec.detectHeadset(jackIn);
    if (!captureCodecOn || !headset)
        codec.micBias(false);
    publishJack();
}

void SwitchAudio::publishJack()
{
    const uint32_t jack =
        (jackIn ? SwitchAudioJackHeadphones : 0u) | (headset && tegraCapture ? SwitchAudioJackMicrophone : 0u);
    if (control->jack == jack)
        return;
    control->jack = jack;
    messageClients(kIOMessageServicePropertyChange);
}

// Also the first reading, kJackSettleMS after codecStart; until then the jack counts as empty (the speaker).
void SwitchAudio::jackSettled(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const uint16_t status = SwitchRT5639::read(rt5639::INT_IRQ_ST);
    const bool in = SwitchRT5639::answering() && SwitchRT5639::jackPlugged(status);
    const bool first = !self->jackKnown;
    if (first) {
        self->jackKnown = true;
        IOLog("SwitchAudio: headphones %s at start\n", in ? "in" : "out");
    } else if (in != self->jackIn) {
        IOLog("SwitchAudio: headphones %s\n", in ? "plugged in" : "unplugged");
    } else {
        return;
    }
    self->jackIn = in;
    // The references come up here once with every output off, and go down kIdleMS later as after a stream. At
    // boot the VREF capacitor starts from 0 V: charged by the first stream instead, its 15 ms of fast charge left
    // the reference still rising when the class-D amplifier came on, the loud click of the first sound after
    // boot. Later power-ups start from a charged capacitor.
    if (first && !self->codec.biasIsOn()) {
        self->codec.biasStandby();
        self->idleTimer->setTimeoutMS(kIdleMS);
    }
    self->noteJack();
    if (!self->codecOn)
        return;  // the next stream routes
    if (self->codec.route() == self->wantedRoute())
        return;
    self->codec.routeDown();
    self->codec.routeUp(self->wantedRoute());
    if (self->control->state)
        self->codec.mute(false);
}

#if DEVELOPMENT || DEBUG
// The stats line, every kStatsMS on the codec work loop (it reads the codec over I2C). "starts" counts the
// streams the hardware started since the last line, "moved" what the playback DMA played (240000 a line while a
// stream runs), "under" the periods that played before the plug-in had written their end, "peak" the loudest sample
// since the last line (16-bit units; 0: only silence reached the DMA); "io" the plug-in's playback IO cycles in the
// window and the frames each wrote (the audio server's wake-ups and IO buffer size), "lead" the least write-ahead
// the plug-in had at a period's end (frames; under 0 is an underrun), "lag" the period interrupts' least and most
// lateness against the I2S clock model in us (see notePlayed; "-" without a period); then, as read now, ADMA ch0
// STATUS and completed periods, I2S1 enable and RX (playback) status, ADMAIF1 TX status, the jack (INT_IRQ_ST,
// bit 4 set = empty; PV6, 1 = empty) and route, and PWR_DIG1, PWR_ANLG1, PWR_MIXER, PWR_VOL, SPK_VOL, HP_VOL,
// DAC1_DIG_VOL.
// The line runs while there is audio to report: the first window after boot, then from a stream's start
// (codecPowerUp arms it) until a window with no stream, no start, nothing moved and the codec powered down; that
// window reads nothing, writes no line and leaves the timer off, so a silent console takes no stats wakeups.
void SwitchAudio::statsFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const uint64_t frames = self->statFrames;
    const uint64_t moved = frames - self->statFramesShown;
    const uint32_t starts = self->statStarts - self->statStartsShown;
    if (self->statsShown && !self->control->state && !self->codecOn && !starts && !moved) {
        self->statsArmed = false;
        return;
    }
    self->statsShown = true;
    self->statFramesShown = frames;
    self->statStartsShown += starts;
    const int peak = self->statPeak;
    self->statPeak = 0;
    const uint64_t ioCycles = self->control->io_cycles;
    const uint64_t ioFrames = self->control->io_frames;
    const uint64_t cycles = ioCycles - self->statIoCyclesShown;
    const uint64_t written = ioFrames - self->statIoFramesShown;
    self->statIoCyclesShown = ioCycles;
    self->statIoFramesShown = ioFrames;
    char lead[24] = "-";
    char lag[48] = "-";
    if (self->statLeadMin != INT64_MAX)
        snprintf(lead, sizeof(lead), "%lld", static_cast<long long>(self->statLeadMin));
    if (self->statLagMax != INT64_MIN) {
        snprintf(lag, sizeof(lag), "%lld..%lld", static_cast<long long>(self->statLagMin),
                 static_cast<long long>(self->statLagMax));
    }
    self->statLeadMin = INT64_MAX;
    self->statLagMin = INT64_MAX;
    self->statLagMax = INT64_MIN;
    static const char *const routes[] = {"none", "speaker", "headphones"};
    char line[416];
    snprintf(
        line, sizeof(line),
        "SwitchAudio stats: run %u starts %u moved %llu under %u peak %d io %llu x %llu f lead %s lag %s us | ADMA "
        "%#x xfer %u, I2S en %u rx %#x, ADMAIF tx %#x | jack %s st %#x pv6 %d, %s | codec %04x %04x %04x %04x vol "
        "%04x %04x dac %04x",
        self->control->state, starts, moved, self->statUnderruns, peak, cycles, cycles ? written / cycles : 0, lead,
        lag, switch_mmio_read32(self->adma, CH_STATUS), switch_mmio_read32(self->adma, CH_XFER_STATUS) & 0xffff,
        switch_mmio_read32(self->ahub, I2S_ENABLE), switch_mmio_read32(self->ahub, I2S_RX_STATUS),
        switch_mmio_read32(self->ahub, ADMAIF_TX_STATUS), self->jackIn ? "in" : "out",
        SwitchRT5639::read(rt5639::INT_IRQ_ST), tegra_gpio_read(kJackDetect), routes[self->codec.route()],
        SwitchRT5639::read(rt5639::PWR_DIG1), SwitchRT5639::read(rt5639::PWR_ANLG1),
        SwitchRT5639::read(rt5639::PWR_MIXER), SwitchRT5639::read(rt5639::PWR_VOL), SwitchRT5639::read(rt5639::SPK_VOL),
        SwitchRT5639::read(rt5639::HP_VOL), SwitchRT5639::read(rt5639::DAC1_DIG_VOL));
    os_log(OS_LOG_DEFAULT, "%s", line);  // log only, never the boot console
    sender->setTimeoutMS(kStatsMS);
}
#endif

#if DEVELOPMENT || DEBUG
// Codec work loop: a stream starts; the stats line resumes if it had stopped (statsFired).
void SwitchAudio::statsResume()
{
    if (!statsTimer || statsArmed)
        return;
    statsArmed = true;
    statsTimer->setTimeoutMS(kStatsMS);
}
#endif

void SwitchAudio::codecStart()
{
    codecLoop = IOWorkLoop::workLoop();
    codecGate = codecLoop ? IOCommandGate::commandGate(this) : nullptr;
    jackTimer = codecGate ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::jackSettled) : nullptr;
    idleTimer = jackTimer ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::codecIdle) : nullptr;
    if (!idleTimer || codecLoop->addEventSource(codecGate) != kIOReturnSuccess ||
        codecLoop->addEventSource(jackTimer) != kIOReturnSuccess ||
        codecLoop->addEventSource(idleTimer) != kIOReturnSuccess) {
        panic("SwitchAudio: codec work loop setup failed");
    }
    codec.probe(true, tegraCapture);
    // PV6 as a GPIO input (CNF set, output off), so its IN bit is the pad's level; no event source, so its interrupt
    // stays masked: the codec's IRQ reports the jack.
    if (!tegra_gpio_interrupt_nub(kJackDetect, TEGRA_GPIO_EDGE_BOTH))
        panic("SwitchAudio: cannot make PV6 a GPIO input");
    // The jack is first read once its line has settled, with the IRQ already armed so no change is missed
    // meanwhile: read right after the setup above, JD_STATUS says plugged with nothing in the jack. (Linux reads it
    // from a work item and confirms a plug for a second: rt5640_detect_headset.)
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): TegraGPIO keeps its nubs; this reference is not ours
    IOService *nub = tegra_gpio_interrupt_nub(kCodecIrq, TEGRA_GPIO_EDGE_BOTH);
    jackSource = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::jackEdge, nub, 0) : nullptr;
    if (!jackSource || codecLoop->addEventSource(jackSource) != kIOReturnSuccess) {
        panic("SwitchAudio: cannot attach the RT5639 IRQ (GPIO %u, PBB4)", kCodecIrq);
    }
    jackSource->enable();
    jackTimer->setTimeoutMS(kJackSettleMS);
#if DEVELOPMENT || DEBUG
    statsTimer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::statsFired);
    if (!statsTimer || codecLoop->addEventSource(statsTimer) != kIOReturnSuccess)
        panic("SwitchAudio: stats timer setup failed");
    statsResume();
#endif
}

// The codec's interrupt and timers off, then the codec powered down.
void SwitchAudio::codecQuiesce()
{
    jackSource->disable();
    jackTimer->cancelTimeout();
    idleTimer->cancelTimeout();
#if DEVELOPMENT || DEBUG
    if (statsTimer)
        statsTimer->cancelTimeout();
#endif
    codecPowerDown();
}

IOReturn SwitchAudio::codecSleepGated(OSObject *owner, void *entering, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->codecOff)
        return kIOReturnSuccess;
    if (entering) {
        self->codecQuiesce();
        return kIOReturnSuccess;
    }
    if (self->hardwareLost)
        return kIOReturnSuccess;
    self->codec.probe(true, self->tegraCapture);
    self->jackKnown = false;
    self->jackSource->enable();
    self->jackTimer->setTimeoutMS(kJackSettleMS);
#if DEVELOPMENT || DEBUG
    if (self->statsTimer)
        self->statsResume();
#endif
    if (self->control->state) {  // the I2S already runs again (sleepGated)
        self->codecPowerUp();
        self->codec.mute(false);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::codecShutdownGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->codecOff)
        return kIOReturnSuccess;
    self->codecQuiesce();
    self->codec.powerOff();
    self->codecOff = true;
    return kIOReturnSuccess;
}

// The headphone volume is the codec's: 0.375 dB DAC steps, rounded to the nearest.
IOReturn SwitchAudio::setHeadphoneLevel(uint64_t attenuationMdB)
{
    if (!tegra)
        return kIOReturnSuccess;
    const uint64_t steps = attenuationMdB > SWITCH_AUDIO_HEADPHONE_RANGE_MDB ? 0xffff : (attenuationMdB + 187) / 375;
    __atomic_store_n(&headphoneSteps, steps > 0xffff ? 0xffffu : static_cast<unsigned>(steps), __ATOMIC_RELAXED);
    codecStep(kCodecHeadphoneLevel);
    return kIOReturnSuccess;
}
