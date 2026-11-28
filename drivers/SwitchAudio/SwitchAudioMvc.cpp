// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kBase = kMvc1Base;
constexpr uint32_t kTimeoutUs = 10000u;
constexpr uint32_t kRampDurationFrames = 720u;  // 15 ms at 48 kHz
constexpr uint32_t kRampDurationInv = (1u << 31) / kRampDurationFrames;
constexpr uint32_t kPolyN1 = 16u;
constexpr uint32_t kPolyN2 = 63u;
constexpr uint32_t kPeakCtrlWindowed = 0x000012c0u;
constexpr uint32_t kMvcMaxChannels = 8u;

constexpr int32_t kDefaultPolyCoeffs[9] = {
    23738319, 659403, -3680, 15546680, 2530732, -120985, 12048422, 5527252, -785042,
};

bool writeCfgRam(volatile uint8_t *ahub, uint32_t addr, uint32_t value)
{
    if (!waitReg(ahub, kBase + kMvcCfgRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMvcCfgRamCtrl, kRamCtrlWriteSeq | (addr & 0x1ffu));
    switch_mmio_write32(ahub, kBase + kMvcCfgRamData, value);
    return true;
}

uint32_t ctrlWord(bool muted)
{
    return kMvcCtrlPerChanEn | kMvcCtrlCurveLinear | kMvcCtrlRoundingUnbias | (muted ? kMvcCtrlMuteMask : 0u);
}

}  // namespace

bool mvcConfigure(volatile uint8_t *ahub, uint32_t channels, int32_t initialMdB, bool muted)
{
    if (!ahub || channels < 1u || channels > 8u) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMvcEnable, 0u);
    if (!waitReg(ahub, kBase + kMvcStatus, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMvcSoftReset, 1u);
    if (!waitReg(ahub, kBase + kMvcSoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, kBase + kMvcRxCif, cif);
    switch_mmio_write32(ahub, kBase + kMvcTxCif, cif);
    switch_mmio_write32(ahub, kBase + kMvcDuration, kRampDurationFrames);
    switch_mmio_write32(ahub, kBase + kMvcDurationInv, kRampDurationInv);
    switch_mmio_write32(ahub, kBase + kMvcPolyN1, kPolyN1);
    switch_mmio_write32(ahub, kBase + kMvcPolyN2, kPolyN2);
    for (uint32_t i = 0; i < 9u; ++i) {
        if (!writeCfgRam(ahub, i, static_cast<uint32_t>(kDefaultPolyCoeffs[i]))) {
            return false;
        }
    }
    switch_mmio_write32(ahub, kBase + kMvcCtrl, ctrlWord(muted));
    const uint32_t vol = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(initialMdB));
    for (uint32_t ch = 0; ch < kMvcMaxChannels; ++ch) {
        switch_mmio_write32(ahub, kBase + kMvcInitVol + 4u * ch, vol);
        switch_mmio_write32(ahub, kBase + kMvcTargetVol + 4u * ch, vol);
    }
    switch_mmio_write32(ahub, kBase + kMvcPeakCtrl, kPeakCtrlWindowed);
    switch_mmio_write32(ahub, kBase + kMvcSwitch, kMvcSwitchVolume);
    return true;
}

void mvcEnable(volatile uint8_t *ahub)
{
    switch_mmio_write32(ahub, kBase + kMvcEnable, 1u);
}

bool mvcStop(volatile uint8_t *ahub)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMvcEnable, 0u);
    return waitReg(ahub, kBase + kMvcStatus, 1u, 0u, kTimeoutUs);
}

bool mvcSetVolume(volatile uint8_t *ahub, int32_t targetMdB, bool muted, bool immediate)
{
    if (!ahub) {
        return false;
    }
    if (!waitReg(ahub, kBase + kMvcSwitch, kMvcSwitchVolume, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMvcCtrl, ctrlWord(muted));
    const uint32_t vol = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(targetMdB));
    for (uint32_t ch = 0; ch < kMvcMaxChannels; ++ch) {
        if (immediate) {
            switch_mmio_write32(ahub, kBase + kMvcInitVol + 4u * ch, vol);
        }
        switch_mmio_write32(ahub, kBase + kMvcTargetVol + 4u * ch, vol);
    }
    switch_mmio_write32(ahub, kBase + kMvcSwitch, kMvcSwitchVolume);
    return true;
}

// NOLINTNEXTLINE(misc-const-correctness): signature fixed in SwitchAudioAhub.h
void mvcReadPeakChannels(volatile uint8_t *ahub, uint32_t *left, uint32_t *right)
{
    const uint32_t ch0 = switch_mmio_read32(ahub, kBase + kMvcPeakValue);
    const uint32_t ch1 = switch_mmio_read32(ahub, kBase + kMvcPeakValue + 4u);
    if (left) {
        *left = ch0;
    }
    if (right) {
        *right = ch1;
    }
}

// NOLINTNEXTLINE(misc-const-correctness): signature fixed in SwitchAudioAhub.h
uint32_t mvcReadPeak(volatile uint8_t *ahub)
{
    uint32_t ch0 = 0u;
    uint32_t ch1 = 0u;
    mvcReadPeakChannels(ahub, &ch0, &ch1);
    return ch0 > ch1 ? ch0 : ch1;
}

// NOLINTNEXTLINE(misc-const-correctness): signature fixed in SwitchAudioAhub.h
uint32_t mvcReadConfigErr(volatile uint8_t *ahub)
{
    return switch_mmio_read32(ahub, kBase + kMvcConfigErr);
}

}  // namespace switch_audio_ahub
