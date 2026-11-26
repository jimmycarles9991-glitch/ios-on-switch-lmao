// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: Tegra APE playback/capture pipeline, ADMA periods, interrupts, and hardware timestamps.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

namespace {
// Combined AHUB playback FIFO prefill across ADMA, ADMAIF1, MIXER1, SFC1, MVC1, OPE1, and I2S1 (~4.5 ms at 44.1 kHz).
// Shifting the initial anchor backward keeps hw_host_time <= mach_absolute_time() while avoiding a period-0 step.
constexpr uint32_t kAhubFifoPrefillFrames = 198u;
}  // namespace

void SwitchAudio::tick(OSObject *owner, IOTimerEventSource *source)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (!self->control->state)
        return;
    self->advance();
    source->setTimeoutMS(kTickMS);
}

void SwitchAudio::publishHwAnchor(uint64_t sampleTime, uint64_t hostTime, bool bumpSeed)
{
    const uint32_t seq = __atomic_load_n(&control->hw_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&control->hw_seq, seq + 1, __ATOMIC_RELEASE);
    // Order the odd sequence store before payload updates on ARM64 so EL0 readers never observe a torn snapshot.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&control->hw_sample_time, sampleTime, __ATOMIC_RELAXED);
    __atomic_store_n(&control->hw_host_time, hostTime, __ATOMIC_RELAXED);
    if (bumpSeed) {
        __atomic_store_n(&control->hw_seed, __atomic_load_n(&control->hw_seed, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&control->hw_seq, seq + 2, __ATOMIC_RELEASE);
}

int32_t SwitchAudio::desiredMvcMdB() const
{
    if (wantedRoute() == rt5639::kRouteHeadphones) {
        uint8_t reg = 0;
        int32_t mvcMdB = 0;
        switch_audio_hp_stage_split(__atomic_load_n(&headphoneAttenuationMdB, __ATOMIC_RELAXED), &reg, &mvcMdB);
        return mvcMdB;
    }
    const uint32_t atten = __atomic_load_n(&speakerAttenuationMdB, __ATOMIC_RELAXED);
    return atten > SWITCH_AUDIO_SPEAKER_RANGE_MDB ? -120000 : -static_cast<int32_t>(atten);
}

void SwitchAudio::applyMvcLive(bool immediate)
{
    if (!tegra || !loop || !gate || hardwareLost || !apeGate.powered())
        return;
    if (!loop->inGate()) {
        const IOCommandGate::Action thunk = [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            static_cast<SwitchAudio *>(owner)->applyMvcLive(arg != nullptr);
            return kIOReturnSuccess;
        };
        gate->runAction(thunk, reinterpret_cast<void *>(static_cast<uintptr_t>(immediate ? 1u : 0u)));
        return;
    }
    if (hardwareLost || !apeGate.powered() || !ahubActive)
        return;
    const rt5639::Route route = wantedRoute();
    const bool muted =
        __atomic_load_n(&outputMuted, __ATOMIC_RELAXED) ||
        (route == rt5639::kRouteHeadphones
             ? __atomic_load_n(&headphoneAttenuationMdB, __ATOMIC_RELAXED) > SWITCH_AUDIO_HEADPHONE_RANGE_MDB
             : __atomic_load_n(&speakerAttenuationMdB, __ATOMIC_RELAXED) > SWITCH_AUDIO_SPEAKER_RANGE_MDB);
    (void)switch_audio_ahub::mvcSetVolume(ahub, desiredMvcMdB(), muted, immediate);
}

bool SwitchAudio::rampMvcMute()
{
    if (!tegra || !loop || !gate || hardwareLost || !apeGate.powered())
        return false;
    bool ramped = false;
    const IOCommandGate::Action thunk = [](OSObject *owner, void *out, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->hardwareLost || !self->apeGate.powered() || !self->ahubActive)
            return kIOReturnSuccess;
        *static_cast<bool *>(out) = switch_audio_ahub::mvcSetVolume(self->ahub, -120000, true, false);
        return kIOReturnSuccess;
    };
    gate->runAction(thunk, &ramped);
    return ramped;
}

// Per TRM Ch. 23.2.12, OPE1 cannot change live; pause ADMA ch0 briefly while disabling and re-enabling it.
void SwitchAudio::reconfigureOpeForRoute()
{
    if (!tegra || !loop || !gate || hardwareLost || !apeGate.powered())
        return;
    if (!loop->inGate()) {
        const IOCommandGate::Action thunk = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<SwitchAudio *>(owner)->reconfigureOpeForRoute();
            return kIOReturnSuccess;
        };
        gate->runAction(thunk);
        return;
    }
    if (hardwareLost || !apeGate.powered() || !ahubActive)
        return;
    switch_mmio_write32(adma, CH_CTRL, kChCtrl[kChOut] | 1u);
    (void)switch_mmio_wait32(adma, CH_STATUS, 1u << 1, 1u << 1, 1000);
    if (!switch_audio_ahub::opeStop(ahub))
        IOLog("SwitchAudio: OPE1 did not stop during route switch\n");
    if (!switch_audio_ahub::opeConfigure(ahub, clock->rate, wantedRoute() == rt5639::kRouteSpeaker,
                                         SWITCH_AUDIO_OUT_CHANNELS)) {
        IOLog("SwitchAudio: OPE1 reconfigure failed during route switch\n");
    }
    switch_audio_ahub::opeEnable(ahub);
    switch_mmio_write32(adma, CH_CTRL, kChCtrl[kChOut]);
}

