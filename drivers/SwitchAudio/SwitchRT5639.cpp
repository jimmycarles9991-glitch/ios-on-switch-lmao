// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio's RT5639 (SwitchRT5639.h). Sources: CTCaer switch-l4t-kernel-4.9 sound/soc/codecs/rt5640.c and rt5640.h,
// switchroot android_device_nintendo_nx media/nvaudio_conf.xml; the board facts are in SwitchAudio.cpp's header.
#include "SwitchRT5639.h"
#include <IOKit/IOLib.h>
#include "SwitchAudioFormat.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"

using namespace rt5639;

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kAddr = 0x1c;
constexpr unsigned kLdo1Enable = TEGRA_GPIO(Z, 4);
bool gLost;

void markLost(const char *what, uint16_t reg, int error)
{
    IOLog("SwitchAudio: codec off: RT5639 %s %#x failed: %d\n", what, reg, error);
    gLost = true;
}

enum : unsigned {
    // PWR_DIG1, PWR_DIG2
    P_I2S1 = 1u << 15,
    P_DAC_L1 = 1u << 12,
    P_DAC_R1 = 1u << 11,
    P_ADC_L = 1u << 2,
    P_ADC_R = 1u << 1,
    P_CLS_D = 1u,
    P_ADC_SF = 1u << 15,
    // PWR_ANLG1, PWR_ANLG2
    P_VREF1 = 1u << 15,
    P_FV1 = 1u << 14,
    P_MB = 1u << 13,
    P_BG = 1u << 11,
    P_HP_L = 1u << 7,
    P_HP_R = 1u << 6,
    P_HA = 1u << 5,
    P_VREF2 = 1u << 4,
    P_FV2 = 1u << 3,
    P_LDO2 = 1u << 2,
    P_BST1 = 1u << 15,
    P_MB1 = 1u << 11,
    // PWR_MIXER, PWR_VOL
    P_OM_L = 1u << 15,
    P_OM_R = 1u << 14,
    P_SM_L = 1u << 13,
    P_SM_R = 1u << 12,
    P_RM_L = 1u << 11,
    P_RM_R = 1u << 10,
    P_SV_L = 1u << 15,
    P_SV_R = 1u << 14,
    P_HV_L = 1u << 11,
    P_HV_R = 1u << 10,
    // DEPOP_M1, DEPOP_M2, CHARGE_PUMP
    D_SMT_TRIG = 1u << 15,
    D_HP_L_SMT = 1u << 9,
    D_HP_R_SMT = 1u << 8,
    D_HP_CD_PD = 1u << 7,
    D_RSTN = 1u << 6,
    D_RSTP = 1u << 5,
    D_HP_CO = 1u << 4,
    D_HP_CP = 1u << 3,
    D_HP_SG = 1u << 2,
    D_HP_CB = 1u,
    D_DEPOP_MAN = 1u << 13,
    CP_OSW_L = 1u << 11,
    CP_OSW_R = 1u << 10,
    CP_PM_HP_HV = 2u << 8,
    // SPK_VOL / HP_VOL: left mute 15, left channel off 14, left volume 13:8 (0 = +12 dB, 1.5 dB steps), right 7/6/5:0
    V_MUTE = (1u << 15) | (1u << 7),
};

// nvaudio_conf.xml: "x Speaker Playback Volume" 30 (register 39 - 30 = 9, -1.5 dB), "x HP Playback Volume" 38
// (1, +10.5 dB), both channel switches on. The DAC volume tops out at rt5640.c's own control limits:
// CUSTOM_DAC_SPK_MAX_VAL_FRIG 150 (-9.4 dB, the OLED speakers, eq-config-type 2) and CUSTOM_DAC_HP_MAX_VAL 170 (-1.9 dB).
constexpr uint16_t kSpkVol = (9u << 8) | 9u, kHpVol = (1u << 8) | 1u;
constexpr uint16_t kDacSpeaker = 0x9696, kDacHeadphone = 0xaaaa;
constexpr uint16_t kMicBoost = 1u << 12;  // IN1 boost +20 dB (no switchroot setting exists; headset microphones)

