// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the RT5639 on its own work loop, jack detect, routing, volume staging and stats.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

namespace {
constexpr uint32_t routeToAbi(rt5639::Route route)
{
    if (route == rt5639::kRouteSpeaker)
        return SwitchAudioRouteSpeaker;
    if (route == rt5639::kRouteHeadphones)
        return SwitchAudioRouteHeadphones;
    return SwitchAudioRouteAuto;
}
}  // namespace

rt5639::Route SwitchAudio::wantedRoute() const
{
    const uint32_t req = __atomic_load_n(&requestedRoute, __ATOMIC_RELAXED);
    if (req == SwitchAudioRouteSpeaker)
        return rt5639::kRouteSpeaker;
    if (req == SwitchAudioRouteHeadphones && jackIn)
        return rt5639::kRouteHeadphones;
    return jackIn && !speakerOnly ? rt5639::kRouteHeadphones : rt5639::kRouteSpeaker;
}

void SwitchAudio::codecPowerUp()
{
    idleTimer->cancelTimeout();
    if (!acquireApe())
        return;
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
    const bool routeChanged = codec.route() != want;
    if (routeChanged) {
        codec.routeDown();
        codec.routeUp(want);
    }
    __atomic_store_n(&control->active_route, routeToAbi(want), __ATOMIC_RELEASE);
    if (routeChanged) {
        reconfigureOpeForRoute();
        applyMvcLive(true);
    }
}

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
    if (codec.biasIsOn()) {
        codec.biasOff();
        IOSleep(5);
    }
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
    case kCodecHeadphoneLevel: {
        const uint32_t att = __atomic_load_n(&self->headphoneAttenuationMdB, __ATOMIC_RELAXED);
        const bool muted = att > SWITCH_AUDIO_HEADPHONE_RANGE_MDB;
        uint8_t hpReg = 8;
        int32_t mvcMdB = 0;
        switch_audio_hp_stage_split(att, &hpReg, &mvcMdB);
        (void)mvcMdB;
        self->codec.setHeadphoneAnalog(hpReg, muted);
        break;
    }
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
    case kCodecRouteSync: {
        const rt5639::Route want = self->wantedRoute();
        const bool switchCodec = self->codecOn && self->codec.route() != want;
        if (switchCodec) {
            self->codec.routeDown();
            self->codec.routeUp(want);
        }
        const uint32_t nextAbi = routeToAbi(want);
        const uint32_t prevAbi = __atomic_exchange_n(&self->control->active_route, nextAbi, __ATOMIC_ACQ_REL);
        if (switchCodec || prevAbi != nextAbi)
            self->reconfigureOpeForRoute();
        self->applyMvcLive(false);
        if (switchCodec && self->control->state)
            self->codec.mute(false);
        break;
    }
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
    if (self->control->state || self->codecOff)
        return;
    if (self->codec.biasIsOn())
        tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");
    self->codecPowerDown();
    self->releaseApe();
}

// PBB4 and PV6, both edges: wait for jack contacts to settle before probing the codec.
void SwitchAudio::jackEdge(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchAudio *>(owner)->jackTimer->setTimeoutMS(kJackSettleMS);
}

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
    if (self->codecOff || self->hardwareLost || !self->acquireApe())
        return;
    if (!self->control->state)
        self->idleTimer->setTimeoutMS(kIdleMS);
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
    if (first && !self->codec.biasIsOn())
        self->codec.biasStandby();
    self->noteJack();
    const rt5639::Route want = self->wantedRoute();
    __atomic_store_n(&self->control->active_route, routeToAbi(want), __ATOMIC_RELEASE);
    if (!self->codecOn || self->codec.route() == want)
        return;
    self->codec.routeDown();
    self->codec.routeUp(want);
    self->reconfigureOpeForRoute();
    self->applyMvcLive(true);
    if (self->control->state)
        self->codec.mute(false);
}