void SwitchAudio::tegraStart()
{
    if (hardwareLost || !apeGate.powered())
        return;
    switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
    bzero(out, kOutBytes);
    switch_dcache_clean(out, kOutBytes);
    control->out_frames = 0;
    control->in_frames = 0;
    control->in_anchor = SWITCH_AUDIO_NO_CAPTURE;
    control->out_written = 0;

    switch_mmio_write32(ahub, I2S_CTRL, kI2sCtrl);
    switch_mmio_write32(ahub, I2S_ENABLE, 1);
    switch_mmio_write32(ahub, I2S_TIMING, kI2sTiming);
    switch_mmio_write32(ahub, I2S_RX_CTRL, kI2sDataOffset);
    switch_mmio_write32(ahub, I2S_TX_CTRL, kI2sDataOffset);
    switch_mmio_write32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());
    switch_mmio_write32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());
    // Reset both I2S directions up front: a later TX reset clears I2S_CTRL under active playback.
    i2sSoftReset(I2S_RX_SOFT_RESET, "I2S1 RX");
    if (tegraCapture)
        i2sSoftReset(I2S_TX_SOFT_RESET, "I2S1 TX");

    playbackSfcActive = clientRate != clock->rate;
    switch_audio_ahub::xbarRoutePlayback(ahub, playbackSfcActive);
    const rt5639::Route route = wantedRoute();
    const bool muted =
        __atomic_load_n(&outputMuted, __ATOMIC_RELAXED) ||
        (route == rt5639::kRouteHeadphones
             ? __atomic_load_n(&headphoneAttenuationMdB, __ATOMIC_RELAXED) > SWITCH_AUDIO_HEADPHONE_RANGE_MDB
             : __atomic_load_n(&speakerAttenuationMdB, __ATOMIC_RELAXED) > SWITCH_AUDIO_SPEAKER_RANGE_MDB);
    if (!switch_audio_ahub::opeConfigure(ahub, clock->rate, route == rt5639::kRouteSpeaker, SWITCH_AUDIO_OUT_CHANNELS))
        panic("SwitchAudio: OPE1 configure failed");
    if (!switch_audio_ahub::mvcConfigure(ahub, SWITCH_AUDIO_OUT_CHANNELS, desiredMvcMdB(), muted))
        panic("SwitchAudio: MVC1 configure failed");
    if (playbackSfcActive && !switch_audio_ahub::sfcConfigure(ahub, switch_audio_ahub::kSfc1Base, clientRate,
                                                              clock->rate, SWITCH_AUDIO_OUT_CHANNELS)) {
        panic("SwitchAudio: SFC1 configure failed (%u -> %u Hz)", clientRate, clock->rate);
    }
    if (!switch_audio_ahub::mixerConfigure(ahub, SWITCH_AUDIO_OUT_CHANNELS))
        panic("SwitchAudio: MIXER1 configure failed");
    softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
    softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
    switch_mmio_write32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));
    programChannel(kChOut);

    // TRM Ch. 23.2.1.1: enable AHUB playback chain strictly tail-to-source before starting ADMA ch0.
    switch_mmio_write32(ahub, I2S_RX_ENABLE, 1);
    switch_audio_ahub::opeEnable(ahub);
    switch_audio_ahub::mvcEnable(ahub);
    if (playbackSfcActive)
        switch_audio_ahub::sfcEnable(ahub, switch_audio_ahub::kSfc1Base);
    switch_audio_ahub::mixerEnable(ahub);
    switch_mmio_write32(ahub, ADMAIF_TX_ENABLE, 1);

    switch_mmio_write32(adma, GLOBAL_CG, 0);
    const uint64_t anchorHost = mach_absolute_time();
    control->anchor_host = anchorHost;
    switch_mmio_write32(adma, CH_CMD, 1);
    switch_mmio_write32(adma, GLOBAL_CG, 7);
    ahubActive = true;
    const uint64_t prefillTicks = (static_cast<uint64_t>(kAhubFifoPrefillFrames) * control->ticks_per_frame_q32) >> 32;
    publishHwAnchor(0, anchorHost > prefillTicks ? anchorHost - prefillTicks : 0, true);

    periods[kChOut] = 0;
    xferSeen[kChOut] = static_cast<uint16_t>(switch_mmio_read32(adma, CH_XFER_STATUS));
    graced = false;
    timer->setTimeoutMS(watchdogMS());
