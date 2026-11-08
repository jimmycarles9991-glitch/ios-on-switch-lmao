// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the APE's power, clocks and pads, the AHUB and ADMA set-up, and the rate changes.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

// `clock`'s PLLA and PLLA_OUT0, in the order clk-pll.c programs a disabled SDM PLL (set_defaults,
// _update_pll_mnp, _clk_pll_enable, wait for lock). Left as it is if it already runs this configuration.
void SwitchAudio::startPlla()
{
    const uint32_t mnp = pllaMnp(*clock);
    const uint32_t sdm = clock->sdm;
    const uint32_t base = switch_mmio_read32(car, PLLA_BASE);
    const bool running = (base & (PLL_ENABLE | PLL_BYPASS | PLLA_IDDQ)) == PLL_ENABLE &&
                         (base & PLLA_MNP_MASK) == mnp && ((switch_mmio_read32(car, PLLA_MISC2) & PLLA_EN_SDM) != 0u) &&
                         (switch_mmio_read32(car, PLLA_MISC1) & 0xffff) == sdm;
    if (!running) {
        switch_mmio_write32(car, PLLA_OUT,
                            switch_mmio_read32(car, PLLA_OUT) & ~3u);  // PLLA_OUT0 off while the PLL changes
        switch_mmio_write32(car, PLLA_BASE, (base & ~PLL_ENABLE) | PLLA_IDDQ);
        switch_mmio_write32(car, PLLA_MISC0, PLLA_LOCK_ENABLE);
        switch_mmio_write32(car, PLLA_MISC2, 0);
        switch_mmio_write32(car, PLLA_BASE, (switch_mmio_read32(car, PLLA_BASE) & ~(PLL_BYPASS | PLLA_MNP_MASK)) | mnp);
        switch_mmio_write32(car, PLLA_MISC1, (switch_mmio_read32(car, PLLA_MISC1) & ~0xffffu) | sdm);
        switch_mmio_write32(car, PLLA_MISC2, PLLA_EN_SDM);
        IODelay(1);
        switch_mmio_write32(car, PLLA_BASE, switch_mmio_read32(car, PLLA_BASE) & ~PLLA_IDDQ);
        IODelay(5);
        switch_mmio_write32(car, PLLA_BASE, switch_mmio_read32(car, PLLA_BASE) | PLL_ENABLE);
        if (!switch_mmio_wait32(car, PLLA_BASE, PLL_LOCK, PLL_LOCK, 600)) {
            panic("SwitchAudio: PLLA did not lock: BASE %#x MISC0 %#x MISC1 %#x MISC2 %#x",
                  switch_mmio_read32(car, PLLA_BASE), switch_mmio_read32(car, PLLA_MISC0),
                  switch_mmio_read32(car, PLLA_MISC1), switch_mmio_read32(car, PLLA_MISC2));
        }
        IODelay(50);
    }
    // PLLA_OUT0: ratio at 15:8, out of reset (bit 0) and enabled (bit 1); the vreg field 31:28 stays 0.
    const uint32_t out0Div = clock->out0_x2 - 2;
    switch_mmio_write32(car, PLLA_OUT, (switch_mmio_read32(car, PLLA_OUT) & ~0xf000ff03u) | (out0Div << 8));
    switch_mmio_write32(car, PLLA_OUT, switch_mmio_read32(car, PLLA_OUT) | 3u);
}

// PLLA and the I2S and MCLK dividers under it for `clock`; nothing on them may run.
void SwitchAudio::startClocks()
{
    startPlla();
    switch_mmio_write32(car, CLK_SOURCE_I2S0, switch_audio_i2s_divider(clock->out0_per_fs));         // PLLA_OUT0
    switch_mmio_write32(car, CLK_SOURCE_EXTERN1, switch_audio_extern1_divider(clock->out0_per_fs));  // PLLA_OUT0
}

