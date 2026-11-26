// SPDX-License-Identifier: BSD-2-Clause-Patent
// The OLED's Realtek RT5639 (rt5640 family) on I2C1, for SwitchAudio: register access, probe, static link setup,
// hybrid analog/digital gain staging, and rt5640.c bias, depop and DAPM power steps.
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
    ADC_DIG_VOL = 0x1c,
    STO_ADC_MIXER = 0x27,
    STO_DAC_MIXER = 0x2a,
    DIG_INF_DATA = 0x2f,
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
    EQ_CTRL1 = 0xb0,
    EQ_CTRL2 = 0xb1,
    DRC_AGC_1 = 0xb4,
    DRC_AGC_2 = 0xb5,
    DRC_AGC_3 = 0xb6,
    JD_CTRL = 0xbb,
    IRQ_CTRL1 = 0xbd,
    IRQ_CTRL2 = 0xbe,
    INT_IRQ_ST = 0xbf,
    GPIO_CTRL1 = 0xc0,
    GPIO_CTRL3 = 0xc2,
    ADJ_HPF = 0xd3,
    SV_ZCD1 = 0xd9,
    SV_ZCD2 = 0xda,
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
    static bool present();
    static bool answering();

    void probe(bool ldoOn, bool capture);

    static uint16_t read(uint8_t reg);

    void biasStandby();
    void biasOff();

    bool biasIsOn() const
    {
        return biasOn;
    }

    void powerOff();

    void powerDac();
    void powerCapture();
    void powerCaptureOff();

    void routeUp(rt5639::Route to);
    void routeDown();
    void mute(bool muted);

    // Hybrid headphone analog stage: regVal in [8..39] (0.0 dB down to -46.5 dB in 1.5 dB steps).
    void setHeadphoneAnalog(uint8_t regVal, bool muted);
    void micBias(bool on);

    rt5639::Route route() const
    {
        return currentRoute;
    }

    bool detectHeadset(bool jackIn);

    static bool jackPlugged(uint16_t irqStatus)
    {
        return (irqStatus & (1u << 4)) == 0u;
    }

private:
    rt5639::Route currentRoute = rt5639::kRouteNone;
    bool biasOn = false;
    bool outputMuted = true;
    uint8_t hpAnalogReg = 8;  // 0.0 dB unity analog gain (Switchroot UCM2 value 31)
    bool headphoneMuted = false;

    void write(uint16_t reg, uint16_t value);
    void update(uint8_t reg, uint16_t mask, uint16_t value);

    template <size_t N> void writeList(const rt5639::RegValue (&list)[N])
    {
        for (const rt5639::RegValue &entry : list)
            write(entry.reg, entry.value);
    }

    void depop(bool up);
};