#if DEVELOPMENT || DEBUG
    ++statStarts;
#endif
}

void SwitchAudio::programChannel(unsigned ch)
{
    const uint32_t base = ch * CH_STRIDE;
    switch_mmio_write32(adma, base + CH_CMD, 0);
    switch_mmio_write32(adma, base + CH_INT_CLEAR, 1);
    switch_mmio_write32(adma, base + CH_TC, ch == kChOut ? kOutPeriodBytes : kInPeriodBytes);
    switch_mmio_write32(adma, base + CH_CTRL, kChCtrl[ch]);
    switch_mmio_write32(adma, base + CH_SRC, ch == kChOut ? ringPA[SwitchAudioMemoryOutput] : 0);
    switch_mmio_write32(adma, base + CH_TRG, ch == kChOut ? 0 : ringPA[SwitchAudioMemoryInput]);
    switch_mmio_write32(adma, base + CH_FIFO_CTRL, kChFifo[ch]);
    switch_mmio_write32(adma, base + CH_CONFIG, kChConfig[ch]);
}

void SwitchAudio::captureStart()
{
    if (!tegraCapture || captureRunning || hardwareLost || !apeGate.powered())
        return;
    switch_audio_sample_t *in = ring(SwitchAudioMemoryInput);
    bzero(in, kInBytes);
    switch_dcache_clean_invalidate(in, kInBytes);

    captureSfcActive = clientRate != clock->rate;
    switch_audio_ahub::xbarRouteCapture(ahub, captureSfcActive);
    if (captureSfcActive) {
        if (!switch_audio_ahub::sfcConfigure(ahub, switch_audio_ahub::kSfc2Base, clock->rate, clientRate,
                                             SWITCH_AUDIO_IN_CHANNELS)) {
            panic("SwitchAudio: SFC2 configure failed (%u -> %u Hz)", clock->rate, clientRate);
        }
        switch_audio_ahub::sfcEnable(ahub, switch_audio_ahub::kSfc2Base);
    }
    softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
    switch_mmio_write32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));
    programChannel(kChIn);
    switch_mmio_write32(ahub, I2S_TX_ENABLE, 1);
    switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 1);
    captureStartFrame = nowFrame();
    control->in_frames = captureStartFrame;
    __atomic_store_n(&control->in_anchor, captureStartFrame, __ATOMIC_RELEASE);
    switch_mmio_write32(adma, GLOBAL_CG, 0);
    switch_mmio_write32(adma, kChIn * CH_STRIDE + CH_CMD, 1);
    switch_mmio_write32(adma, GLOBAL_CG, 7);
    periods[kChIn] = 0;
    xferSeen[kChIn] = static_cast<uint16_t>(switch_mmio_read32(adma, kChIn * CH_STRIDE + CH_XFER_STATUS));
    captureRunning = true;
}