void SwitchAudio::tegraClocksAndPads()
{
    // Every rate below assumes the 38.4 MHz oscillator as PLL reference (the T210B01 PLLA table has only that).
    const uint32_t osc = switch_mmio_read32(car, OSC_CTRL);
    if ((osc >> 28) != 5 || ((osc >> 26) & 3))
        panic("SwitchAudio: unexpected OSC_CTRL %#x (want 38.4 MHz, ref /1)", osc);
    // The APE partition (AHUB, ADMA, AGIC) is powered at boot and nothing here gates it; un-gating it would need the
    // MC flush and SLCG sequence of powergate-t21x.c, so a gated partition is reported, not handled.
    if (!(switch_mmio_read32(pmc, PWRGATE_STATUS) & kPartitionAud)) {
        panic("SwitchAudio: APE power partition is off (PMC PWRGATE_STATUS %#x)",
              switch_mmio_read32(pmc, PWRGATE_STATUS));
    }
    if ((switch_mmio_read32(pmc, IO_DPD_STATUS) & (1u << 17)) ||
        (switch_mmio_read32(pmc, IO_DPD2_STATUS) & (1u << 29))) {
        panic("SwitchAudio: audio pads in deep power-down (IO_DPD_STATUS %#x IO_DPD2_STATUS %#x)",
              switch_mmio_read32(pmc, IO_DPD_STATUS), switch_mmio_read32(pmc, IO_DPD2_STATUS));
    }
    // DAP1 is on the audio-hv rail, AUD_MCLK on audio; both are vdd-1v8 (MAX77620 SD3), DTB iopad-defaults 1.8 V.
    // mainline tegra_io_pad_set_voltage: write-enable the bit in PWR_DET, then clear it in PWR_DET_VALUE.
    if (switch_mmio_read32(pmc, PWR_DET_VAL) & kPad33V) {
        switch_mmio_write32(pmc, PWR_DET, switch_mmio_read32(pmc, PWR_DET) | kPad33V);
        switch_mmio_write32(pmc, PWR_DET_VAL, switch_mmio_read32(pmc, PWR_DET_VAL) & ~kPad33V);
        IODelay(250);
    }
    if (switch_mmio_read32(pmc, NO_IOPOWER) & kNoIoPowerAudioHv)
        switch_mmio_write32(pmc, NO_IOPOWER, switch_mmio_read32(pmc, NO_IOPOWER) & ~kNoIoPowerAudioHv);

    startClocks();
    switch_mmio_write32(car, CLK_SOURCE_APE, kApeSource);
    switch_mmio_write32(car, CLK_SOURCE_D_AUDIO, 0);  // PLLA_OUT0 (mux_d_audio_clk index 0), 1:1
    switch_mmio_write32(car, CLK_ENB_Y_SET, kClkY);
    switch_mmio_write32(car, CLK_ENB_V_SET, kClkV);
    switch_mmio_write32(car, CLK_ENB_L_SET, kClkL);
    IODelay(2);
    switch_mmio_write32(car, RST_DEV_Y_CLR, kClkY);  // hekate hw_init leaves APE in reset
    switch_mmio_write32(car, RST_DEV_V_CLR, kClkV);
    switch_mmio_write32(car, RST_DEV_L_CLR, kClkL);
    (void)switch_mmio_read32(car, RST_DEV_L_CLR);
    IODelay(2);
    // MCLK: clk_out_1 <- extern1, forced on (clk-tegra-pmc.c PMC_CLK(1, 6, 2)).
    switch_mmio_write32(pmc, CLK_OUT_CNTRL, (switch_mmio_read32(pmc, CLK_OUT_CNTRL) & ~kClkOut1Mask) | kClkOut1Extern1);
    jackPinFound = switch_mmio_read32(pinmux, kPinJackDetect);
    for (const Pin &pin : kPins) {
        switch_mmio_write32(pinmux, pin.offset, (switch_mmio_read32(pinmux, pin.offset) & ~kPinFields) | pin.value);
    }
}

void SwitchAudio::tegraAhubAndDma()
{
    switch_mmio_write32(ahub, ADMAIF_GLOBAL_ENABLE, 1);
    for (uint32_t part = 0; part < 3; ++part) {
        switch_mmio_write32(ahub, XBAR_I2S1 + part * XBAR_PART, part ? 0 : kXbarFromAdmaif1);
        switch_mmio_write32(ahub, XBAR_ADMAIF1 + part * XBAR_PART, part ? 0 : kXbarFromI2s1);
    }
    // tegra_adma_init: clear interrupts, soft reset, enable.
    switch_mmio_write32(adma, GLOBAL_INT_CLEAR, 1);
    switch_mmio_write32(adma, GLOBAL_SOFT_RESET, 1);
    if (!switch_mmio_wait32(adma, GLOBAL_SOFT_RESET, 1, 0, 10000))
        panic("SwitchAudio: ADMA soft reset did not finish");
    switch_mmio_write32(adma, GLOBAL_CMD, 1);
}

