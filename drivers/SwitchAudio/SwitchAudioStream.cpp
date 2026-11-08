// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the playback and capture streams, their ADMA periods, interrupts and deadline.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

void SwitchAudio::tick(OSObject *owner, IOTimerEventSource *source)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (!self->control->state)
        return;
    self->advance();
    source->setTimeoutMS(kTickMS);
}

void SwitchAudio::tegraStart()
{
    if (hardwareLost)
        return;  // the play position stands still: the plug-in sees no progress, never noise
    switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
    bzero(out, kOutBytes);
    switch_dcache_clean(out, kOutBytes);
    control->out_frames = 0;
    control->in_frames = 0;
    control->in_anchor = SWITCH_AUDIO_NO_CAPTURE;
    control->out_written = 0;
    // I2S1 and ADMAIF1, each session in L4T's order (tegra210_i2s_alt.c): set_fmt (CTRL with master mode, I2S_EN),
    // hw_params (timing, data offset, CIF), then the DAPM PRE_PMU sw reset of each direction it powers up
    // (tegra210_i2s_rx_stop/tx_stop), which keeps that configuration and needs master mode (i2sSoftReset). Then
    // the ADMAIF resets and tegra_admaif_hw_params.
    switch_mmio_write32(ahub, I2S_CTRL, kI2sCtrl);
    switch_mmio_write32(ahub, I2S_ENABLE, 1);
    switch_mmio_write32(ahub, I2S_TIMING, kI2sTiming);
    switch_mmio_write32(ahub, I2S_RX_CTRL, kI2sDataOffset);
    switch_mmio_write32(ahub, I2S_TX_CTRL, kI2sDataOffset);
    switch_mmio_write32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());  // playback: XBAR stereo -> DAP stereo
    // capture: DAP stereo -> XBAR mono (left: IN1 is on both)
    switch_mmio_write32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());
    // Both directions reset here, before either runs: a TX reset later would also clear I2S_CTRL under running
    // playback (i2sSoftReset), so captureStart and captureStop leave it out.
    i2sSoftReset(I2S_RX_SOFT_RESET, "I2S1 RX");
    if (tegraCapture)
        i2sSoftReset(I2S_TX_SOFT_RESET, "I2S1 TX");
    softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
    softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
    switch_mmio_write32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));
    programChannel(kChOut);
    switch_mmio_write32(ahub, I2S_RX_ENABLE, 1);
    switch_mmio_write32(ahub, ADMAIF_TX_ENABLE, 1);
    // Sample time 0 is ring frame 0 leaving memory. tegra_adma_start brackets the start with the global clock gate.
    control->anchor_host = mach_absolute_time();
    switch_mmio_write32(adma, GLOBAL_CG, 0);
    switch_mmio_write32(adma, CH_CMD, 1);
    switch_mmio_write32(adma, GLOBAL_CG, 7);
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

// Input requested with the I2S running (tegraStart, or a stream already playing): ADMAIF1 RX, I2S TX and ADMA
// ch1 from capture ring frame 0, which holds the sample time captureStartFrame (in_anchor for the plug-in).
void SwitchAudio::captureStart()
{
    if (!tegraCapture || captureRunning || hardwareLost)
        return;
    switch_audio_sample_t *in = ring(SwitchAudioMemoryInput);
    bzero(in, kInBytes);
    switch_dcache_clean_invalidate(in, kInBytes);
    softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
    switch_mmio_write32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));
    programChannel(kChIn);
    switch_mmio_write32(ahub, I2S_TX_ENABLE, 1);
    switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 1);
    captureStartFrame = nowFrame();
    control->in_frames = captureStartFrame;
    __atomic_store_n(&control->in_anchor, captureStartFrame, __ATOMIC_RELEASE);
    switch_mmio_write32(adma, kChIn * CH_STRIDE + CH_CMD, 1);
    periods[kChIn] = 0;
    xferSeen[kChIn] = static_cast<uint16_t>(switch_mmio_read32(adma, kChIn * CH_STRIDE + CH_XFER_STATUS));
    captureRunning = true;
}

