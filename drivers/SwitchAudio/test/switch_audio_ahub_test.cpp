// SPDX-License-Identifier: BSD-2-Clause-Patent
// Behavioral host-model tests for Tegra210 AHUB hardware offload engines (AXBAR, SFC, MIXER1, MVC1, OPE1 [PEQ1 + MBDRC1]).
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../SwitchAudioAhub.h"

namespace {

using namespace switch_audio_ahub;

struct RamCapture {
    bool active = false;
    uint32_t addr = 0;
    std::vector<uint32_t> words;
};

struct AhubModel {
    static constexpr uintptr_t kBase = 0x10000000u;
    static constexpr uint32_t kSize = 0xd000u;
    std::unordered_map<uint32_t, uint32_t> regs;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    bool freezeSoftReset = false;
    bool freezeStatus = false;
    bool freezeMvcSwitch = false;

    RamCapture sfc1Ram;
    RamCapture sfc2Ram;
    RamCapture mixerGainRam;
    RamCapture mvcRam;
    RamCapture peqGainRam;
    RamCapture peqShiftRam;
    std::array<RamCapture, kMbdrcBands> mbdrcRam;

    void reset()
    {
        regs.clear();
        writes.clear();
        freezeSoftReset = false;
        freezeStatus = false;
        freezeMvcSwitch = false;
        sfc1Ram = {};
        sfc2Ram = {};
        mixerGainRam = {};
        mvcRam = {};
        peqGainRam = {};
        peqShiftRam = {};
        mbdrcRam = {};
    }

    static volatile uint8_t *basePtr()
    {
        return reinterpret_cast<volatile uint8_t *>(kBase);
    }

    static bool isSoftResetReg(uint32_t off)
    {
        return off == kSfc1Base + kSfcSoftReset || off == kSfc2Base + kSfcSoftReset ||
               off == kMixer1Base + kMixerRxSoftReset || off == kMixer1Base + kMixerTx1SoftReset ||
               off == kMvc1Base + kMvcSoftReset || off == kOpe1Base + kOpeSoftReset;
    }

    static void startRam(RamCapture &ram, uint32_t ctrl, uint32_t capacity)
    {
        if (ram.words.size() < capacity) {
            ram.words.assign(capacity, 0u);
        }
        ram.active = (ctrl & kRamCtrlRwWrite) != 0u;
        ram.addr = ctrl & 0xfffu;
    }

    static void writeRam(RamCapture &ram, uint32_t value)
    {
        if (!ram.active) {
            return;
        }
        if (ram.addr < ram.words.size()) {
            ram.words[ram.addr] = value;
        }
        ++ram.addr;
    }

    uint32_t read32(uint32_t off)
    {
        EXPECT_LT(off, kSize) << "out-of-bounds AHUB MMIO read offset 0x" << std::hex << off;
        EXPECT_EQ(off & 3u, 0u) << "unaligned AHUB MMIO read offset 0x" << std::hex << off;
        if (off == kSfc1Base + kSfcStatus) {
            return freezeStatus ? 1u : regs[kSfc1Base + kSfcEnable];
        }
        if (off == kSfc2Base + kSfcStatus) {
            return freezeStatus ? 1u : regs[kSfc2Base + kSfcEnable];
        }
        if (off == kMixer1Base + kMixerTx1Status) {
            return freezeStatus ? 1u : regs[kMixer1Base + kMixerTx1Enable];
        }
        if (off == kMvc1Base + kMvcStatus) {
            return freezeStatus ? 1u : regs[kMvc1Base + kMvcEnable];
        }
        if (off == kOpe1Base + kOpeStatus) {
            return freezeStatus ? 1u : regs[kOpe1Base + kOpeEnable];
        }
        if (isSoftResetReg(off)) {
            const uint32_t v = regs[off];
            if (!freezeSoftReset && (v & 1u)) {
                regs[off] = 0u;
            }
            return v;
        }
        if (off == kMvc1Base + kMvcSwitch) {
            const uint32_t v = regs[off];
            if (!freezeMvcSwitch && v) {
                regs[off] = 0u;
            }
            return v;
        }
        return regs[off];
    }