// rt5640.c init_list (applied at probe).
constexpr RegValue kInitList[] = {
    {0x11d, 0x0347}, {0x13d, 0x3600}, {0x112, 0x0aa8}, {0x114, 0x8aaa},
    {0x120, 0x6110}, {0x121, 0xe0e0}, {0x123, 0x1804},
};
// eq_speakers_list_frig: the OLED speakers' EQ and limiter (rt5640_spk_event POST_PMU, eq-config-type 2).
constexpr RegValue kSpeakerEq[] = {
    {0x1a0, 0xe3f0}, {0x1a1, 0x0000}, {0x1a2, 0xc5e9}, {0x1a3, 0x1a98}, {0x1a4, 0x1d2c}, {0x1a5, 0xc882},
    {0x1a6, 0x1c10}, {0x1a7, 0x01f4}, {0x1a8, 0xe904}, {0x1a9, 0x1c10}, {0x1aa, 0x01f4}, {0x1ab, 0xe904},
    {0x1ac, 0x1c10}, {0x1ad, 0x01f4}, {0x1ae, 0x1e88}, {0x1af, 0x03ea}, {0x1b0, 0x1fe2}, {0x1b1, 0x001e},
    {0x1b2, 0x1fe2}, {0x1b3, 0x0800}, {0x1b4, 0x066e}, {0x0b1, 0x00e1}, {0x0b0, 0x6061}, {0x0b5, 0x1f00},
    {0x0b6, 0x0480}, {0x0b4, 0x2b30},
};
// eq_microphone_list_nx (rt5640_hp_event POST_PMU: with headphones the EQ block filters the ADC instead).
constexpr RegValue kMicEq[] = {
    {0x0d3, 0x2a20}, {0x1b0, 0x1f68}, {0x1b1, 0x0094}, {0x1b2, 0x1f69},
    {0x1b3, 0x0800}, {0x1b4, 0x0800}, {0x0b1, 0x0040}, {0x0b0, 0xe040},
};
// eq_normal_list_nx (rt5640_spk_event PRE_PMD).
constexpr RegValue kEqOff[] = {{0x0b1, 0x0000}, {0x0b0, 0x6000}, {0x0b4, 0x2206}};
}  // namespace

uint16_t SwitchRT5639::read(uint8_t reg)
{
    uint8_t bytes[2] = {};
    if (gLost)
        return 0;
    int error = tegra_i2c_read(kBus, kAddr, reg, bytes, sizeof(bytes));
    if (error)
        error = tegra_i2c_read(kBus, kAddr, reg, bytes, sizeof(bytes));
    if (error) {
        markLost("read", reg, error);
        return 0;
    }
    return static_cast<uint16_t>(static_cast<unsigned>(bytes[0]) << 8U | bytes[1]);  // 16-bit registers, big endian
}

// NOLINTNEXTLINE(misc-no-recursion): a private register is one PRIV_INDEX write deep
void SwitchRT5639::write(uint16_t reg, uint16_t value)
{
    if (reg >= kPrivate) {
        write(PRIV_INDEX, reg - kPrivate);
        reg = PRIV_DATA;
    }
    if (gLost)
        return;
    const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    int error = tegra_i2c_write(kBus, kAddr, static_cast<uint8_t>(reg), bytes, sizeof(bytes));
    if (error)
        error = tegra_i2c_write(kBus, kAddr, static_cast<uint8_t>(reg), bytes, sizeof(bytes));
    if (error)
        markLost("write", reg, error);
}

void SwitchRT5639::update(uint8_t reg, uint16_t mask, uint16_t value)
{
    const unsigned old = read(reg);
    const unsigned now = (old & ~static_cast<unsigned>(mask)) | (static_cast<unsigned>(value) & mask);
    if (now != old)
        write(reg, static_cast<uint16_t>(now));
}

bool SwitchRT5639::present()
{
    if (tegra_i2c_init(kBus) != TEGRA_I2C_OK)
        panic("SwitchAudio: I2C1 did not start");
    if (tegra_gpio_output(kLdo1Enable, 1))
        panic("SwitchAudio: cannot drive PZ4 (RT5639 LDO1_EN)");
    IOSleep(400);
    uint8_t id[2] = {};
    const int error = tegra_i2c_read(kBus, kAddr, VENDOR_ID2, id, sizeof(id));
    if (!error && id[0] == 0x62 && id[1] == 0x31)
        return true;
    IOLog("SwitchAudio: no RT5639 on I2C1 0x1c (VENDOR_ID2 %02x%02x, error %d): audio is the null backend\n", id[0],
          id[1], error);
    (void)tegra_gpio_output(kLdo1Enable, 0);
    gLost = true;
    return false;
}

