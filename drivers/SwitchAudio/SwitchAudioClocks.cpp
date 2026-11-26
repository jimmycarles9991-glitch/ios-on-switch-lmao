// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: APE power-gating, CAR/PMC clocks and pads, AHUB/ADMA/AGIC setup, and multi-rate switching.
#include "SwitchAudioDriver.h"

using namespace switch_audio_hw;

namespace {
constexpr unsigned kApeClocks[] = {kClkIdApe, kClkIdDAudio, kClkIdApb2Ape, kClkIdExtern1, kClkIdI2s0};

bool waitPmcClear(uint32_t offset, uint32_t mask, uint32_t timeoutUs)
{
    if ((switch_pmc_read(offset) & mask) == 0u)
        return true;
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeoutUs, NSEC_PER_USEC, &deadline);
    while (mach_absolute_time() < deadline) {
        if ((switch_pmc_read(offset) & mask) == 0u)
            return true;
        IODelay(1);
    }
    return (switch_pmc_read(offset) & mask) == 0u;
}
}  // namespace

// Programs PLLA and PLLA_OUT0 for `clock` in clk-pll.c SDM lock order, leaving them untouched if already locked to it.
void SwitchAudio::startPlla()
{
    const uint32_t mnp = pllaMnp(*clock);
    const uint32_t sdm = clock->sdm & 0xffffu;
    const uint32_t base = switch_car_read(PLLA_BASE);
    const bool running = (base & (PLL_ENABLE | PLL_BYPASS | PLLA_IDDQ)) == PLL_ENABLE &&
                         (base & PLLA_MNP_MASK) == mnp && ((switch_car_read(PLLA_MISC2) & PLLA_EN_SDM) != 0u) &&
                         switch_car_read(PLLA_MISC1) == sdm;
    if (!running) {
        switch_car_rmw(PLLA_OUT, 3u, 0);
        switch_car_write(PLLA_BASE, (base & ~PLL_ENABLE) | PLLA_IDDQ);
        switch_car_write(PLLA_MISC0, PLLA_LOCK_ENABLE);
        switch_car_write(PLLA_MISC2, 0);
        switch_car_rmw(PLLA_BASE, PLL_BYPASS | PLLA_MNP_MASK, mnp);
        // Full 32-bit write clears stale bootloader setup bits in upper 16 bits of PLLA_MISC1.
        switch_car_write(PLLA_MISC1, sdm);
        switch_car_write(PLLA_MISC2, PLLA_EN_SDM);
        IODelay(1);
        switch_car_rmw(PLLA_BASE, PLLA_IDDQ, 0);
        IODelay(5);
        switch_car_rmw(PLLA_BASE, 0, PLL_ENABLE);
        if (!switch_car_wait(PLLA_BASE, PLL_LOCK, PLL_LOCK, 600)) {
            panic("SwitchAudio: PLLA did not lock: BASE %#x MISC0 %#x MISC1 %#x MISC2 %#x", switch_car_read(PLLA_BASE),
                  switch_car_read(PLLA_MISC0), switch_car_read(PLLA_MISC1), switch_car_read(PLLA_MISC2));
        }
        IODelay(50);
    }
    const uint32_t out0Div = clock->out0_x2 - 2;
    switch_car_rmw(PLLA_OUT, 0xf000ff03u, out0Div << 8);
    switch_car_rmw(PLLA_OUT, 0, 3u);
}

void SwitchAudio::stopPlla()
{
    switch_car_rmw(PLLA_OUT, 3u, 0);
    switch_car_rmw(PLLA_BASE, PLL_ENABLE, PLLA_IDDQ);
}

void SwitchAudio::startClocks()
{
    startPlla();
    switch_car_write(CLK_SOURCE_I2S0, switch_audio_i2s_divider(clock->out0_per_fs));
    switch_car_write(CLK_SOURCE_EXTERN1, switch_audio_extern1_divider(clock->out0_per_fs));
}