    void write32(uint32_t off, uint32_t value)
    {
        EXPECT_LT(off, kSize) << "out-of-bounds AHUB MMIO write offset 0x" << std::hex << off;
        EXPECT_EQ(off & 3u, 0u) << "unaligned AHUB MMIO write offset 0x" << std::hex << off;
        writes.emplace_back(off, value);
        regs[off] = value;

        if (off == kSfc1Base + kSfcCfgRamCtrl) {
            startRam(sfc1Ram, value, kSfcCoefDepth);
        } else if (off == kSfc1Base + kSfcCfgRamData) {
            writeRam(sfc1Ram, value);
        } else if (off == kSfc2Base + kSfcCfgRamCtrl) {
            startRam(sfc2Ram, value, kSfcCoefDepth);
        } else if (off == kSfc2Base + kSfcCfgRamData) {
            writeRam(sfc2Ram, value);
        } else if (off == kMixer1Base + kMixerGainRamCtrl) {
            startRam(mixerGainRam, value, 16u);
        } else if (off == kMixer1Base + kMixerGainRamData) {
            writeRam(mixerGainRam, value);
        } else if (off == kMvc1Base + kMvcCfgRamCtrl) {
            startRam(mvcRam, value, 9u);
        } else if (off == kMvc1Base + kMvcCfgRamData) {
            writeRam(mvcRam, value);
        } else if (off == kPeq1Base + kPeqRamCtrl) {
            startRam(peqGainRam, value, 8u * kPeqGainWordsPerCh);
        } else if (off == kPeq1Base + kPeqRamData) {
            writeRam(peqGainRam, value);
        } else if (off == kPeq1Base + kPeqShiftCtrl) {
            startRam(peqShiftRam, value, 8u * kPeqShiftWordsPerCh);
        } else if (off == kPeq1Base + kPeqShiftData) {
            writeRam(peqShiftRam, value);
        } else {
            for (uint32_t b = 0; b < kMbdrcBands; ++b) {
                if (off == kMbdrc1Base + kMbdrcRamCtrl + 4u * b) {
                    startRam(mbdrcRam[b], value, kMbdrcWordsPerBand);
                } else if (off == kMbdrc1Base + kMbdrcRamData + 4u * b) {
                    writeRam(mbdrcRam[b], value);
                }
            }
        }
    }

    size_t countWrites(uint32_t off) const
    {
        size_t n = 0;
        for (const auto &[reg, val] : writes) {
            (void)val;
            if (reg == off) {
                ++n;
            }
        }
        return n;
    }
};

AhubModel gModel;

class AhubTest : public testing::Test {
protected:
    void SetUp() override
    {
        gModel.reset();
    }
};

}  // namespace

extern "C" {

uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gModel.read32(static_cast<uint32_t>(address - AhubModel::kBase));
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gModel.write32(static_cast<uint32_t>(address - AhubModel::kBase), value);
}

uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write8(uintptr_t, uint8_t)
{}

}  // extern "C"

namespace {

TEST_F(AhubTest, XbarPlaybackAndCaptureRoutingAreOneHot)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    auto expectConn = [](uint32_t dst, XbarSource expected) {
        for (uint32_t part = 0; part < 3u; ++part) {
            const uint32_t off = kXbarBase + dst + part * kXbarPartStride;
            EXPECT_EQ(gModel.regs[off], part == expected.part ? expected.mask : 0u)
                << "dst=" << dst << " part=" << part;
        }
    };

    xbarRoutePlayback(ahub, false);
    expectConn(kXbarDstMixer1Rx1, kSrcAdmaif1);
    expectConn(kXbarDstSfc1, kSrcNone);
    expectConn(kXbarDstMvc1, kSrcMixer1Tx1);
    expectConn(kXbarDstOpe1, kSrcMvc1);
    expectConn(kXbarDstI2s1, kSrcOpe1);

