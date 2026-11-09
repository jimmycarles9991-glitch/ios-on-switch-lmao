// thermalmonitord's PMU tcal correction against what TP0Z answers (SwitchSensorModels.h SWITCH_SENSOR_TCAL_MILLIC_*).
//
// The model is thermalmonitord's own, from both IPSWs' binaries (21H461 0x100006220, 24B119 sub_100004f0c and
// sub_100004ff8): readings in centi-degrees; the TP0Z filter is seeded with CommonProduct's
// calibrationResistorTempDefault (5182) when the product asks for no calibration samples (J120), else with the mean of
// its first requiredCalibrationResistorSamples readings (J181: 5), then follows each reading through an IIR whose gain
// is 5 / (filterParams + 5) in 16.16; every TP?d / TP?[A-Z] sensor gets default minus the filtered TP0Z added.
// J181's skin model is -[tmf02fbce43bb83f6eb51bd31374aa8279 updateAllThermalLoad:] (24B119): BCH7Skin TSBH =
// 0.88 TG0B + 0.13 TP3d - 22, CGCenterSkin TSFD = 67 + 1.06 TP5d - 0.08 TP2d; SoCLowHS acts on TSBH from 43 C.
#include <gtest/gtest.h>

#include <cstdint>

#include "switch_thermal_sensors_harness.h"

namespace {

constexpr int kDefault = 5182;  // calibrationResistorTempDefault, both IPSWs
constexpr int kPassesPerHour = 720;
constexpr int kSoCLowTarget = 4300;

struct Product {
    int filterParams;
    unsigned requiredSamples;
};

constexpr Product kJ120{6000, 0};
constexpr Product kJ181{600, 5};

class Tcal {
public:
    explicit Tcal(Product p) : product_(p)
    {}

    // One 5 s pass with TP0Z reading `raw`; returns the offset added to the board thermistors.
    int pass(int raw)
    {
        int value = raw;
        if (!seeded_) {
            if (product_.requiredSamples == 0) {
                value = kDefault;
            } else {
                ++count_;
                sum_ += static_cast<uint32_t>(raw);
            }
            filter_ = value * 65536;
            seeded_ = true;
        } else if (count_ < product_.requiredSamples) {
            ++count_;
            sum_ += static_cast<uint32_t>(raw);
            value = static_cast<int>(sum_ / count_);
            filter_ = value * 65536;
        } else {
            const int gain = (0x50000 / (product_.filterParams + 5)) & 0xffff;
            filter_ += gain * (raw - (filter_ >> 16));
            value = filter_ >> 16;
        }
        return kDefault - value;
    }

private:
    Product product_;
    bool seeded_ = false;
    unsigned count_ = 0;
    uint32_t sum_ = 0;
    int filter_ = 0;
};

int offsetAfter(Product p, int raw, int passes)
{
    Tcal tcal(p);
    int offset = 0;
    for (int i = 0; i < passes; ++i)
        offset = tcal.pass(raw);
    return offset;
}

int tsbh(int battery, int tp3d)
{
    return static_cast<int>(battery * 0.88 + -22.0 + tp3d * 0.13);
}

int tsfd(int tp5d, int tp2d)
{
    return static_cast<int>(67.0 + tp5d * 1.06 + tp2d * -0.08);
}

int answeredCenti(int milliC)
{
    return cltm_centi(fixed_of(milliC));
}

TEST(SwitchThermalTcal, TP0ZAnswersTheReference)
{
    EXPECT_EQ(answeredCenti(tcal_millic_j120()), kDefault);
    EXPECT_EQ(answeredCenti(tcal_millic_j181()), kDefault);
    EXPECT_TRUE(plausible(tcal_millic_j181()));
}

TEST(SwitchThermalTcal, NoCorrectionAtAnyHour)
{
    for (int hours : {0, 1, 8}) {
        const int passes = 1 + hours * kPassesPerHour;
        EXPECT_EQ(offsetAfter(kJ120, answeredCenti(tcal_millic_j120()), passes), 0) << hours;
        EXPECT_EQ(offsetAfter(kJ181, answeredCenti(tcal_millic_j181()), passes), 0) << hours;
    }
}

// TP0Z at 0 C, before: J181 put 51.82 C on every board thermistor from its first pass; J120 drifted there over hours.
TEST(SwitchThermalTcal, ZeroWasAHotBoard)
{
    EXPECT_EQ(offsetAfter(kJ181, 0, 6), kDefault);
    EXPECT_EQ(offsetAfter(kJ120, 0, 1), 0);
    EXPECT_GT(offsetAfter(kJ120, 0, kPassesPerHour), 2000);
}

// Boot 15 (24B119, 19:15:46): battery 35.69 C, PCB about 39.5 C. SwitchDisplay logged TSFD 83.68 C at 19:07:33 with the
// PCB near 33 C.
TEST(SwitchThermalTcal, J181SkinFromBoot15)
{
    const int battery = 3569, pcb = 3950;
    const int before = pcb + offsetAfter(kJ181, 0, 6);
    const int now = pcb + offsetAfter(kJ181, answeredCenti(tcal_millic_j181()), 6);
    EXPECT_GE(tsbh(battery, before), kSoCLowTarget);
    EXPECT_LT(tsbh(battery, now), kSoCLowTarget - 500);
    EXPECT_NEAR(tsfd(3290 + kDefault, 3290 + kDefault), 8368, 50);
    EXPECT_NEAR(tsfd(now, now), now, 100);
}

}  // namespace