void SwitchAudio::tegraClocksAndPads()
{
    const uint32_t osc = switch_car_read(OSC_CTRL);
    if ((osc >> 28) != 5 || ((osc >> 26) & 3))
        panic("SwitchAudio: unexpected OSC_CTRL %#x (want 38.4 MHz, ref /1)", osc);
    if (!switch_pmc_partition_is_on(kPartitionAudId)) {
        panic("SwitchAudio: APE power partition is off (PMC PWRGATE_STATUS %#x)", switch_pmc_read(PWRGATE_STATUS));
    }
    if ((switch_pmc_read(IO_DPD_STATUS) & (1u << 17)) || (switch_pmc_read(IO_DPD2_STATUS) & (1u << 29))) {
        panic("SwitchAudio: audio pads in deep power-down (IO_DPD_STATUS %#x IO_DPD2_STATUS %#x)",
              switch_pmc_read(IO_DPD_STATUS), switch_pmc_read(IO_DPD2_STATUS));
    }
    if (switch_pmc_read(PWR_DET_VAL) & kPad33V) {
        switch_pmc_rmw(PWR_DET, 0, kPad33V);
        switch_pmc_rmw(PWR_DET_VAL, kPad33V, 0);
        IODelay(250);
    }
    if (switch_pmc_read(NO_IOPOWER) & kNoIoPowerAudioHv)
        switch_pmc_rmw(NO_IOPOWER, kNoIoPowerAudioHv, 0);

    startClocks();
    switch_car_write(CLK_SOURCE_APE, kApeSource);
    switch_car_write(CLK_SOURCE_D_AUDIO, 0);
    for (const unsigned id : kApeClocks)
        switch_car_clock_enable(id);
    IODelay(2);
    for (const unsigned id : kApeClocks)
        switch_car_reset_deassert(id);
    (void)switch_car_reset_asserted(kClkIdI2s0);
    IODelay(2);
    switch_pmc_rmw(CLK_OUT_CNTRL, kClkOut1Mask, kClkOut1Extern1);
    jackPinFound = switch_mmio_read32(pinmux, kPinJackDetect);
    for (const Pin &pin : kPins) {
        switch_mmio_write32(pinmux, pin.offset, (switch_mmio_read32(pinmux, pin.offset) & ~kPinFields) | pin.value);
    }
}

void SwitchAudio::tegraAhubAndDma()
{
    switch_mmio_write32(ahub, ADMAIF_GLOBAL_ENABLE, 1);
    switch_audio_ahub::xbarRoutePlayback(ahub, false);
    switch_audio_ahub::xbarRouteCapture(ahub, false);
    switch_mmio_write32(adma, GLOBAL_INT_CLEAR, 1);
    switch_mmio_write32(adma, GLOBAL_SOFT_RESET, 1);
    if (!switch_mmio_wait32(adma, GLOBAL_SOFT_RESET, 1, 0, 10000))
        panic("SwitchAudio: ADMA soft reset did not finish");
    switch_mmio_write32(adma, GLOBAL_CMD, 1);
}

void SwitchAudio::tegraAgic()
{
    const uint32_t mask = switch_mmio_read32(agic, GICD_ITARGETSR) & 0xff;
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
        switch_mmio_write32(agic, cfg, switch_mmio_read32(agic, cfg) & ~(2u << (2 * (id % 16))));
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
    pinmux = map(kPinmuxPA, 0x1000, "pinmux");
    ahub = map(kAhubPA, kAhubSize, "AHUB");
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
    IOService *nub = switch_interrupt_nub(kCascadeIntid, false);
    cascade = IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::cascadeFired, nub, 0);
    if (!cascade || loop->addEventSource(cascade) != kIOReturnSuccess) {
        panic("SwitchAudio: cannot attach the APE interrupt (INTID %u)", kCascadeIntid);
    }
    cascade->enable();
    if (!apeGate.start(SwitchEngineGateOps{"SwitchAudio", this, &SwitchAudio::apeGateOn, &SwitchAudio::apeGateOff}))
        panic("SwitchAudio: cannot start APE engine gate");
    if (!acquireApe())
        panic("SwitchAudio: cannot hold APE gate at boot");
}