void SwitchAudio::captureStop()
{
    if (!captureRunning)
        return;
    captureRunning = false;
    __atomic_store_n(&control->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
    const uint32_t base = kChIn * CH_STRIDE;
    switch_mmio_write32(adma, base + CH_CMD, 0);
    if (!switch_mmio_wait32(adma, base + CH_STATUS, 1, 0, 10000))
        panic("SwitchAudio: ADMA ch1 did not stop (STATUS %#x)", switch_mmio_read32(adma, base + CH_STATUS));
    switch_mmio_write32(adma, base + CH_INT_CLEAR, 1);
    stopUnit(ADMAIF_RX_ENABLE, ADMAIF_RX_STATUS, "ADMAIF1 RX");
    softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
    if (captureSfcActive) {
        if (!switch_audio_ahub::sfcStop(ahub, switch_audio_ahub::kSfc2Base))
            panic("SwitchAudio: SFC2 did not stop");
        switch_audio_ahub::xbarRouteCapture(ahub, false);
        captureSfcActive = false;
    }
    stopUnit(I2S_TX_ENABLE, I2S_TX_STATUS, "I2S1 TX");
}

// TRM Ch. 23.2.1.1: stop AHUB playback chain strictly source-to-tail.
void SwitchAudio::tegraStop()
{
    timer->cancelTimeout();
    if (hardwareLost || !apeGate.powered())
        return;
    captureStop();
    if (!ahubActive)
        return;
    switch_mmio_write32(adma, CH_CMD, 0);
    if (!switch_mmio_wait32(adma, CH_STATUS, 1, 0, 10000))
        panic("SwitchAudio: ADMA ch0 did not stop (STATUS %#x)", switch_mmio_read32(adma, CH_STATUS));
    switch_mmio_write32(adma, CH_INT_CLEAR, 1);
    stopUnit(ADMAIF_TX_ENABLE, ADMAIF_TX_STATUS, "ADMAIF1 TX");
    softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
    if (!switch_audio_ahub::mixerStop(ahub))
        panic("SwitchAudio: MIXER1 did not stop");
    if (playbackSfcActive) {
        if (!switch_audio_ahub::sfcStop(ahub, switch_audio_ahub::kSfc1Base))
            panic("SwitchAudio: SFC1 did not stop");
        switch_audio_ahub::xbarRoutePlayback(ahub, false);
        playbackSfcActive = false;
    }
    if (!switch_audio_ahub::mvcStop(ahub))
        panic("SwitchAudio: MVC1 did not stop");
    if (!switch_audio_ahub::opeStop(ahub))
        panic("SwitchAudio: OPE1 did not stop");
    stopUnit(I2S_RX_ENABLE, I2S_RX_STATUS, "I2S1 RX");
    switch_mmio_write32(ahub, I2S_ENABLE, 0);
    ahubActive = false;
}

void SwitchAudio::playedPeriod()
{
    const uint64_t irqHost = mach_absolute_time();
    const uint64_t n = periods[kChOut]++;
    switch_audio_sample_t *slot =
        ring(SwitchAudioMemoryOutput) + (n % kOutPeriods) * kOutPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS;
    const uint64_t done = (n + 1) * kOutPeriodFrames;
    publishHwAnchor(done, irqHost, false);
    const uint64_t now = nowFrame();
#if DEVELOPMENT || DEBUG
    if (statsTimer)
        notePlayed(slot, done);
#endif
    bzero(slot, kOutPeriodBytes);
    switch_dcache_clean(slot, kOutPeriodBytes);
    control->out_frames = done;
    if (done > now + kOutPeriodFrames) {
        panic("SwitchAudio: ADMA ch0 runs fast: %llu frames played at clock frame %llu (I2S %#x CTRL %#x TIMING %#x)",
              done, now, switch_mmio_read32(ahub, I2S_RX_STATUS), switch_mmio_read32(ahub, I2S_CTRL),
              switch_mmio_read32(ahub, I2S_TIMING));
    }
}

#if DEVELOPMENT || DEBUG
void SwitchAudio::notePlayed(const switch_audio_sample_t *slot, uint64_t done)
{
    statFrames += kOutPeriodFrames;
    const bool settled = done > kSettlePeriods * kOutPeriodFrames;
    if (settled) {
        const int64_t lagTicks = static_cast<int64_t>(mach_absolute_time() - control->anchor_host) -
                                 static_cast<int64_t>((done / kOutPeriodFrames) * periodTicksQ16 >> 16);
        const int64_t lagUs = lagTicks * 1000000 / static_cast<int64_t>(hz);
        if (lagUs < statLagMin)
            statLagMin = lagUs;
        if (lagUs > statLagMax)
            statLagMax = lagUs;
    }
    if (!(control->state & SwitchAudioOutputRunning))
        return;
    uint32_t peakL = 0u;
    uint32_t peakR = 0u;
    for (uint32_t i = 0; i < kOutPeriodFrames; ++i) {
        const int64_t l = slot[2u * i];
        const int64_t r = slot[2u * i + 1u];
        const uint32_t absL = static_cast<uint32_t>(l < 0 ? -l : l);
        const uint32_t absR = static_cast<uint32_t>(r < 0 ? -r : r);
        if (absL > peakL)
            peakL = absL;
        if (absR > peakR)
            peakR = absR;
    }
    if (peakL > __atomic_load_n(&statRingPeakL, __ATOMIC_RELAXED))
        __atomic_store_n(&statRingPeakL, peakL, __ATOMIC_RELAXED);
    if (peakR > __atomic_load_n(&statRingPeakR, __ATOMIC_RELAXED))
        __atomic_store_n(&statRingPeakR, peakR, __ATOMIC_RELAXED);
    const int64_t lead = static_cast<int64_t>(control->out_written) - static_cast<int64_t>(done);
    if (settled && lead < statLeadMin)
        statLeadMin = lead;
    if (lead < 0)
        ++statUnderruns;
    const int peak = static_cast<int>(switch_audio_ahub::mvcReadPeak(ahub) >> 16);
    if (peak > statPeak)
        statPeak = peak;
}
#endif

void SwitchAudio::captureOff()
{
    const uint32_t in = kChIn * CH_STRIDE;
    const uint32_t id = kAgicAdma + kChIn;
    IOLog("SwitchAudio: headset microphone off for this boot: ADMA ch1 delivered no period in %llu frames "
          "(XFER %u STATUS %#x INT_STATUS %#x, ADMAIF RX %#x, I2S TX %#x)\n",
          nowFrame() - captureStartFrame, switch_mmio_read32(adma, in + CH_XFER_STATUS) & 0xffff,
          switch_mmio_read32(adma, in + CH_STATUS), switch_mmio_read32(adma, in + CH_INT_STATUS),
          switch_mmio_read32(ahub, ADMAIF_RX_STATUS), switch_mmio_read32(ahub, I2S_TX_STATUS));
    switch_mmio_write32(adma, in + CH_CMD, 0);
    switch_mmio_write32(adma, in + CH_INT_CLEAR, 1);
    switch_mmio_write32(agic, GICD_ICENABLER + 4 * (id / 32), 1u << (id % 32));
    switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 0);
    if (captureSfcActive) {
        switch_mmio_write32(ahub, switch_audio_ahub::kSfc2Base + switch_audio_ahub::kSfcEnable, 0);
        switch_audio_ahub::xbarRouteCapture(ahub, false);
        captureSfcActive = false;
    }
    switch_mmio_write32(ahub, I2S_TX_ENABLE, 0);
    tegraCapture = false;
    captureRunning = false;
    __atomic_store_n(&control->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
    codecStep(kCodecNoMicrophone);
}

void SwitchAudio::capturedPeriod()
{
    captureProven = true;
    const uint64_t n = periods[kChIn]++;
    switch_dcache_invalidate(
        ring(SwitchAudioMemoryInput) + (n % kInPeriods) * kInPeriodFrames * SWITCH_AUDIO_IN_CHANNELS, kInPeriodBytes);
    control->in_frames = captureStartFrame + (n + 1) * kInPeriodFrames;
}

void SwitchAudio::channelInterrupt(unsigned ch)
{
    const uint32_t base = ch * CH_STRIDE;
    const uint32_t status = switch_mmio_read32(adma, base + CH_INT_STATUS);
    if (status)
        switch_mmio_write32(adma, base + CH_INT_CLEAR, status);
    if (!control->state || (ch == kChIn && !captureRunning))
        return;
    const uint16_t xfer = static_cast<uint16_t>(switch_mmio_read32(adma, base + CH_XFER_STATUS));
    uint32_t done = static_cast<uint16_t>(xfer - xferSeen[ch]);
    xferSeen[ch] = xfer;
    if (done > (ch == kChOut ? kOutPeriods : kInPeriods)) {
        panic("SwitchAudio: ADMA ch%u completed %u periods in one interrupt (XFER_STATUS %#x): lost track", ch, done,
              xfer);
    }
    for (; done; --done)
        ch == kChOut ? playedPeriod() : capturedPeriod();
    if (ch != kChOut)
        return;
    graced = false;
    timer->setTimeoutMS(watchdogMS());
    if (!captureRunning)
        return;
    const uint64_t now = nowFrame();
    const uint64_t captured = periods[kChIn] * static_cast<uint64_t>(kInPeriodFrames);
    const uint64_t played = now > captureStartFrame ? now - captureStartFrame : 0;
    if (!captureProven && played > 2 * static_cast<uint64_t>(kOutPeriodFrames)) {
        captureOff();
        return;
    }
    if (played > captured + 2 * static_cast<uint64_t>(kOutPeriodFrames)) {
        const uint32_t in = kChIn * CH_STRIDE;
        const uint32_t id = kAgicAdma + kChIn;
        const uint32_t bit = 1u << (id % 32);
        const uint32_t word = 4 * (id / 32);
        panic("SwitchAudio: ADMA ch1 behind: %llu periods vs %llu, XFER %u seen %u INT_STATUS %#x STATUS %#x, AGIC "
              "pend %u, ADMAIF RX %#x I2S TX %#x",
              periods[kChIn], periods[kChOut], switch_mmio_read32(adma, in + CH_XFER_STATUS) & 0xffff, xferSeen[kChIn],
              switch_mmio_read32(adma, in + CH_INT_STATUS), switch_mmio_read32(adma, in + CH_STATUS),
              !!(switch_mmio_read32(agic, GICD_ISPENDR + word) & bit), switch_mmio_read32(ahub, ADMAIF_RX_STATUS),
              switch_mmio_read32(ahub, I2S_TX_STATUS));
    }
}

void SwitchAudio::cascadeFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->hardwareLost || !self->apeGate.powered())
        return;
    for (;;) {
        const uint32_t iar = switch_mmio_read32(self->agic, GICC_IAR);
        const uint32_t id = iar & 0x3ff;
        if (id >= 1020)
            break;
        if (id == kAgicAdma + kChOut || (self->tegraCapture && id == kAgicAdma + kChIn)) {
            self->channelInterrupt(id - kAgicAdma);
        } else {
            switch_mmio_write32(self->agic, GICD_ICENABLER + 4 * (id / 32), 1u << (id % 32));
            IOLog("SwitchAudio: AGIC interrupt %u is not ours, disabled\n", id);
        }
        switch_mmio_write32(self->agic, GICC_EOIR, iar);
    }
}