// The two ADMA channel SPIs on the AGIC, as Linux irq-gic.c sets up a distributor and CPU interface (priority
// 0xa0, level, the reading CPU interface as target, PMR 0xf0, bypass bits kept). A refused enable panics.
void SwitchAudio::tegraAgic()
{
    const uint32_t mask =
        switch_mmio_read32(agic, GICD_ITARGETSR) & 0xff;  // banked: the interface our accesses come from
    if (!mask)
        panic("SwitchAudio: AGIC CPU interface mask reads 0 (GICD_CTLR %#x)", switch_mmio_read32(agic, GICD_CTLR));
    const unsigned channels = tegraCapture ? 2 : 1;
    for (unsigned ch = 0; ch < channels; ++ch) {
        const uint32_t id = kAgicAdma + ch;
        const uint32_t bit = 1u << (id % 32);
        const uint32_t word = 4 * (id / 32);
        const uint32_t shift = 8 * (id % 4);
        switch_mmio_write32(agic, GICD_ICENABLER + word, bit);
        switch_mmio_write32(agic, GICD_ICPENDR + word, bit);
        const uint32_t byteReg = id & ~3u;
        switch_mmio_write32(agic, GICD_IPRIORITYR + byteReg,
                            (switch_mmio_read32(agic, GICD_IPRIORITYR + byteReg) & ~(0xffu << shift)) | 0xa0u << shift);
        switch_mmio_write32(agic, GICD_ITARGETSR + byteReg,
                            (switch_mmio_read32(agic, GICD_ITARGETSR + byteReg) & ~(0xffu << shift)) | mask << shift);
        const uint32_t cfg = GICD_ICFGR + 4 * (id / 16);
        switch_mmio_write32(agic, cfg, switch_mmio_read32(agic, cfg) & ~(2u << (2 * (id % 16))));  // level
    }
    switch_mmio_write32(agic, GICD_CTLR, switch_mmio_read32(agic, GICD_CTLR) | 1);
    switch_mmio_write32(agic, GICC_PMR, 0xf0);
    switch_mmio_write32(agic, GICC_CTLR, (switch_mmio_read32(agic, GICC_CTLR) & 0x1e0) | 1);
    for (unsigned ch = 0; ch < channels; ++ch) {
        const uint32_t id = kAgicAdma + ch;
        const uint32_t bit = 1u << (id % 32);
        const uint32_t word = 4 * (id / 32);
        switch_mmio_write32(agic, GICD_ISENABLER + word, bit);
        if (!(switch_mmio_read32(agic, GICD_ISENABLER + word) & bit)) {
            panic("SwitchAudio: AGIC refused SPI %u (GICD_CTLR %#x GICC_CTLR %#x)", id - 32,
                  switch_mmio_read32(agic, GICD_CTLR), switch_mmio_read32(agic, GICC_CTLR));
        }
    }
}

void SwitchAudio::tegraInit()
{
    car = map(kCarPA, 0x1000, "CAR");
    pmc = map(kPmcPagePA, 0x1000, "PMC") + kPmcOffset;
    pinmux = map(kPinmuxPA, 0x1000, "pinmux");
    ahub = map(kAhubPA, 0x2000, "AHUB");
    adma = map(kAdmaPA, 0x1000, "ADMA");
    agic = map(kAgicPA, 0x2000, "AGIC");
    for (uint32_t type = SwitchAudioMemoryOutput; type <= SwitchAudioMemoryInput; ++type) {
        IOByteCount length = 0;
        const addr64_t pa = memory[type]->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
        const uint32_t size = type == SwitchAudioMemoryOutput ? kOutBytes : kInBytes;
        if (!pa || length < size || pa + size > 0x100000000ULL)
            panic("SwitchAudio: ring %u at %#llx is not DMA-able", type, pa);
        ringPA[type] = static_cast<uint32_t>(pa);
    }
    tegraClocksAndPads();
    tegraAhubAndDma();
    tegraAgic();
    IOService *nub = switch_interrupt_nub(kCascadeIntid, false);  // sleeps until the GIC is up
    cascade = IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::cascadeFired, nub, 0);
    if (!cascade || loop->addEventSource(cascade) != kIOReturnSuccess) {
        panic("SwitchAudio: cannot attach the APE interrupt (INTID %u)", kCascadeIntid);
    }
    cascade->enable();
}

void SwitchAudio::softReset(uint32_t reg, const char *what)
{
    switch_mmio_write32(ahub, reg, 1);
    if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000))
        panic("SwitchAudio: %s soft reset did not finish", what);
}

