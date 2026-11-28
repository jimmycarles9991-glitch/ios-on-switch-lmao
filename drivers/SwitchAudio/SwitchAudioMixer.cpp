// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kBase = kMixer1Base;
constexpr uint32_t kTimeoutUs = 10000u;

// RX1 gain RAM words 0..13: 9 linear ramp poly coeffs, unity Q16 gain, instant duration {1,1,1,1}. Word 15 triggers.
constexpr uint32_t kRx1GainWords[14] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x01000000u, 0u, kMixerGainUnityQ16, 1u, 1u, 1u, 1u,
};
constexpr uint32_t kMixerGainTriggerAddr = 0xfu;

bool writeGainRam(volatile uint8_t *ahub, uint32_t addr, uint32_t value)
{
    if (!waitReg(ahub, kBase + kMixerGainRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerGainRamCtrl, kRamCtrlWriteSeq | (addr & 0x1ffu));
    switch_mmio_write32(ahub, kBase + kMixerGainRamData, value);
    return true;
}

}  // namespace

bool mixerConfigure(volatile uint8_t *ahub, uint32_t channels)
{
    if (!ahub || channels < 1u || channels > 8u) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 0u);
    if (!waitReg(ahub, kBase + kMixerTx1Status, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    // MIXER_ENABLE must be asserted before writing GAIN_CFG_RAM (matches tegra210_mixer_runtime_resume).
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 1u);
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, kBase + kMixerRxCif, cif);
    switch_mmio_write32(ahub, kBase + kMixerTx1Cif, cif);
    switch_mmio_write32(ahub, kBase + kMixerTx1AdderCfg, 1u);
    const uint32_t rxCtrl = switch_mmio_read32(ahub, kBase + kMixerRxCtrl);
    switch_mmio_write32(ahub, kBase + kMixerRxCtrl, rxCtrl & ~kMixerRxSampleCountEn);
    switch_mmio_write32(ahub, kBase + kMixerRxCtrl, rxCtrl | kMixerRxSampleCountEn);
    for (uint32_t addr = 0; addr < 14u; ++addr) {
        if (!writeGainRam(ahub, addr, kRx1GainWords[addr])) {
            return false;
        }
    }
    return writeGainRam(ahub, kMixerGainTriggerAddr, 1u);
}

void mixerEnable(volatile uint8_t *ahub)
{
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 1u);
    // MIXER_ENABLE resets to 0 in hardware and gates every RX/TX path.
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 1u);
}

bool mixerStop(volatile uint8_t *ahub)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_rmw32(ahub, kBase + kMixerRxCtrl, kMixerRxSampleCountEn, 0u);
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 0u);
    const bool stopped = waitReg(ahub, kBase + kMixerTx1Status, 1u, 0u, kTimeoutUs);
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 0u);
    return stopped;
}

// NOLINTNEXTLINE(misc-const-correctness): signature fixed in SwitchAudioAhub.h
uint32_t mixerSampleCount(volatile uint8_t *ahub)
{
    return switch_mmio_read32(ahub, kBase + kMixerRxSampleCount);
}

}  // namespace switch_audio_ahub