// tegraStop's capture half: DMA, ADMAIF, I2S TX; playback, if it runs, goes on.
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
    stopUnit(I2S_TX_ENABLE, I2S_TX_STATUS, "I2S1 TX");
}

// tegra_adma_stop, tegra_admaif_stop, the I2S widgets' power-down: DMA, then ADMAIF, then I2S.
void SwitchAudio::tegraStop()
{
    timer->cancelTimeout();
    if (hardwareLost)
        return;  // tegraStart started nothing; a powered-off APE must not be accessed
    captureStop();
    switch_mmio_write32(adma, CH_CMD, 0);
    if (!switch_mmio_wait32(adma, CH_STATUS, 1, 0, 10000))
        panic("SwitchAudio: ADMA ch0 did not stop (STATUS %#x)", switch_mmio_read32(adma, CH_STATUS));
    switch_mmio_write32(adma, CH_INT_CLEAR, 1);
    stopUnit(ADMAIF_TX_ENABLE, ADMAIF_TX_STATUS, "ADMAIF1 TX");
    softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
    stopUnit(I2S_RX_ENABLE, I2S_RX_STATUS, "I2S1 RX");
    switch_mmio_write32(ahub, I2S_ENABLE,
                        0);  // the bit and frame clocks stop until the next session (runtime suspend in Linux)
}

// Period n of the playback ring has left memory: clear it for the next turn and move out_frames past it. A DMA
// that finishes periods sooner than the 48 kHz clock allows is not paced by the I2S: panic rather than play noise.
void SwitchAudio::playedPeriod()
{
    const uint64_t n = periods[kChOut]++;
    switch_audio_sample_t *slot =
        ring(SwitchAudioMemoryOutput) + (n % kOutPeriods) * kOutPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS;
    const uint64_t done = (n + 1) * kOutPeriodFrames;
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
// The stats: the period that just played, before it is cleared: its loudest sample (in 16-bit units:
// 32767 is full scale), an underrun when the plug-in had not written up to its end, how far past it the plug-in
// had written (the write-ahead margin: the safety offset in use, less the IO thread's lateness), and how late this
// interrupt runs after the period's end on the I2S clock model the plug-in's zero time stamps use
// (anchor_host + frames * ticks_per_frame_q32). The model and the host timer both run from the
// 38.4 MHz oscillator, so the lag only holds the DMA's start offset and the interrupt latency: one that drifts
// over a session means the time stamps do. The first kSettlePeriods of a start stay out of lead and lag (the audio
// server's first IO cycle comes after them, so "under" counts one per start). (The plug-in's writes are already in
// this CPU-coherent mapping.)
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
    const int64_t lead = static_cast<int64_t>(control->out_written) - static_cast<int64_t>(done);
    if (settled && lead < statLeadMin)
        statLeadMin = lead;
    if (lead < 0)
        ++statUnderruns;
    int peak = statPeak;
    for (uint32_t i = 0; i < kOutPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS; ++i) {
        // NOLINTNEXTLINE(bugprone-signed-bitwise): the slot's signed top half, sign and all
        const int sample = slot[i] >> 16;  // -32768..32767
        const int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
            peak = magnitude;
    }
    statPeak = peak;
}
#endif

// Audio work loop: the capture channel delivered no period in two playback periods, on its first run since
// start. Capture stops for the boot, playback goes on, and the microphone reads as absent (publishJack).
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
    switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 0);  // not waited for: nothing here starts them again this boot
    switch_mmio_write32(ahub, I2S_TX_ENABLE, 0);
    tegraCapture = false;
    captureRunning = false;
    __atomic_store_n(&control->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
    codecStep(kCodecNoMicrophone);
}