bool SwitchRT5639::answering()
{
    return !gLost;
}

void SwitchRT5639::probe(bool ldoOn, bool capture)
{
    if (gLost)
        return;
    if (tegra_i2c_init(kBus) != TEGRA_I2C_OK)
        panic("SwitchAudio: I2C1 did not start");
    uint8_t id[2] = {};
    const bool powered =
        ldoOn && (tegra_i2c_read(kBus, kAddr, VENDOR_ID2, id, sizeof(id)) == 0) && id[0] == 0x62 && id[1] == 0x31;
    if (!powered) {
        if (tegra_gpio_output(kLdo1Enable, 1))
            panic("SwitchAudio: cannot drive PZ4 (RT5639 LDO1_EN)");
        IOSleep(400);
    }
    const int error = tegra_i2c_read(kBus, kAddr, VENDOR_ID2, id, sizeof(id));
    if (error || id[0] != 0x62 || id[1] != 0x31) {
        IOLog("SwitchAudio: codec off: no RT5639 on I2C1 0x1c (VENDOR_ID2 %02x%02x, error %d)\n", id[0], id[1], error);
        gLost = true;
        return;
    }
    currentRoute = kRouteNone;
    write(RESET, 0);
    update(GEN_CTRL1, (1u << 11) | 1u, (1u << 11) | 1u);  // MCLK detect, I2S clock
    writeList(kInitList);
    write(kPrivate + 0x77, 0x9f00);         // HP DCC
    update(CLS_D_OUT, 1u << 11, 1u << 11);  // class-D stereo
    update(CLS_D_OVCD, 1u << 8, 1u << 8);   // power down on over-current
    update(SPO_CLSD_RATIO, 7, 4);
    write(kPrivate + 0x90, 0x2000);  // HP amp detection bounds
    write(kPrivate + 0x91, 0x1000);
    biasOff();
    update(GEN_CTRL1, 0x0301, 0x0301);
    // Jack detect: source the codec's GPIO2, GPIO1 as the IRQ output, JD1 only, over-current 600 uA x 0.75 scale.
    update(JD_CTRL, 7u << 13, 4u << 13);
    update(GPIO_CTRL1, (1u << 15) | (1u << 10), (1u << 15) | (1u << 10));
    update(GPIO_CTRL3, 1u << 2, 1u << 2);
    write(GEN_CTRL1, 0x3e01);
    write(GEN_CTRL2, 0x0000);
    write(kPrivate + 0x15, 0xa800 | (2u << 8));
    update(MICBIAS, (3u << 9) | (1u << 11), 1u << 11);
    update(IRQ_CTRL2, 1u << 11, 0);  // external microphone: no sticky over-current
    write(IRQ_CTRL1, 1u << 15);      // IRQ_JD_NOR
    // Slave, normal BCLK, 24-bit, I2S; sysclk = MCLK; BCLK 64 fs, pre-divider 1 (256 fs) (SwitchAudioFormat.h).
    update(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);
    update(GLB_CLK, 3u << 14, 0);
    update(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);
    // Mixer switches (a cleared bit routes): stereo DAC mixers <- DAC1, speaker mixers <- DAC1, speaker outputs
    // <- SPKVOL, output mixers <- DAC1, HPO mixer <- HPVOL. Outputs muted and at nvaudio_conf.xml's volumes.
    update(STO_DAC_MIXER, (1u << 14) | (1u << 6), 0);
    update(SPK_L_MIXER, 1u << 3, 0);
    update(SPK_R_MIXER, 1u << 3, 0);
    update(SPO_L_MIXER, 1u << 12, 0);
    update(SPO_R_MIXER, 1u << 12, 0);
    update(OUT_L3_MIXER, 1u, 0);
    update(OUT_R3_MIXER, 1u, 0);
    update(HPO_MIXER, 1u << 13, 0);
    write(SPK_VOL, V_MUTE | kSpkVol);
    write(HP_VOL, V_MUTE | kHpVol);
    if (capture) {
        // IN1P -> BST1 -> REC mixers -> ADC -> stereo ADC mixer (ADC1) -> IF1.
        update(REC_L2_MIXER, 1u << 1, 0);
        update(REC_R2_MIXER, 1u << 1, 0);
        update(STO_ADC_MIXER, (1u << 14) | (1u << 6), 0);
        update(IN1_IN2, 0xfu << 12, kMicBoost);
    }
}