    xbarRoutePlayback(ahub, true);
    expectConn(kXbarDstMixer1Rx1, kSrcAdmaif1);
    expectConn(kXbarDstSfc1, kSrcMixer1Tx1);
    expectConn(kXbarDstMvc1, kSrcSfc1);
    expectConn(kXbarDstOpe1, kSrcMvc1);
    expectConn(kXbarDstI2s1, kSrcOpe1);

    xbarRouteCapture(ahub, true);
    expectConn(kXbarDstSfc2, kSrcI2s1);
    expectConn(kXbarDstAdmaif1, kSrcSfc2);

    xbarRouteCapture(ahub, false);
    expectConn(kXbarDstSfc2, kSrcNone);
    expectConn(kXbarDstAdmaif1, kSrcI2s1);
}

TEST_F(AhubTest, SfcSupportsAllAdvertisedRatesAndProgramsBypassVsRam)
{
    volatile uint8_t *ahub = AhubModel::basePtr();

    constexpr uint32_t kRates48[] = {48000u, 96000u, 32000u, 24000u, 16000u, 8000u};
    for (uint32_t rate : kRates48) {
        EXPECT_TRUE(sfcSupportsPair(rate, 48000u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(48000u, rate)) << rate;
    }
    constexpr uint32_t kRates44[] = {44100u, 88200u, 22050u, 11025u};
    for (uint32_t rate : kRates44) {
        EXPECT_TRUE(sfcSupportsPair(rate, 44100u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(44100u, rate)) << rate;
        EXPECT_TRUE(sfcSupportsPair(rate, 48000u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(48000u, rate)) << rate;
    }
    EXPECT_FALSE(sfcSupportsPair(64000u, 48000u));
    EXPECT_FALSE(sfcSupportsPair(48000u, 64000u));
    EXPECT_FALSE(sfcSupportsPair(0u, 48000u));
    EXPECT_EQ(sfcCoefficients(48000u, 48000u), nullptr);
    EXPECT_NE(sfcCoefficients(96000u, 48000u), nullptr);

    ASSERT_TRUE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 0u);
    EXPECT_EQ(gModel.countWrites(kSfc1Base + kSfcCfgRamData), 0u);

    gModel.reset();
    ASSERT_TRUE(sfcConfigure(ahub, kSfc1Base, 96000u, 48000u, 2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxFreq], 10u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 1u);
    EXPECT_EQ(gModel.countWrites(kSfc1Base + kSfcCfgRamData), kSfcCoefDepth);
    ASSERT_EQ(gModel.sfc1Ram.words.size(), kSfcCoefDepth);
    const uint32_t *const expected = sfcCoefficients(96000u, 48000u);
    ASSERT_NE(expected, nullptr);
    for (uint32_t i = 0; i < kSfcCoefDepth; ++i) {
        EXPECT_EQ(gModel.sfc1Ram.words[i], expected[i]) << "word " << i;
    }

    sfcEnable(ahub, kSfc1Base);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcEnable], 1u);
    ASSERT_TRUE(sfcStop(ahub, kSfc1Base));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcEnable], 0u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 0u);
}