// tegra210_i2s_alt.c tegra210_i2s_sw_reset: the reset clears the CIF and stream controls and I2S_CTRL, so they are
// saved around it. It completes on the I2S's interface clock, so I2S_CTRL must already say master (the bit clock then
// comes from the CAR i2s0 clock; as a slave the I2S would wait for one from the codec, which is a slave too).
void SwitchAudio::i2sSoftReset(uint32_t reg, const char *what)
{
    const uint32_t saved[] = {I2S_TX_CIF, I2S_RX_CIF, I2S_TX_CTRL, I2S_RX_CTRL, I2S_CTRL};
    uint32_t values[5];
    for (unsigned i = 0; i < 5; ++i)
        values[i] = switch_mmio_read32(ahub, saved[i]);
    switch_mmio_write32(ahub, reg, 1);
    if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000)) {
        panic("SwitchAudio: %s soft reset did not finish (I2S CTRL %#x EN %#x TIMING %#x RX %#x TX %#x; CAR I2S0 "
              "src %#x "
              "enb %u rst %u PLLA %#x OUT %#x)",
              what, switch_mmio_read32(ahub, I2S_CTRL), switch_mmio_read32(ahub, I2S_ENABLE),
              switch_mmio_read32(ahub, I2S_TIMING), switch_mmio_read32(ahub, I2S_RX_STATUS),
              switch_mmio_read32(ahub, I2S_TX_STATUS), switch_mmio_read32(car, CLK_SOURCE_I2S0),
              !!(switch_mmio_read32(car, CLK_OUT_ENB_L) & kClkL), !!(switch_mmio_read32(car, RST_DEVICES_L) & kClkL),
              switch_mmio_read32(car, PLLA_BASE), switch_mmio_read32(car, PLLA_OUT));
    }
    for (unsigned i = 0; i < 5; ++i)
        switch_mmio_write32(ahub, saved[i], values[i]);
}

void SwitchAudio::stopUnit(uint32_t enable, uint32_t status, const char *what)
{
    switch_mmio_write32(ahub, enable, 0);
    if (!switch_mmio_wait32(ahub, status, 1, 0, 10000))
        panic("SwitchAudio: %s did not stop (status %#x)", what, switch_mmio_read32(ahub, status));
}

// SC7 powers the APE partition and the codec's link down and resets the CAR, ADMA, AHUB and AGIC; the codec keeps
// its supply only as LDO1_EN (PZ4) is held, so it is set up again from reset like at boot. The partition comes
// back as powergate-t21x.c powers a partition on: modules in reset with clocks on, toggle, unclamp.
bool SwitchAudio::apePartitionOn()
{
    if (switch_mmio_read32(pmc, PWRGATE_STATUS) & kPartitionAud)
        return true;
    switch_mmio_write32(car, RST_DEV_Y_SET, kClkY);
    switch_mmio_write32(car, RST_DEV_V_SET, kClkV);
    switch_mmio_write32(car, RST_DEV_L_SET, kClkL);
    switch_mmio_write32(car, CLK_ENB_Y_SET, kClkY);
    switch_mmio_write32(car, CLK_ENB_V_SET, kClkV);
    switch_mmio_write32(car, CLK_ENB_L_SET, kClkL);
    IODelay(10);
    if (!switch_pmc_partition_on(kPartitionAudId))
        return false;
    IODelay(10);
    switch_pmc_remove_clamping(kPartitionAudId);
    return switch_mmio_wait32(pmc, CLAMP_STATUS, kPartitionAud, 0, 5000);
}

// ticks_per_frame_q32 (and the stats' period length) for `clock`.
void SwitchAudio::noteClock()
{
    control->rate = clock->rate;
    control->ticks_per_frame_q32 = tegraTicksPerFrame(38400000);
#if DEVELOPMENT || DEBUG
    periodTicksQ16 = static_cast<uint64_t>(kOutPeriodFrames) * control->ticks_per_frame_q32 >> 16;
#endif
}

IOReturn SwitchAudio::rateGated(OSObject *owner, void *next, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (self->control->state) {
        switch_refusal("audio", "rate-while-running", "%u Hz asked while a stream runs at %u Hz",
                       static_cast<const switch_audio_clock *>(next)->rate, self->clock->rate);
        return kIOReturnBusy;
    }
    self->clock = static_cast<const switch_audio_clock *>(next);
    if (!self->hardwareLost)
        self->startClocks();
    self->noteClock();
    return kIOReturnSuccess;
}

// Both directions share the I2S clock, so the rate changes only between streams, and the codec, which runs from
// MCLK, is powered down first rather than left muted on a clock that stops and comes back at another rate.
IOReturn SwitchAudio::setRate(uint32_t rate)
{
    const switch_audio_clock *next = nullptr;
    for (const switch_audio_clock &c : kClocks) {
        if (c.rate == rate && (tegra || c.rate == SWITCH_AUDIO_RATE))
            next = &c;
    }
    if (!next) {
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("audio", "rate"), rate), "audio", "rate",
                         "%u Hz asked; the %s backend runs %u%s", rate, tegra ? "Tegra" : "null", SWITCH_AUDIO_RATE,
                         tegra ? " and 44100 Hz" : " Hz");
        return kIOReturnUnsupported;
    }
    if (next == clock)
        return kIOReturnSuccess;
    codecStep(kCodecIdleNow);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the gate passes void *; rateGated reads it as const
    return gate->runAction(&SwitchAudio::rateGated, const_cast<switch_audio_clock *>(next));
}