// VREF1/VREF2 charge in fast mode (FV clear), then run in normal mode.
void SwitchRT5639::biasStandby()
{
    if (biasOn)
        return;
    update(PWR_ANLG1, P_VREF1 | P_MB | P_BG | P_VREF2, P_VREF1 | P_MB | P_BG | P_VREF2);
    IOSleep(15);
    update(PWR_ANLG1, P_FV1 | P_FV2, P_FV1 | P_FV2);
    update(GEN_CTRL1, 0x0301, 0x0201);
    biasOn = true;
}

void SwitchRT5639::biasOff()
{
    update(DEPOP_M1, D_HP_CB | D_HP_CP | D_HP_CO | D_HP_CD_PD, D_HP_CD_PD);
    write(DEPOP_M2, 0x1100);
    for (uint8_t reg = PWR_DIG1; reg <= PWR_VOL; ++reg)
        write(reg, 0);
    update(GEN_CTRL1, 1u, 1u);  // the I2S clock stays: jack detect with an external microphone needs it
    biasOn = false;
}

void SwitchRT5639::powerOff()
{
    routeDown();
    biasOff();
    write(RESET, 0);
    if (tegra_gpio_output(kLdo1Enable, 0))
        IOLog("SwitchAudio: cannot drive PZ4 (RT5639 LDO1_EN) low; the codec loses power with the PMIC\n");
}

void SwitchRT5639::powerDac()
{
    update(PWR_DIG1, P_I2S1 | P_DAC_L1 | P_DAC_R1, P_I2S1 | P_DAC_L1 | P_DAC_R1);
}

void SwitchRT5639::powerCapture()
{
    update(PWR_ANLG2, P_BST1, P_BST1);
    update(PWR_MIXER, P_RM_L | P_RM_R, P_RM_L | P_RM_R);
    update(PWR_DIG1, P_ADC_L | P_ADC_R, P_ADC_L | P_ADC_R);
    update(PWR_DIG2, P_ADC_SF, P_ADC_SF);
}

void SwitchRT5639::powerCaptureOff()
{
    update(PWR_DIG2, P_ADC_SF, 0);
    update(PWR_DIG1, P_ADC_L | P_ADC_R, 0);
    update(PWR_MIXER, P_RM_L | P_RM_R, 0);
    update(PWR_ANLG2, P_BST1, 0);
}

// rt5640_pud_depop.
void SwitchRT5639::depop(bool up)
{
    if (up) {
        write(CHARGE_PUMP, CP_PM_HP_HV | CP_OSW_R | CP_OSW_L);
        update(DEPOP_M3, 0x777, (7u << 8) | (3u << 4) | 7u);  // 192 kHz, 12 kHz, 192 kHz
        write(kPrivate + 0x37, 0xfc00);
        update(DEPOP_M1, D_SMT_TRIG, D_SMT_TRIG);
        update(DEPOP_M1, D_RSTN, D_RSTN);
        update(DEPOP_M1, D_RSTN | D_HP_R_SMT | D_HP_L_SMT, D_HP_R_SMT | D_HP_L_SMT);
    } else {
        update(DEPOP_M3, 0x777, (6u << 8) | (4u << 4) | 6u);  // 96 kHz, 24 kHz, 96 kHz
        write(kPrivate + 0x37, 0xfc00);
        update(DEPOP_M1, D_HP_SG, D_HP_SG);
        update(DEPOP_M1, D_RSTP, D_RSTP);
        update(DEPOP_M1, D_RSTP | D_HP_R_SMT | D_HP_L_SMT, D_HP_R_SMT | D_HP_L_SMT);
    }
}