#if DEVELOPMENT || DEBUG
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
    const uint32_t ringPeakL = __atomic_exchange_n(&self->statRingPeakL, 0u, __ATOMIC_RELAXED);
    const uint32_t ringPeakR = __atomic_exchange_n(&self->statRingPeakR, 0u, __ATOMIC_RELAXED);
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
    char apeBuf[224] = "ape off";
    if (__atomic_load_n(&self->apeHeld, __ATOMIC_RELAXED) && self->apeGate.powered()) {
        uint32_t mvcPeakL = 0u;
        uint32_t mvcPeakR = 0u;
        uint32_t mvcErr = 0u;
        uint32_t opeEn = 0u;
        uint32_t peqEn = 0u;
        uint32_t mbdrcEn = 0u;
        if (self->ahubActive) {
            using namespace switch_audio_ahub;
            mvcReadPeakChannels(self->ahub, &mvcPeakL, &mvcPeakR);
            mvcErr = mvcReadConfigErr(self->ahub);
            opeEn = switch_mmio_read32(self->ahub, kOpe1Base + kOpeEnable) & 1u;
            peqEn = (switch_mmio_read32(self->ahub, kPeq1Base + kPeqCfg) & kPeqCfgModeActive) ? 1u : 0u;
            mbdrcEn = (switch_mmio_read32(self->ahub, kMbdrc1Base + kMbdrcCfg) & 0xfu) ? 1u : 0u;
        }
        snprintf(apeBuf, sizeof(apeBuf),
                 "ring %#x/%#x, ADMA %#x xfer %u, I2S en %u rx %#x, ADMAIF tx %#x, SFC %u, MVC peak %#x/%#x err %#x, "
                 "OPE %u/%u/%u",
                 ringPeakL, ringPeakR, switch_mmio_read32(self->adma, CH_STATUS),
                 switch_mmio_read32(self->adma, CH_XFER_STATUS) & 0xffffu, switch_mmio_read32(self->ahub, I2S_ENABLE),
                 switch_mmio_read32(self->ahub, I2S_RX_STATUS), switch_mmio_read32(self->ahub, ADMAIF_TX_STATUS),
                 self->playbackSfcActive ? 1u : 0u, mvcPeakL, mvcPeakR, mvcErr, opeEn, peqEn, mbdrcEn);
    }
    static const char *const routes[] = {"none", "speaker", "headphones"};
    char line[640];
    snprintf(line, sizeof(line),
             "SwitchAudio stats: run %u starts %u moved %llu under %u peak %d io %llu x %llu f lead %s lag %s us | "
             "%s | jack %s st %#x pv6 %d, %s | codec %04x %04x %04x %04x vol %04x %04x dac %04x spo %04x/%04x clsd "
             "%04x",
             self->control->state, starts, moved, self->statUnderruns, peak, cycles, cycles ? written / cycles : 0,
             lead, lag, apeBuf, self->jackIn ? "in" : "out", SwitchRT5639::read(rt5639::INT_IRQ_ST),
             tegra_gpio_read(kJackDetect), routes[self->codec.route()], SwitchRT5639::read(rt5639::PWR_DIG1),
             SwitchRT5639::read(rt5639::PWR_ANLG1), SwitchRT5639::read(rt5639::PWR_MIXER),
             SwitchRT5639::read(rt5639::PWR_VOL), SwitchRT5639::read(rt5639::SPK_VOL),
             SwitchRT5639::read(rt5639::HP_VOL), SwitchRT5639::read(rt5639::DAC1_DIG_VOL),
             SwitchRT5639::read(rt5639::SPO_L_MIXER), SwitchRT5639::read(rt5639::SPO_R_MIXER),
             SwitchRT5639::read(rt5639::CLS_D_OUT));
    os_log(OS_LOG_DEFAULT, "%s", line);  // log only, never the boot console
    sender->setTimeoutMS(kStatsMS);
}

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
    // PV6 sits directly on the headphone-detect net so idle jack edges wake us even with APE/PLLA/AUD_MCLK gated off.
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): TegraGPIO keeps its nubs; this reference is not ours
    IOService *pv6Nub = tegra_gpio_interrupt_nub(kJackDetect, TEGRA_GPIO_EDGE_BOTH);
    pv6Source =
        pv6Nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::jackEdge, pv6Nub, 0) : nullptr;
    if (!pv6Source || codecLoop->addEventSource(pv6Source) != kIOReturnSuccess) {
        panic("SwitchAudio: cannot attach jack-detect GPIO (GPIO %u, PV6)", kJackDetect);
    }
    pv6Source->enable();
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

void SwitchAudio::codecQuiesce()
{
    jackSource->disable();
    pv6Source->disable();
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
    self->pv6Source->enable();
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

IOReturn SwitchAudio::setHeadphoneLevel(uint64_t attenuationMdB)
{
    const bool muted = attenuationMdB > SWITCH_AUDIO_HEADPHONE_RANGE_MDB;
    const uint32_t stored = muted ? UINT32_MAX : static_cast<uint32_t>(attenuationMdB);
    const uint32_t prev = __atomic_exchange_n(&headphoneAttenuationMdB, stored, __ATOMIC_RELAXED);
    if (!tegra)
        return kIOReturnSuccess;
    applyMvcLive(false);
    const bool prevMuted = prev > SWITCH_AUDIO_HEADPHONE_RANGE_MDB;
    uint8_t prevHpReg = 8;
    uint8_t nextHpReg = 8;
    int32_t mvcMdB = 0;
    switch_audio_hp_stage_split(prev, &prevHpReg, &mvcMdB);
    switch_audio_hp_stage_split(stored, &nextHpReg, &mvcMdB);
    (void)mvcMdB;
    if (nextHpReg != prevHpReg || muted != prevMuted)
        codecStep(kCodecHeadphoneLevel);
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::setSpeakerLevel(uint64_t attenuationMdB)
{
    const uint32_t stored =
        attenuationMdB > SWITCH_AUDIO_SPEAKER_RANGE_MDB ? UINT32_MAX : static_cast<uint32_t>(attenuationMdB);
    __atomic_store_n(&speakerAttenuationMdB, stored, __ATOMIC_RELAXED);
    if (!tegra)
        return kIOReturnSuccess;
    applyMvcLive(false);
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::setMute(bool muted)
{
    __atomic_store_n(&outputMuted, muted, __ATOMIC_RELAXED);
    __atomic_store_n(&control->muted, muted ? 1u : 0u, __ATOMIC_RELEASE);
    if (!tegra)
        return kIOReturnSuccess;
    applyMvcLive(false);
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::setRoute(uint32_t route)
{
    if (route > SwitchAudioRouteHeadphones)
        return kIOReturnBadArgument;
    __atomic_store_n(&requestedRoute, route, __ATOMIC_RELAXED);
    if (!tegra) {
        __atomic_store_n(&control->active_route, route, __ATOMIC_RELEASE);
        return kIOReturnSuccess;
    }
    codecStep(kCodecRouteSync);
    return kIOReturnSuccess;
}
