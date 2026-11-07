// SPDX-License-Identifier: BSD-2-Clause-Patent
// The OLED's Realtek RT5639 (rt5640 family) on I2C1, for SwitchAudio: register access, the probe and the static link
// setup, and rt5640.c's bias, depop and DAPM power steps. Thread context (I2C transfers and the power-up waits sleep);
// SwitchAudio serializes every call on its codec work loop. A transfer that fails twice panics with one line.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rt5639 {
// Registers (rt5640.h). Values at kPrivate + n are private registers (PRIV_INDEX n, then PRIV_DATA).
enum : uint8_t {
    RESET = 0x00,
    SPK_VOL = 0x01,
    HP_VOL = 0x02,
    IN1_IN2 = 0x0d,
    DAC1_DIG_VOL = 0x19,
    STO_ADC_MIXER = 0x27,
    STO_DAC_MIXER = 0x2a,
    REC_L2_MIXER = 0x3c,
    REC_R2_MIXER = 0x3e,
    HPO_MIXER = 0x45,
    SPK_L_MIXER = 0x46,
    SPK_R_MIXER = 0x47,
    SPO_L_MIXER = 0x48,
    SPO_R_MIXER = 0x49,
    SPO_CLSD_RATIO = 0x4a,
    OUT_L3_MIXER = 0x4f,
    OUT_R3_MIXER = 0x52,
    PWR_DIG1 = 0x61,
    PWR_DIG2 = 0x62,
    PWR_ANLG1 = 0x63,
    PWR_ANLG2 = 0x64,
    PWR_MIXER = 0x65,
    PWR_VOL = 0x66,
    PRIV_INDEX = 0x6a,
    PRIV_DATA = 0x6c,
    I2S1_SDP = 0x70,
    ADDA_CLK1 = 0x73,
    GLB_CLK = 0x80,
    CLS_D_OVCD = 0x8c,
    CLS_D_OUT = 0x8d,
    DEPOP_M1 = 0x8e,
    DEPOP_M2 = 0x8f,
    DEPOP_M3 = 0x90,
    CHARGE_PUMP = 0x91,
    MICBIAS = 0x93,
    JD_CTRL = 0xbb,
    IRQ_CTRL1 = 0xbd,
    IRQ_CTRL2 = 0xbe,
    INT_IRQ_ST = 0xbf,
    GPIO_CTRL1 = 0xc0,
    GPIO_CTRL3 = 0xc2,
    GEN_CTRL1 = 0xfa,
    GEN_CTRL2 = 0xfb,
    VENDOR_ID2 = 0xff,
};

constexpr uint16_t kPrivate = 0x100;

enum Route : uint8_t {
    kRouteNone,
    kRouteSpeaker,
    kRouteHeadphones
};

struct RegValue {
    uint16_t reg, value;
};
}  // namespace rt5639

class SwitchRT5639 {
public:
    // rt5640_i2c_probe, rt5640_probe, rt5640_enable_jack_detect, then the static link setup the machine driver and
    // hw_params give it and the mixer switches of nvaudio_conf.xml's speaker and headphone paths; outputs muted, every
    // power register 0. After SC7 (afterSleep) LDO1_EN stayed high, so a codec that answers needs no power-up wait.
    void probe(bool afterSleep, bool capture);

    uint16_t read(uint8_t reg);

    // rt5640_set_bias_level STANDBY (from OFF) and OFF.
    void biasStandby();
    void biasOff();

    bool biasIsOn() const
    {
        return biasOn;
    }

    // The datasheet's power-off order, as rt5640_suspend does it: every codec function off (route, then bias), the
    // reset, then LDO1_EN low. probe() powers it again.
    void powerOff();

    // A stream's paths: I2S1 and DAC1; the headset microphone's BST1, REC mixers and ADC.
    void powerDac();
    void powerCapture();
    void powerCaptureOff();

    // The DAPM power-up and power-down of each route, in DAPM order; the output mute is separate (mute) so a route can
    // be up while no stream plays.
    void routeUp(rt5639::Route to);
    void routeDown();
    void mute(bool muted);

    // The headphone level: the DAC's own volume (0.375 dB steps below its full level), applied now when the
    // headphones play and at every headphone routeUp; steps past the DAC's range keep the headphone output muted.
    void setHeadphoneAttenuation(unsigned steps);
    void micBias(bool on);

    rt5639::Route route() const
    {
        return currentRoute;
    }

    // rt5640_detect_headset, once the jack has settled: MICBIAS1 on, and an over-current means a plain TRS plug (the
    // microphone contact grounded). Keeps the bias only for a headset; returns whether it is one.
    bool detectHeadset(bool jackIn);

    static bool jackPlugged(uint16_t irqStatus)
    {
        return !(irqStatus & (1u << 4));  // JD_STATUS, inverted (jd_inverted)
    }

private:
    rt5639::Route currentRoute = rt5639::kRouteNone;
    bool biasOn = false;
    unsigned headphoneSteps = 0;
    bool headphoneMuted = false;
    uint16_t headphoneDac() const;

    void write(uint16_t reg, uint16_t value);
    void update(uint8_t reg, uint16_t mask, uint16_t value);

    template <size_t N> void writeList(const rt5639::RegValue (&list)[N])
    {
        for (const rt5639::RegValue &entry : list)
            write(entry.reg, entry.value);
    }

    void depop(bool up);
};
