// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"
#ifndef SWITCH_MMIO_HOST_MODEL
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#endif

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kOpeBase = kOpe1Base;
constexpr uint32_t kPeqBase = kPeq1Base;
constexpr uint32_t kMbdrcBase = kMbdrc1Base;
constexpr uint32_t kTimeoutUs = 10000u;

// Switch OLED speaker PEQ matching Nintendo's eq_speakers_list_frig (-1.90 dB overall, 40 Hz Butterworth HPF, 360 Hz
// +1.0 dB shelf). TRM Ch. 23.2.12 negates a1/a2; both stages use shift 30 with 0.5x feedforward scaling (matching
// mainline tegra210_peq.c) and post-gain 4.0x at shift 28 so intermediate values never clip and feedback is fixed Q2.30.
constexpr int32_t kPeqPreGainOledQ30 = 862978048;
constexpr int32_t kPeqPostGain4xQ28 = 1073741824;
constexpr uint32_t kPeqMaxChannels = 8u;
constexpr uint32_t kPeqTotalStages = 12u;
constexpr uint32_t kPeqActiveStages = 2u;

constexpr int32_t kPeqStage0_48k[5] = {534886875, -1069773750, 534886875, 2139532835, -1065820340};
constexpr int32_t kPeqStage1_48k[5] = {601224974, -1163531725, 563527478, 2073864729, -1002564362};

constexpr int32_t kPeqStage0_44k[5] = {534711769, -1069423538, 534711769, 2138829710, -1065122620};
constexpr int32_t kPeqStage1_44k[5] = {601123097, -1159895854, 560214663, 2067359731, -996501718};

constexpr int32_t kPeqStageBypass[5] = {1, 0, 0, 0, 0};

constexpr uint32_t kPeqShiftTable[kPeqShiftWordsPerCh] = {
    30u, 30u, 30u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 28u,
};

bool waitReg(const volatile uint8_t *ahub, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    const uint32_t limit = timeoutUs < 1024u ? timeoutUs : 1024u;
    for (uint32_t i = 0; i < limit; ++i) {
        if ((switch_mmio_read32(ahub, offset) & mask) == value) {
            return true;
        }
    }
    return (switch_mmio_read32(ahub, offset) & mask) == value;
#else
    return switch_mmio_wait32(ahub, offset, mask, value, timeoutUs);
#endif
}

void writePeqStage(volatile uint8_t *ahub, const int32_t stage[5])
{
    for (uint32_t i = 0; i < 5u; ++i) {
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(stage[i]));
    }
}

bool configurePeqSpeaker(volatile uint8_t *ahub, uint32_t linkRate)
{
    const bool rate44k = switch_audio_is_44k_family(linkRate) != 0;
    const int32_t *const stage0 = rate44k ? kPeqStage0_44k : kPeqStage0_48k;
    const int32_t *const stage1 = rate44k ? kPeqStage1_44k : kPeqStage1_48k;

    for (uint32_t ch = 0; ch < kPeqMaxChannels; ++ch) {
        if (!waitReg(ahub, kPeqBase + kPeqRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqRamCtrl, kRamCtrlWriteSeq | (ch * kPeqGainWordsPerCh));
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(kPeqPreGainOledQ30));
        writePeqStage(ahub, stage0);
        writePeqStage(ahub, stage1);
        for (uint32_t s = kPeqActiveStages; s < kPeqTotalStages; ++s) {
            writePeqStage(ahub, kPeqStageBypass);
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(kPeqPostGain4xQ28));

        if (!waitReg(ahub, kPeqBase + kPeqShiftCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqShiftCtrl, kRamCtrlWriteSeq | (ch * kPeqShiftWordsPerCh));
        for (const uint32_t shift : kPeqShiftTable) {
            switch_mmio_write32(ahub, kPeqBase + kPeqShiftData, shift);
        }
    }

    const uint32_t cfg = ((kPeqActiveStages - 1u) << kPeqCfgStagesShift) | kPeqCfgUnbias | kPeqCfgModeActive;
    switch_mmio_write32(ahub, kPeqBase + kPeqCfg, cfg);
    return true;
}

}  // namespace

bool opeConfigure(volatile uint8_t *ahub, uint32_t linkRate, bool speakerActive, uint32_t channels)
{
    if (!ahub || channels < 1u || channels > 8u) {
        return false;
    }
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 0u);
    if (!waitReg(ahub, kOpeBase + kOpeStatus, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    // Reset OPE1 as a whole before programming (TRM Ch. 23.2.12 forbids standalone PEQ_SOFT_RESET).
    switch_mmio_write32(ahub, kOpeBase + kOpeSoftReset, 1u);
    if (!waitReg(ahub, kOpeBase + kOpeSoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, kOpeBase + kOpeRxCif, cif);
    switch_mmio_write32(ahub, kOpeBase + kOpeTxCif, cif);
    switch_mmio_write32(ahub, kOpeBase + kOpeDir, kOpeDirPeqToMbdrc);
    // Keep MBDRC bypassed (matches Nintendo's OLED speaker profile where codec DRC is off).
    switch_mmio_write32(ahub, kMbdrcBase + kMbdrcCfg, 0u);

    if (!speakerActive) {
        switch_mmio_write32(ahub, kPeqBase + kPeqCfg, kPeqCfgUnbias);
        return true;
    }
    return configurePeqSpeaker(ahub, linkRate);
}

void opeEnable(volatile uint8_t *ahub)
{
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 1u);
}

bool opeStop(volatile uint8_t *ahub)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 0u);
    return waitReg(ahub, kOpeBase + kOpeStatus, 1u, 0u, kTimeoutUs);
}

}  // namespace switch_audio_ahub