void SwitchAudio::softReset(uint32_t reg, const char *what)
{
    switch_mmio_write32(ahub, reg, 1);
    if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000))
        panic("SwitchAudio: %s soft reset did not finish", what);
}

// I2S soft reset clears CIF/CTRL registers and completes on the bit clock, so I2S_CTRL must already select master.
void SwitchAudio::i2sSoftReset(uint32_t reg, const char *what)
{
    const uint32_t saved[] = {I2S_TX_CIF, I2S_RX_CIF, I2S_TX_CTRL, I2S_RX_CTRL, I2S_CTRL};
    uint32_t values[5];
    for (unsigned i = 0; i < 5; ++i)
        values[i] = switch_mmio_read32(ahub, saved[i]);
    switch_mmio_write32(ahub, reg, 1);
    if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000)) {
        panic("SwitchAudio: %s soft reset did not finish (I2S CTRL %#x EN %#x TIMING %#x RX %#x TX %#x; CAR I2S0 "
              "src %#x enb %u rst %u PLLA %#x OUT %#x)",
              what, switch_mmio_read32(ahub, I2S_CTRL), switch_mmio_read32(ahub, I2S_ENABLE),
              switch_mmio_read32(ahub, I2S_TIMING), switch_mmio_read32(ahub, I2S_RX_STATUS),
              switch_mmio_read32(ahub, I2S_TX_STATUS), switch_car_read(CLK_SOURCE_I2S0),
              switch_car_clock_enabled(kClkIdI2s0) ? 1u : 0u, switch_car_reset_asserted(kClkIdI2s0) ? 1u : 0u,
              switch_car_read(PLLA_BASE), switch_car_read(PLLA_OUT));
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

bool SwitchAudio::apePartitionOn()
{
    if (switch_pmc_partition_is_on(kPartitionAudId))
        return true;
    if (!switch_pmc_partition_on(kPartitionAudId))
        return false;
    IODelay(10);
    for (const unsigned id : kApeClocks)
        switch_car_clock_enable(id);
    for (const unsigned id : kApeClocks)
        switch_car_reset_assert(id);
    IODelay(10);
    // TRM 12.2.7: poll REMOVE_CLAMPING_CMD clearing (CLAMP_STATUS is legacy, TRM 12.6.12).
    switch_pmc_remove_clamping(kPartitionAudId);
    if (!waitPmcClear(PMC_REMOVE_CLAMPING, kPartitionAud, 5000)) {
        for (const unsigned id : kApeClocks)
            switch_car_clock_disable(id);
        IODelay(10);
        (void)switch_pmc_partition_off(kPartitionAudId);
        return false;
    }
    IODelay(10);
    for (const unsigned id : kApeClocks)
        switch_car_reset_deassert(id);
    IODelay(10);
    tegra_mc_hot_reset(kMcHotResetCtrl1, kMcHotResetApe, false);
    IODelay(10);
    return true;
}

bool SwitchAudio::apePartitionOff()
{
    if (!switch_pmc_partition_is_on(kPartitionAudId))
        return true;
    tegra_mc_hot_reset(kMcHotResetCtrl1, kMcHotResetApe, true);
    uint32_t settled = 0;
    for (uint32_t i = 0; i < 1000 && settled < 5; ++i) {
        if (!(tegra_smmu_read(kMcHotResetCtrl1) & kMcHotResetApe)) {
            tegra_mc_hot_reset(kMcHotResetCtrl1, kMcHotResetApe, false);
            return false;
        }
        settled = (tegra_smmu_read(kMcHotResetStatus1) & kMcHotResetApe) ? settled + 1 : 0;
        if (settled < 5)
            IODelay(10);
    }
    if (settled < 5) {
        tegra_mc_hot_reset(kMcHotResetCtrl1, kMcHotResetApe, false);
        return false;
    }
    switch_pmc_rmw(CLK_OUT_CNTRL, kClkOut1Mask, 0);
    for (const unsigned id : kApeClocks)
        switch_car_reset_assert(id);
    IODelay(10);
    for (const unsigned id : kApeClocks)
        switch_car_clock_disable(id);
    IODelay(10);
    stopPlla();
    return switch_pmc_partition_off(kPartitionAudId);
}

int SwitchAudio::apeGateOn(void *engine)
{
    auto *self = static_cast<SwitchAudio *>(engine);
    if (!apePartitionOn())
        return -1;
    self->tegraClocksAndPads();
    self->tegraAhubAndDma();
    self->tegraAgic();
    self->cascade->enable();
    return 0;
}

int SwitchAudio::apeGateOff(void *engine, bool drain)
{
    const auto *self = static_cast<const SwitchAudio *>(engine);
    if (!drain && self->control->state)
        return SWITCH_ENGINE_GATE_BUSY;
    self->cascade->disable();
    if (!apePartitionOff())
        return -1;
    return SWITCH_ENGINE_GATE_OFF;
}

bool SwitchAudio::acquireApe()
{
    if (hardwareLost)
        return false;
    // Always enter apeGate.use() first so concurrent callers wait for power-on to finish before accessing APE.
    if (apeGate.use() != 0)
        return false;
    if (__atomic_exchange_n(&apeHeld, true, __ATOMIC_ACQ_REL))
        apeGate.done();
    return true;
}

void SwitchAudio::releaseApe()
{
    if (__atomic_exchange_n(&apeHeld, false, __ATOMIC_ACQ_REL))
        apeGate.done();
}

void SwitchAudio::noteClock()
{
    control->rate = clientRate;
    control->ticks_per_frame_q32 = tegra ? tegraTicksPerFrame(38400000) : (hz << 32) / clientRate;
#if DEVELOPMENT || DEBUG
    periodTicksQ16 = static_cast<uint64_t>(kOutPeriodFrames) * control->ticks_per_frame_q32 >> 16;
#endif
}

IOReturn SwitchAudio::rateGated(OSObject *owner, void *rateArg, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const uint32_t rate = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rateArg));
    if (self->control->state) {
        switch_refusal("audio", "rate-while-running", "%u Hz asked while a stream runs at %u Hz", rate,
                       self->clientRate);
        return kIOReturnBusy;
    }
    const switch_audio_clock *wantLink = switch_audio_is_44k_family(rate) ? &kClocks[1] : &kClocks[0];
    const bool linkChanged = wantLink != self->clock;
    self->clock = wantLink;
    self->clientRate = rate;
    if (self->tegra && linkChanged && !self->hardwareLost && self->apeGate.powered())
        self->startClocks();
    self->noteClock();
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::setRate(uint32_t rate)
{
    bool supported = false;
    if (tegra) {
        for (const uint32_t r : kSupportedRates) {
            if (r == rate) {
                supported = true;
                break;
            }
        }
    } else {
        supported = (rate == SWITCH_AUDIO_RATE);
    }
    if (!supported) {
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("audio", "rate"), rate), "audio", "rate",
                         "%u Hz asked; unsupported by the %s backend", rate, tegra ? "Tegra" : "null");
        return kIOReturnUnsupported;
    }
    if (rate == clientRate)
        return kIOReturnSuccess;
    const switch_audio_clock *wantLink = switch_audio_is_44k_family(rate) ? &kClocks[1] : &kClocks[0];
    if (tegra && !control->state && wantLink != clock)
        codecStep(kCodecIdleNow);
    return gate->runAction(&SwitchAudio::rateGated, reinterpret_cast<void *>(static_cast<uintptr_t>(rate)));
}