void SwitchAudio::watchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (!self->control->state || self->hardwareLost || !self->apeGate.powered())
        return;
    volatile uint8_t *dma = self->adma, *gic = self->agic;
    const uint32_t xfer = switch_mmio_read32(dma, CH_XFER_STATUS) & 0xffff;
    if (xfer != self->xferSeen[kChOut] && !self->graced) {
        self->graced = true;
        sender->setTimeoutMS(kGraceMS);
        return;
    }
    if (xfer != self->xferSeen[kChOut]) {
        char state[160];
        switch_interrupt_describe(kCascadeIntid, state, sizeof(state));
        const uint32_t id = kAgicAdma + kChOut;
        const uint32_t bit = 1u << (id % 32);
        const uint32_t word = 4 * (id / 32);
        panic("SwitchAudio: ADMA ch0 period interrupt lost: XFER %u seen %u INT_STATUS %#x, AGIC en %u pend %u act "
              "%u, %s",
              xfer, self->xferSeen[kChOut], switch_mmio_read32(dma, CH_INT_STATUS),
              !!(switch_mmio_read32(gic, GICD_ISENABLER + word) & bit),
              !!(switch_mmio_read32(gic, GICD_ISPENDR + word) & bit),
              !!(switch_mmio_read32(gic, GICD_ISACTIVER + word) & bit), state);
    }
    panic("SwitchAudio: ADMA ch0 stalled at period %llu: STATUS %#x TC_STATUS %#x, ADMAIF TX %#x, I2S RX %#x EN %#x",
          self->periods[kChOut], switch_mmio_read32(dma, CH_STATUS), switch_mmio_read32(dma, CH_TC_STATUS),
          switch_mmio_read32(self->ahub, ADMAIF_TX_STATUS), switch_mmio_read32(self->ahub, I2S_RX_STATUS),
          switch_mmio_read32(self->ahub, I2S_ENABLE));
}