TEST_F(AhubTest, MixerUnityGainRamAndGlobalEnable)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    ASSERT_TRUE(mixerConfigure(ahub, 2u));
    EXPECT_EQ(gModel.countWrites(kMixer1Base + kMixerRxSoftReset), 0u);
    EXPECT_EQ(gModel.countWrites(kMixer1Base + kMixerTx1SoftReset), 0u);
    size_t globalEnableIdx = gModel.writes.size();
    size_t firstGainRamIdx = gModel.writes.size();
    for (size_t i = 0; i < gModel.writes.size(); ++i) {
        if (gModel.writes[i].first == kMixer1Base + kMixerGlobalEnable && gModel.writes[i].second == 1u &&
            globalEnableIdx == gModel.writes.size()) {
            globalEnableIdx = i;
        }
        if ((gModel.writes[i].first == kMixer1Base + kMixerGainRamCtrl ||
             gModel.writes[i].first == kMixer1Base + kMixerGainRamData) &&
            firstGainRamIdx == gModel.writes.size()) {
            firstGainRamIdx = i;
        }
    }
    EXPECT_LT(globalEnableIdx, firstGainRamIdx);
    EXPECT_EQ(gModel.countWrites(kMixer1Base + kMixerGainRamData), 15u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Cif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1AdderCfg], 1u);
    EXPECT_NE(gModel.regs[kMixer1Base + kMixerRxCtrl] & kMixerRxSampleCountEn, 0u);
    ASSERT_EQ(gModel.mixerGainRam.words.size(), 16u);
    EXPECT_EQ(gModel.mixerGainRam.words[7], 0x01000000u);
    EXPECT_EQ(gModel.mixerGainRam.words[9], kMixerGainUnityQ16);
    EXPECT_EQ(gModel.mixerGainRam.words[15], 1u);

    mixerEnable(ahub);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Enable], 1u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerGlobalEnable], 1u);

    gModel.regs[kMixer1Base + kMixerRxSampleCount] = 0x12345678u;
    EXPECT_EQ(mixerSampleCount(ahub), 0x12345678u);

    ASSERT_TRUE(mixerStop(ahub));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Enable], 0u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerGlobalEnable], 0u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerRxCtrl] & kMixerRxSampleCountEn, 0u);
}

TEST_F(AhubTest, MvcLinearRampVolumeMuteAndPeak)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    constexpr uint32_t kCtrlUnmuted = kMvcCtrlPerChanEn | kMvcCtrlCurveLinear | kMvcCtrlRoundingUnbias;
    ASSERT_TRUE(mvcConfigure(ahub, 2u, -6000, false));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcDuration], 720u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcDurationInv], (1u << 31) / 720u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPolyN1], 16u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPolyN2], 63u);
    EXPECT_EQ(gModel.countWrites(kMvc1Base + kMvcCfgRamData), 9u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kCtrlUnmuted);
    const uint32_t minus6dB = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(-6000));
    for (uint32_t ch = 0; ch < 8u; ++ch) {
        EXPECT_EQ(gModel.regs[kMvc1Base + kMvcInitVol + 4u * ch], minus6dB) << ch;
        EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol + 4u * ch], minus6dB) << ch;
    }
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPeakCtrl], 0x12c0u);

    mvcEnable(ahub);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcEnable], 1u);

    const size_t initWritesBefore = gModel.countWrites(kMvc1Base + kMvcInitVol);
    ASSERT_TRUE(mvcSetVolume(ahub, -12000, true, false));
    EXPECT_EQ(gModel.countWrites(kMvc1Base + kMvcInitVol), initWritesBefore);
    const uint32_t minus12dB = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(-12000));
    for (uint32_t ch = 0; ch < 8u; ++ch) {
        EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol + 4u * ch], minus12dB) << ch;
    }
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kCtrlUnmuted | kMvcCtrlMuteMask);

    ASSERT_TRUE(mvcSetVolume(ahub, 0, false, true));
    EXPECT_GT(gModel.countWrites(kMvc1Base + kMvcInitVol), initWritesBefore);
    for (uint32_t ch = 0; ch < 8u; ++ch) {
        EXPECT_EQ(gModel.regs[kMvc1Base + kMvcInitVol + 4u * ch], 0u) << ch;
        EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol + 4u * ch], 0u) << ch;
    }
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kCtrlUnmuted);

    for (const auto &[reg, val] : gModel.writes) {
        if (reg == kMvc1Base + kMvcSwitch) {
            EXPECT_EQ(val, kMvcSwitchVolume);
        }
    }

    gModel.regs[kMvc1Base + kMvcPeakValue] = 0x00100000u;
    gModel.regs[kMvc1Base + kMvcPeakValue + 4u] = 0x00250000u;
    gModel.regs[kMvc1Base + kMvcConfigErr] = 0x5u;
    uint32_t peakL = 0u;
    uint32_t peakR = 0u;
    mvcReadPeakChannels(ahub, &peakL, &peakR);
    EXPECT_EQ(peakL, 0x00100000u);
    EXPECT_EQ(peakR, 0x00250000u);
    EXPECT_EQ(mvcReadPeak(ahub), 0x00250000u);
    EXPECT_EQ(mvcReadConfigErr(ahub), 0x5u);

    ASSERT_TRUE(mvcStop(ahub));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcEnable], 0u);
}