// Supplies, mixers, PGAs, then the HP post and bias-on steps.
void SwitchRT5639::routeUp(Route to)
{
    if (to == kRouteSpeaker) {
        update(PWR_DIG1, P_CLS_D, P_CLS_D);  // "Improve SPK Amp Drv", then rt5640_spk_event POST_PMU
        writeList(kSpeakerEq);
        update(PWR_MIXER, P_SM_L | P_SM_R, P_SM_L | P_SM_R);
        update(PWR_VOL, P_SV_L | P_SV_R, P_SV_L | P_SV_R);
        write(DAC1_DIG_VOL, kDacSpeaker);
    } else if (to == kRouteHeadphones) {
        update(PWR_ANLG1, P_HP_L | P_HP_R, P_HP_L | P_HP_R);
        // "Improve HP Amp Drv": hp_amp_power_on.
        update(DEPOP_M2, D_DEPOP_MAN, D_DEPOP_MAN);
        write(DEPOP_M1, D_HP_CP | D_HP_CB);
        update(PWR_ANLG1, P_FV1 | P_FV2, 0);
        update(PWR_ANLG1, P_HA, P_HA);
        IOSleep(15);
        update(PWR_ANLG1, P_FV1 | P_FV2, P_FV1 | P_FV2);
        update(DEPOP_M1, D_HP_SG | D_HP_CO, D_HP_SG | D_HP_CO);
        update(PWR_MIXER, P_OM_L | P_OM_R, P_OM_L | P_OM_R);
        update(PWR_VOL, P_HV_L | P_HV_R, P_HV_L | P_HV_R);
        depop(true);  // "HP Amp" POST_PMU
        writeList(kMicEq);
        write(DAC1_DIG_VOL, headphoneDac());
        IOSleep(80);   // "HP Post"
        IOSleep(100);  // bias ON with the HP amp up: then release its soft mute
        update(DEPOP_M1, D_HP_SG | D_HP_R_SMT | D_HP_L_SMT, 0);
    }
    currentRoute = to;
}

void SwitchRT5639::routeDown()
{
    mute(true);
    if (currentRoute == kRouteSpeaker) {
        update(PWR_VOL, P_SV_L | P_SV_R, 0);
        update(PWR_MIXER, P_SM_L | P_SM_R, 0);
        writeList(kEqOff);  // rt5640_spk_event PRE_PMD
        update(PWR_DIG1, P_CLS_D, 0);
    } else if (currentRoute == kRouteHeadphones) {
        depop(false);  // "HP Amp" PRE_PMD
        IOSleep(30);
        update(DEPOP_M1, D_SMT_TRIG | D_HP_R_SMT | D_HP_L_SMT, 0);
        write(CHARGE_PUMP, CP_OSW_R | CP_OSW_L);
        update(PWR_VOL, P_HV_L | P_HV_R, 0);
        update(PWR_MIXER, P_OM_L | P_OM_R, 0);
        update(PWR_ANLG1, P_HP_L | P_HP_R, 0);
    }
    currentRoute = kRouteNone;
}

void SwitchRT5639::mute(bool muted)
{
    if (currentRoute == kRouteSpeaker) {
        update(SPK_VOL, V_MUTE, muted ? V_MUTE : 0);
    } else if (currentRoute == kRouteHeadphones) {
        update(HP_VOL, V_MUTE, muted || headphoneMuted ? V_MUTE : 0);
    }
}

// DAC1_DIG_VOL: left 15:8, right 7:0, 0xaf = 0 dB in 0.375 dB steps; kDacHeadphone is the full headphone level.
uint16_t SwitchRT5639::headphoneDac() const
{
    const unsigned full = kDacHeadphone & 0xff;
    const unsigned steps = headphoneSteps < full ? headphoneSteps : full;
    return static_cast<uint16_t>((full - steps) << 8 | (full - steps));
}

void SwitchRT5639::setHeadphoneAttenuation(unsigned steps)
{
    headphoneSteps = steps;
    const bool muted = steps > (kDacHeadphone & 0xffu);
    if (currentRoute != kRouteHeadphones) {
        headphoneMuted = muted;
        return;
    }
    write(DAC1_DIG_VOL, headphoneDac());
    if (muted != headphoneMuted) {
        headphoneMuted = muted;
        if (muted)
            update(HP_VOL, V_MUTE, V_MUTE);
    }
}

void SwitchRT5639::micBias(bool on)
{
    update(PWR_ANLG1, P_LDO2, on ? P_LDO2 : 0);
    update(PWR_ANLG2, P_MB1, on ? P_MB1 : 0);
}

bool SwitchRT5639::detectHeadset(bool jackIn)
{
    bool headset = false;
    if (jackIn) {
        micBias(true);
        update(IRQ_CTRL2, 1u << 3, 0);
        IOSleep(100);
        headset = ((read(IRQ_CTRL2) & (1u << 3)) == 0u);
    }
    if (!headset)
        micBias(false);
    return headset;
}