IOReturn SwitchAudio::requestGated(OSObject *owner, void *selector, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    SwitchAudioControl *c = self->control;
    const uint32_t was = c->state;
    switch (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(selector))) {
    case SwitchAudioStartOutput:
        if (!(was & SwitchAudioOutputRunning)) {
            if (!self->tegra) {
                bzero(self->ring(SwitchAudioMemoryOutput), kOutBytes);
                c->out_frames = self->nowFrame();
            }
            c->state = was | SwitchAudioOutputRunning;
        }
        break;
    case SwitchAudioStopOutput:
        c->state = was & ~static_cast<uint32_t>(SwitchAudioOutputRunning);
        break;
    case SwitchAudioStartInput:
        if (!(was & SwitchAudioInputRunning)) {
            if (!self->tegra) {
                c->in_frames = self->nowFrame();
                __atomic_store_n(&c->in_anchor, c->in_frames, __ATOMIC_RELEASE);
            }
            c->state = was | SwitchAudioInputRunning;
        }
        break;
    case SwitchAudioStopInput:
        if (!self->tegra)
            __atomic_store_n(&c->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
        c->state = was & ~static_cast<uint32_t>(SwitchAudioInputRunning);
        break;
    default:
        return kIOReturnBadArgument;
    }
    const uint32_t actions = switch_audio_transition(was, c->state);
    if (!self->tegra) {
        if (actions & SWITCH_AUDIO_HW_START)
            self->timer->setTimeoutMS(kTickMS);
        if (actions & SWITCH_AUDIO_HW_STOP)
            self->timer->cancelTimeout();
        return kIOReturnSuccess;
    }
    if (actions & SWITCH_AUDIO_HW_START)
        self->tegraStart();
    if (actions & SWITCH_AUDIO_CAPTURE_START)
        self->captureStart();
    if (actions & SWITCH_AUDIO_CAPTURE_STOP)
        self->captureStop();
    if (actions & SWITCH_AUDIO_HW_STOP)
        self->tegraStop();
    return kIOReturnSuccess;
}