TEST_F(AhubTest, OpeSpeakerDspAndHeadphoneBypass)
{
    volatile uint8_t *ahub = AhubModel::basePtr();

    ASSERT_TRUE(opeConfigure(ahub, 48000u, false, 2u));
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeDir], kOpeDirPeqToMbdrc);
    EXPECT_EQ(gModel.regs[kPeq1Base + kPeqCfg], kPeqCfgUnbias);
    EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcCfg], 0u);
    EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqRamData), 0u);
    EXPECT_EQ(gModel.countWrites(kMbdrc1Base + kMbdrcRamData), 0u);
    EXPECT_EQ(gModel.countWrites(kPeq1Base + 0x04u), 0u);

    for (uint32_t linkRate : {48000u, 44100u}) {
        gModel.reset();
        ASSERT_TRUE(opeConfigure(ahub, linkRate, true, 2u)) << linkRate;
        EXPECT_EQ(gModel.countWrites(kPeq1Base + 0x04u), 0u);
        EXPECT_EQ(gModel.regs[kPeq1Base + kPeqCfg], (1u << kPeqCfgStagesShift) | kPeqCfgUnbias | kPeqCfgModeActive);
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcCfg], 0u);
        EXPECT_EQ(gModel.countWrites(kMbdrc1Base + kMbdrcRamData), 0u);
        EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqRamData), 8u * kPeqGainWordsPerCh);
        EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqShiftData), 8u * kPeqShiftWordsPerCh);
        ASSERT_EQ(gModel.peqGainRam.words.size(), 8u * kPeqGainWordsPerCh);
        ASSERT_EQ(gModel.peqShiftRam.words.size(), 8u * kPeqShiftWordsPerCh);

        for (uint32_t ch = 0; ch < 8u; ++ch) {
            const uint32_t sBase = ch * kPeqShiftWordsPerCh;
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 0], 30u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 1], 30u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 2], 30u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 13], 28u);

            const uint32_t gBase = ch * kPeqGainWordsPerCh;
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 0]), 862978048);
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 61]), 1073741824);
            const int32_t expectedS0B0 = linkRate == 44100u ? 534711769 : 534886875;
            const int32_t expectedS1B0 = linkRate == 44100u ? 601123097 : 601224974;
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 1]), expectedS0B0);
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 6]), expectedS1B0);
        }
    }

    opeEnable(ahub);
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeEnable], 1u);
    ASSERT_TRUE(opeStop(ahub));
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeEnable], 0u);
}

TEST_F(AhubTest, NullAndTimeoutGuards)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    EXPECT_FALSE(sfcConfigure(nullptr, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_FALSE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 0u));
    EXPECT_FALSE(mixerConfigure(nullptr, 2u));
    EXPECT_FALSE(mvcConfigure(nullptr, 2u, 0, false));
    EXPECT_FALSE(opeConfigure(nullptr, 48000u, true, 2u));

    gModel.freezeSoftReset = true;
    EXPECT_FALSE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_FALSE(mvcConfigure(ahub, 2u, 0, false));
    EXPECT_FALSE(opeConfigure(ahub, 48000u, true, 2u));

    gModel.reset();
    gModel.freezeStatus = true;
    EXPECT_FALSE(mixerConfigure(ahub, 2u));

    gModel.reset();
    gModel.freezeMvcSwitch = true;
    gModel.regs[kMvc1Base + kMvcSwitch] = kMvcSwitchVolume;
    EXPECT_FALSE(mvcSetVolume(ahub, -3000, false, false));
}

}  // namespace