// Period n of the capture ring is in memory: drop what the CPUs cached of it, then publish it.
void SwitchAudio::capturedPeriod()
{
    captureProven = true;
    const uint64_t n = periods[kChIn]++;
    switch_dcache_clean_invalidate(
        ring(SwitchAudioMemoryInput) + (n % kInPeriods) * kInPeriodFrames * SWITCH_AUDIO_IN_CHANNELS, kInPeriodBytes);
    control->in_frames = captureStartFrame + (n + 1) * kInPeriodFrames;
}

// One ADMA channel's interrupt: acknowledge it and account every period its completed-period counter
// (ADMA_CH_XFER_STATUS, as tegra_adma_get_residue reads it) has moved past, so a late interrupt loses nothing.
void SwitchAudio::channelInterrupt(unsigned ch)
{
    const uint32_t base = ch * CH_STRIDE;
    const uint32_t status = switch_mmio_read32(adma, base + CH_INT_STATUS);
    if (status)
        switch_mmio_write32(adma, base + CH_INT_CLEAR, status);
    if (!control->state || (ch == kChIn && !captureRunning))
        return;  // the period that ended as the stream stopped
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
    // Capture runs on the same clock from its own start: two playback periods behind the clock, its interrupt is lost
    // or its DMA stalled.
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
              "pend %u, "
              "ADMAIF RX %#x I2S TX %#x",
              periods[kChIn], periods[kChOut], switch_mmio_read32(adma, in + CH_XFER_STATUS) & 0xffff, xferSeen[kChIn],
              switch_mmio_read32(adma, in + CH_INT_STATUS), switch_mmio_read32(adma, in + CH_STATUS),
              !!(switch_mmio_read32(agic, GICD_ISPENDR + word) & bit), switch_mmio_read32(ahub, ADMAIF_RX_STATUS),
              switch_mmio_read32(ahub, I2S_TX_STATUS));
    }
}

// Work loop action for INTID 134 (level; the GIC masks it while this runs): drain the AGIC.
void SwitchAudio::cascadeFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    for (;;) {
        const uint32_t iar = switch_mmio_read32(self->agic, GICC_IAR);
        const uint32_t id = iar & 0x3ff;
        if (id >= 1020)
            break;
        if (id == kAgicAdma + kChOut || (self->tegraCapture && id == kAgicAdma + kChIn)) {
            self->channelInterrupt(id - kAgicAdma);
        } else {
            switch_mmio_write32(self->agic, GICD_ICENABLER + 4 * (id / 32),
                                1u << (id % 32));  // not ours: keep it off
            IOLog("SwitchAudio: AGIC interrupt %u is not ours, disabled\n", id);
        }
        switch_mmio_write32(self->agic, GICC_EOIR, iar);
    }
}

// No playback period interrupt for 4 periods. If the ADMA moved on, its interrupt is not reaching us (after a grace
// period for an action already queued); if it did not, the DMA is stalled (no I2S clock pulling data). Either way,
// one line with the state.
void SwitchAudio::watchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (!self->control->state)
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
    // Both directions share one clock: the I2S and playback DMA run while either is requested, the capture path
    // only while input is.
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

// As ASoC orders a stream: the codec powered with its outputs muted, then the I2S clocks and the DMA, then the
// unmute, so the RT5639 (the I2S slave) has a running frame clock before anything reaches its amplifiers. The
// last stop mutes before the clocks stop.
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
    if (run)
        codecStep(kCodecPowerUp);
    if (selector == SwitchAudioStartInput) {
        codecStep(kCodecCaptureOn);
    } else if (selector == SwitchAudioStopInput) {
        codecStep(kCodecCaptureOff);
    }
    if (!run && !(control->state & ~bit))
        codecStep(kCodecPause);
    void *const argument = reinterpret_cast<void *>(static_cast<uintptr_t>(selector));
    const IOReturn result = gate->runAction(&SwitchAudio::requestGated, argument);
    if (run)
        codecStep(kCodecUnmute);
    return result;
}