// ASoC order: power APE & codec (muted) before I2S clocks/DMA start, then unmute; last stop mutes before clocks stop.
IOReturn SwitchAudio::request(uint32_t selector)
{
    if (selector > SwitchAudioStopInput)
        return kIOReturnBadArgument;
    if (!tegra) {
        return gate->runAction(&SwitchAudio::requestGated, reinterpret_cast<void *>(static_cast<uintptr_t>(selector)));
    }
    const bool run = selector == SwitchAudioStartOutput || selector == SwitchAudioStartInput;
    const uint32_t bit = (selector == SwitchAudioStartOutput || selector == SwitchAudioStopOutput)
                             ? SwitchAudioOutputRunning
                             : SwitchAudioInputRunning;
    const bool startingFirst = run && !control->state;
    const bool stoppingLast = !run && !(control->state & ~bit);
    if (run) {
        if (!acquireApe())
            return kIOReturnNotReady;
        codecStep(kCodecPowerUp);
    }
    if (selector == SwitchAudioStartInput) {
        codecStep(kCodecCaptureOn);
    } else if (selector == SwitchAudioStopInput) {
        codecStep(kCodecCaptureOff);
    }
    if (stoppingLast) {
        if (rampMvcMute())
            IOSleep(18);
        codecStep(kCodecPause);
    }
    void *const argument = reinterpret_cast<void *>(static_cast<uintptr_t>(selector));
    const IOReturn result = gate->runAction(&SwitchAudio::requestGated, argument);
    if (run) {
        if (startingFirst)
            IOSleep(5);
        codecStep(kCodecUnmute);
    }
    return result;
}
