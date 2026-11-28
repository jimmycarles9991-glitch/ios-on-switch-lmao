// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 AHUB hardware offload modules (AXBAR, SFC, MIXER1, MVC1, OPE1 [PEQ1 + MBDRC1]).
// Offsets and bitfields verified against Tegra X1 TRM Ch. 23 and mainline Linux sound/soc/tegra/tegra210_*.h.
// Built against kits/SwitchMMIO/SwitchMMIO.h so host unit tests can exercise every module with SWITCH_MMIO_HOST_MODEL.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchAudioFormat.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

namespace switch_audio_ahub {

// Module offsets inside AHUB (0x702D0000, size 0xD000).
constexpr uint32_t kXbarBase = 0x0800;
constexpr uint32_t kSfc1Base = 0x2000;
constexpr uint32_t kSfc2Base = 0x2200;
constexpr uint32_t kOpe1Base = 0x8000;
constexpr uint32_t kPeq1Base = 0x8100;
constexpr uint32_t kMbdrc1Base = 0x8200;
constexpr uint32_t kMvc1Base = 0xa000;
constexpr uint32_t kMixer1Base = 0xbb00;

// Shared AHUB RAM control bits (SFC, MVC, PEQ, MBDRC, MIXER).
constexpr uint32_t kRamCtrlReadBusy = 1u << 31;
constexpr uint32_t kRamCtrlRwWrite = 1u << 14;
constexpr uint32_t kRamCtrlAddrInitEn = 1u << 13;
constexpr uint32_t kRamCtrlSeqAccessEn = 1u << 12;
constexpr uint32_t kRamCtrlWriteSeq = kRamCtrlRwWrite | kRamCtrlAddrInitEn | kRamCtrlSeqAccessEn;

// AXBAR destination RX offsets (relative to kXbarBase; parts 0/1/2 spaced 0x200 apart).
constexpr uint32_t kXbarPartStride = 0x200;
constexpr uint32_t kXbarDstAdmaif1 = 0x000;
constexpr uint32_t kXbarDstI2s1 = 0x040;
constexpr uint32_t kXbarDstSfc1 = 0x060;
constexpr uint32_t kXbarDstSfc2 = 0x064;
constexpr uint32_t kXbarDstMixer1Rx1 = 0x080;
constexpr uint32_t kXbarDstOpe1 = 0x100;
constexpr uint32_t kXbarDstMvc1 = 0x120;

// AXBAR source TX identifiers: {part, bit} (TRM Ch. 23.3.1, pp. 1700-1721).
struct XbarSource {
    uint32_t part;
    uint32_t mask;
};

constexpr XbarSource kSrcNone = {0, 0};
constexpr XbarSource kSrcAdmaif1 = {0, 1u << 0};
constexpr XbarSource kSrcI2s1 = {0, 1u << 16};
constexpr XbarSource kSrcSfc1 = {0, 1u << 24};
constexpr XbarSource kSrcSfc2 = {0, 1u << 25};
constexpr XbarSource kSrcMixer1Tx1 = {1, 1u << 0};
constexpr XbarSource kSrcOpe1 = {2, 1u << 0};
constexpr XbarSource kSrcMvc1 = {2, 1u << 8};

// SFC registers (relative to kSfc1Base / kSfc2Base).
constexpr uint32_t kSfcRxCif = 0x20;
constexpr uint32_t kSfcRxFreq = 0x24;
constexpr uint32_t kSfcTxCif = 0x60;
constexpr uint32_t kSfcTxFreq = 0x64;
constexpr uint32_t kSfcEnable = 0x80;
constexpr uint32_t kSfcSoftReset = 0x84;
constexpr uint32_t kSfcCg = 0x88;
constexpr uint32_t kSfcStatus = 0x8c;
constexpr uint32_t kSfcCoefRam = 0xbc;
constexpr uint32_t kSfcCfgRamCtrl = 0xc0;
constexpr uint32_t kSfcCfgRamData = 0xc4;
constexpr uint32_t kSfcCoefDepth = 64;

// MIXER1 registers (relative to kMixer1Base).
constexpr uint32_t kMixerRxStride = 0x40;
constexpr uint32_t kMixerRxSoftReset = 0x04;
constexpr uint32_t kMixerRxStatus = 0x10;
constexpr uint32_t kMixerRxCif = 0x24;
constexpr uint32_t kMixerRxCtrl = 0x28;
constexpr uint32_t kMixerRxPeakCtrl = 0x2c;
constexpr uint32_t kMixerRxSampleCount = 0x30;
constexpr uint32_t kMixerTx1Enable = 0x280;
constexpr uint32_t kMixerTx1SoftReset = 0x284;
constexpr uint32_t kMixerTx1Status = 0x290;
constexpr uint32_t kMixerTx1Cif = 0x2a4;
constexpr uint32_t kMixerTx1AdderCfg = 0x2a8;
constexpr uint32_t kMixerGlobalEnable = 0x400;
constexpr uint32_t kMixerGlobalSoftReset = 0x404;
constexpr uint32_t kMixerGlobalStatus = 0x410;
constexpr uint32_t kMixerGainRamCtrl = 0x42c;
constexpr uint32_t kMixerGainRamData = 0x430;
constexpr uint32_t kMixerRxSampleCountEn = 1u << 24;
constexpr uint32_t kMixerGainUnityQ16 = 0x10000u;

// MVC1 registers (relative to kMvc1Base).
constexpr uint32_t kMvcRxCif = 0x20;
constexpr uint32_t kMvcTxCif = 0x60;
constexpr uint32_t kMvcEnable = 0x80;
constexpr uint32_t kMvcSoftReset = 0x84;
constexpr uint32_t kMvcCg = 0x88;
constexpr uint32_t kMvcStatus = 0x90;
constexpr uint32_t kMvcCtrl = 0xa8;
constexpr uint32_t kMvcSwitch = 0xac;
constexpr uint32_t kMvcInitVol = 0xb0;
constexpr uint32_t kMvcTargetVol = 0xd0;
constexpr uint32_t kMvcDuration = 0xf0;
constexpr uint32_t kMvcDurationInv = 0xf4;
constexpr uint32_t kMvcPolyN1 = 0xf8;
constexpr uint32_t kMvcPolyN2 = 0xfc;
constexpr uint32_t kMvcPeakCtrl = 0x100;
constexpr uint32_t kMvcCfgRamCtrl = 0x104;
constexpr uint32_t kMvcCfgRamData = 0x108;
constexpr uint32_t kMvcPeakValue = 0x10c;
constexpr uint32_t kMvcConfigErr = 0x12c;
constexpr uint32_t kMvcCtrlPerChanEn = 1u << 30;
constexpr uint32_t kMvcCtrlMuteShift = 8;
constexpr uint32_t kMvcCtrlMuteMask = 0xffu << 8;
constexpr uint32_t kMvcCtrlCurveLinear = 1u << 1;
constexpr uint32_t kMvcCtrlRoundingUnbias = 1u << 0;
constexpr uint32_t kMvcSwitchVolume = 1u << 2;

// OPE1 / PEQ1 / MBDRC1 registers (relative to kOpe1Base, kPeq1Base, kMbdrc1Base).
constexpr uint32_t kOpeRxCif = 0x20;
constexpr uint32_t kOpeTxCif = 0x60;
constexpr uint32_t kOpeEnable = 0x80;
constexpr uint32_t kOpeSoftReset = 0x84;
constexpr uint32_t kOpeCg = 0x88;
constexpr uint32_t kOpeStatus = 0x8c;
constexpr uint32_t kOpeDir = 0x94;
constexpr uint32_t kOpeDirPeqToMbdrc = 1u;

constexpr uint32_t kPeqCfg = 0x0c;
constexpr uint32_t kPeqRamCtrl = 0x10;
constexpr uint32_t kPeqRamData = 0x14;
constexpr uint32_t kPeqShiftCtrl = 0x18;
constexpr uint32_t kPeqShiftData = 0x1c;
constexpr uint32_t kPeqCfgModeActive = 1u << 0;
constexpr uint32_t kPeqCfgUnbias = 1u << 1;
constexpr uint32_t kPeqCfgStagesShift = 2;
constexpr uint32_t kPeqGainWordsPerCh = 62;
constexpr uint32_t kPeqShiftWordsPerCh = 14;

constexpr uint32_t kMbdrcSoftReset = 0x04;
constexpr uint32_t kMbdrcCfg = 0x28;
constexpr uint32_t kMbdrcChanMask = 0x2c;
constexpr uint32_t kMbdrcMasterVol = 0x30;
constexpr uint32_t kMbdrcFastFactor = 0x34;
constexpr uint32_t kMbdrcIirCfg = 0x38;
constexpr uint32_t kMbdrcInAttack = 0x44;
constexpr uint32_t kMbdrcInRelease = 0x50;
constexpr uint32_t kMbdrcFastAttack = 0x5c;
constexpr uint32_t kMbdrcInThresh = 0x68;
constexpr uint32_t kMbdrcOutThresh = 0x74;
constexpr uint32_t kMbdrcRatio1st = 0x80;
constexpr uint32_t kMbdrcRatio2nd = 0x8c;
constexpr uint32_t kMbdrcRatio3rd = 0x98;
constexpr uint32_t kMbdrcRatio4th = 0xa4;
constexpr uint32_t kMbdrcRatio5th = 0xb0;
constexpr uint32_t kMbdrcMakeupGain = 0xbc;
constexpr uint32_t kMbdrcInitGain = 0xc8;
constexpr uint32_t kMbdrcGainAttack = 0xd4;
constexpr uint32_t kMbdrcGainRelease = 0xe0;
constexpr uint32_t kMbdrcFastRelease = 0xec;
constexpr uint32_t kMbdrcRamCtrl = 0xf8;
constexpr uint32_t kMbdrcRamData = 0x104;
constexpr uint32_t kMbdrcBands = 3;
constexpr uint32_t kMbdrcWordsPerBand = 40;

inline void xbarConnect(volatile uint8_t *ahub, uint32_t dst, XbarSource src)
{
    for (uint32_t part = 0; part < 3; ++part)
        switch_mmio_write32(ahub, kXbarBase + dst + part * kXbarPartStride, part == src.part ? src.mask : 0u);
}

inline void xbarRoutePlayback(volatile uint8_t *ahub, bool useSfc)
{
    xbarConnect(ahub, kXbarDstMixer1Rx1, kSrcAdmaif1);
    if (useSfc) {
        xbarConnect(ahub, kXbarDstSfc1, kSrcMixer1Tx1);
        xbarConnect(ahub, kXbarDstMvc1, kSrcSfc1);
    } else {
        xbarConnect(ahub, kXbarDstSfc1, kSrcNone);
        xbarConnect(ahub, kXbarDstMvc1, kSrcMixer1Tx1);
    }
    xbarConnect(ahub, kXbarDstOpe1, kSrcMvc1);
    xbarConnect(ahub, kXbarDstI2s1, kSrcOpe1);
}

inline void xbarRouteCapture(volatile uint8_t *ahub, bool useSfc)
{
    if (useSfc) {
        xbarConnect(ahub, kXbarDstSfc2, kSrcI2s1);
        xbarConnect(ahub, kXbarDstAdmaif1, kSrcSfc2);
    } else {
        xbarConnect(ahub, kXbarDstSfc2, kSrcNone);
        xbarConnect(ahub, kXbarDstAdmaif1, kSrcI2s1);
    }
}

#ifdef SWITCH_MMIO_HOST_MODEL
inline bool waitReg(const volatile uint8_t *ahub, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs)
{
    const uint32_t limit = timeoutUs < 1024u ? timeoutUs : 1024u;
    for (uint32_t i = 0; i < limit; ++i) {
        if ((switch_mmio_read32(ahub, offset) & mask) == value)
            return true;
    }
    return (switch_mmio_read32(ahub, offset) & mask) == value;
}
#else
bool waitReg(const volatile uint8_t *ahub, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs);
#endif

// SFC (SwitchAudioSfc.cpp): returns false if rate pair is unsupported or reset times out.
bool sfcSupportsPair(uint32_t inRate, uint32_t outRate);
const uint32_t *sfcCoefficients(uint32_t inRate, uint32_t outRate);
bool sfcConfigure(volatile uint8_t *ahub, uint32_t base, uint32_t inRate, uint32_t outRate, uint32_t channels);
void sfcEnable(volatile uint8_t *ahub, uint32_t base);
bool sfcStop(volatile uint8_t *ahub, uint32_t base);

// MIXER1 (SwitchAudioMixer.cpp): unity mix on RX1 -> TX1 plus free-running hardware SAMPLE_COUNT.
bool mixerConfigure(volatile uint8_t *ahub, uint32_t channels);
void mixerEnable(volatile uint8_t *ahub);
bool mixerStop(volatile uint8_t *ahub);
uint32_t mixerSampleCount(volatile uint8_t *ahub);

// MVC1 (SwitchAudioMvc.cpp): click-free LINEAR_RAMP volume/mute and hardware peak meter.
bool mvcConfigure(volatile uint8_t *ahub, uint32_t channels, int32_t initialMdB, bool muted);
void mvcEnable(volatile uint8_t *ahub);
bool mvcStop(volatile uint8_t *ahub);
bool mvcSetVolume(volatile uint8_t *ahub, int32_t targetMdB, bool muted, bool immediate);
void mvcReadPeakChannels(volatile uint8_t *ahub, uint32_t *left, uint32_t *right);
uint32_t mvcReadPeak(volatile uint8_t *ahub);
uint32_t mvcReadConfigErr(volatile uint8_t *ahub);

// OPE1 (SwitchAudioOpe.cpp): Switch OLED Q2.30 PEQ (MBDRC bypassed) for speakers, full bypass for headphones.
bool opeConfigure(volatile uint8_t *ahub, uint32_t linkRate, bool speakerActive, uint32_t channels);
void opeEnable(volatile uint8_t *ahub);
bool opeStop(volatile uint8_t *ahub);

}  // namespace switch_audio_ahub
