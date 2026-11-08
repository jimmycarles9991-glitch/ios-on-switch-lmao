// The ambient light sensor's and backlight's values in driver sources, against the references they cite (no model
// sees these: the bus table, pad and command constants, the properties CoreBrightness reads).
//
// 2. TegraI2C.cpp's I2C2 row and pad values: Atmosphere i2c_bus_device_map.inc (I2c2 0x7000C400, fast mode, interrupt
//    116), pinmux_initial_pad_config_aula.inc (Gen2I2cScl/Sda opt 0x40, mask 0x27F), pinmux_initial_drive_pad_config.inc
//    (Gen2I2c 0x0001F000, mask 0x01F1F000) and pinmux_drive_pad_characters.inc (0x09B0, 0x09B4); Linux
//    pinctrl-tegra210.c (gen2_i2c_scl_pj2 0x30c4, gen2_i2c_sda_pj3 0x30c8); L4T tegra210b01-fric-pinmux.dtsi (function
//    i2c2, no pull, not tristate, input, io-high-voltage off; pull-down strength 0x1F, pull-up 0); hekate clock.c
//    (I2C2: RST_DEV_H_SET, CLK_ENB_H_SET, CLK_SOURCE_I2C2 0x198, bit 22, CLK_M / 1 for 400 kHz).
// 3. SwitchALS.cpp's BH1730FVC command bytes against the ROHM datasheet (TSZ02201-0M3M0F616050-1-2): COMMAND = 1,
//    TRANSACTION 00 + register or 11 + special command (software reset 00100), PART_ID at 12h, DATA0 at 14h.
// 4. What CoreBrightness reads (21H461 disassembly): the light sensor's personality keys in SwitchHIDDevice.cpp
//    (-[CBALSNode initALSProperties]: Placement, Orientation, the three integration times; IOHIDFamily
//    AppleEmbeddedHIDKeys.h enums) and SwitchBacklight.cpp's backlight-calibration-parameters (DisplayCreateWithID: all
//    nine mandatory keys, 16.16 fixed point, nits in order).
// 7. SwitchBacklight publishes new-backlight-architecture, without which CoreBrightness's faded ramps never reach
//    brightness-nits.
// 8. DisplayCreateWithID's ways to fail that are this service's (21H461): IODisplayParameters (0x1b62acf74) and, under
//    the new architecture, backlight-marketing-table (0x1b62ae660; without it there is no display, so libEDR's server
//    never starts and Core Animation's render server stalls 5 s per frame on its port). SwitchBacklight.cpp publishes
//    both before backlight-control, backlight-control only with IODisplayParameters, new-backlight-architecture only
//    with the table.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
constexpr uint32_t kItimeUs = 2700 * 38;

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\n"), b = s.find_last_not_of(" \t\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// kBus's rows by bus number: "{fields},  // I2Cn ..." (a row's comment may follow on its next line).
std::map<int, std::vector<std::string>> busTable(const std::string &i2c)
{
    const std::string table = Between(i2c, "constexpr BusInfo kBus[kBuses] = {", "};");
    std::map<int, std::vector<std::string>> rows;
    const std::regex row(R"(\{([^{}]*)\},?\s*//\s*I2C(\d))");
    for (std::sregex_iterator it(table.begin(), table.end(), row), end; it != end; ++it) {
        std::vector<std::string> fields;
        const std::string body = (*it)[1];
        size_t at = 0;
        for (size_t comma; (comma = body.find(',', at)) != std::string::npos; at = comma + 1)
            fields.push_back(trim(body.substr(at, comma - at)));
        fields.push_back(trim(body.substr(at)));
        rows[std::stoi((*it)[2])] = fields;
    }
    return rows;
}
}  // namespace

TEST(SwitchALSSource, I2C2BusAndPads)
{
    const std::string i2c = SourceText("../../TegraI2C/TegraI2C.cpp");
    const auto table = busTable(i2c);
    std::vector<int> buses;
    for (const auto &[bus, fields] : table)
        buses.push_back(bus);
    ASSERT_EQ(buses, (std::vector<int>{1, 2, 3, 4, 5}));
    // BusInfo: regs, reset, enable, source, resetState, enableState, bit, divider, scl, sda, spi, sclDrive, sdaDrive,
    // pullDown (then chained, dmaRequest, dmaChannel).
    const std::vector<std::string> &row = table.at(2);
    ASSERT_GE(row.size(), 14u);
    std::vector<uint64_t> f;
    for (size_t i = 0; i < 14; ++i)
        f.push_back(std::stoull(row[i], nullptr, 0));
    const uint64_t regs = f[0], reset = f[1], enable = f[2], source = f[3], resetState = f[4], enableState = f[5];
    const uint64_t bit = f[6], divider = f[7], scl = f[8], sda = f[9], spi = f[10], sclDrive = f[11], sdaDrive = f[12];
    const uint64_t pullDown = f[13];
    EXPECT_EQ(0x7000C000 + regs, 0x7000C400u) << "Atmosphere I2c2 registers";
    EXPECT_EQ(reset, 0x308u) << "hekate clock.c _clock_i2c[1]";
    EXPECT_EQ(enable, 0x328u);
    EXPECT_EQ(source, 0x198u);
    EXPECT_EQ(bit, 22u);
    EXPECT_EQ(resetState, 0x008u) << "RST_DEVICES_H";
    EXPECT_EQ(enableState, 0x014u) << "CLK_OUT_ENB_H";
    ASSERT_NE(divider, 0u);
    EXPECT_EQ(19200000 / divider / 48, 400000u) << "SpeedMode_Fast; L4T clock-frequency 400000";
    EXPECT_EQ(spi + 32, 116u) << "Atmosphere interrupt 116; tegra210.dtsi GIC_SPI 84";
    EXPECT_EQ(0x3000 + scl, 0x30C4u) << "pinctrl-tegra210.c gen2_i2c_scl_pj2";
    EXPECT_EQ(0x3000 + sda, 0x30C8u) << "pinctrl-tegra210.c gen2_i2c_sda_pj3";
    EXPECT_EQ(sclDrive, 0x9B0u) << "pinmux_drive_pad_characters.inc Gen2I2cScl";
    EXPECT_EQ(sdaDrive, 0x9B4u) << "pinmux_drive_pad_characters.inc Gen2I2cSda";

    std::smatch pinmuxShift;
    ASSERT_TRUE(std::regex_search(i2c, pinmuxShift, std::regex(R"(kPinmuxI2C = 1u << (\d+);)")));
    const uint32_t pinmux = 1u << std::stoi(pinmuxShift[1]);
    // Atmosphere opt 0x40 (Bidirection) under mask 0x27F sets PM 0 (I2C2), PUPD 0, TRISTATE 0, E_INPUT 1, E_OD 0 (the
    // pad has it: characters mask 0x01DFF) and E_IO_HV 0 (NormalVoltage); everything else it leaves as found.
    constexpr uint32_t kHorizonFields = 0x3 | 0xC | 0x10 | 0x40 | 0x800 | 0x400;
    EXPECT_EQ(pinmux & kHorizonFields, 0x40u);
    EXPECT_EQ(pinmux & ~kHorizonFields, 0u) << "we also clear PARK, LOCK, E_LPDR, E_SCHMT, as hekate pinmux_config_i2c";

    EXPECT_THAT(i2c, HasSubstr("kDriveMask = (0x1fu << 20) | (0x1fu << 12);"));
    constexpr uint32_t kMask = (0x1fu << 20) | (0x1fu << 12);
    EXPECT_EQ(kMask, 0x01F1F000u);
    EXPECT_EQ(pullDown, 0x1Fu);
    for (uint32_t old : {0u, 0xFFFFFFFFu, 0xF0F0F0F0u, 0x00004000u, 0x12345678u}) {
        const uint32_t ours = (old & ~kMask) | (uint32_t)pullDown << 12;
        const uint32_t horizon = (old & ~0x01F1F000u) | 0x0001F000u;  // reg::ReadWrite(value, mask)
        EXPECT_EQ(ours, horizon) << std::hex << old;
    }
    EXPECT_THAT(i2c, HasSubstr("if (info.sclDrive != kBus[1].sclDrive)\n        return;"))
        << "the drive is I2C2's only";
}

TEST(SwitchALSSource, Bh1730Commands)
{
    const std::string als = SourceText("../SwitchALS.cpp");
    EXPECT_THAT(als, HasSubstr("kSensor = 0x29;")) << "slave address 0101001";
    EXPECT_THAT(als, HasSubstr("kBus = 2;")) << "I2C2";
    EXPECT_THAT(als, HasSubstr("kCommand = 0x80, kSoftwareReset = 0xe4;")) << "1 00 xxxxx; 1 11 00100";
    EXPECT_THAT(
        als,
        HasSubstr(
            "RegControl = 0x00,\n    RegTiming = 0x01,\n    RegGain = 0x07,\n    RegId = 0x12,\n    RegData0 = 0x14"));
    EXPECT_THAT(als, HasSubstr("tegra_i2c_write(kBus, kSensor, kSoftwareReset, nullptr, 0)"))
        << "special command alone";
}

// The order the first-sample timing (als_math_test FirstSampleAfterDisplayOnInTime) rests on: a wake starts the sensor
// before the panel's wake, starting sets it up at once, and a set-up from powered down skips nothing.
TEST(SwitchALSSource, WakeSamplesFromTheDcRestart)
{
    const std::string backlight = SourceText("../SwitchBacklight.cpp");
    const std::string on = Body(backlight, "void SwitchBacklight::powerOn()");
    const size_t scanout = on.find("native_legacy_display_set_scanout(true)"),
                 als = on.find("switch_als_set_active(true);"), panel = on.find("tegra_panel_power_on(level)");
    ASSERT_NE(scanout, std::string::npos);
    ASSERT_NE(als, std::string::npos);
    ASSERT_NE(panel, std::string::npos);
    EXPECT_LT(scanout, als) << "the sensor starts once the panel's DC scans out again";
    EXPECT_LT(als, panel) << "the sensor samples through the panel's wake";
    EXPECT_EQ(Count(on, "switch_als_set_active"), 1u);

    const std::string sensor = SourceText("../SwitchALS.cpp");
    const std::string update = Body(sensor, "void SwitchALS::update()");
    EXPECT_THAT(update, HasSubstr("const int err = configure();"));
    EXPECT_LT(update.find("clock->setTimeoutUS(kFirstSampleUS);"), update.find("const int err = configure();"))
        << "the first sample's period counts from the set-up";
    EXPECT_THAT(update, HasSubstr("clock->cancelTimeout();"));
    const std::string configure = Body(sensor, "int SwitchALS::configure()");
    EXPECT_THAT(configure, HasSubstr("skip = 0;"));
    EXPECT_LT(configure.find("RegTiming"), configure.find("RegControl, kPowerOn | kAdcEnable"))
        << "the conversion starts with this configuration";
}

TEST(SwitchALSSource, WhatCoreBrightnessReads)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    std::map<std::string, uint32_t> light;
    const std::regex property(R"re(\{"(Placement|Orientation|ALS\w+IntegrationTime)", (\d+)\})re");
    for (std::sregex_iterator it(hid.begin(), hid.end(), property), end; it != end; ++it)
        light[(*it)[1]] = (uint32_t)std::stoul((*it)[2]);
    EXPECT_EQ(light, (std::map<std::string, uint32_t>{
                         {"Placement", 1},    // kIOHIDPlacementTypeTop; 2 (Bottom) is the rear ALS
                         {"Orientation", 8},  // kIOHIDOrientationTypeSouthWest: portrait bottom left
                         {"ALSFastIntegrationTime", kItimeUs},
                         {"ALSSuperFastIntegrationTime", kItimeUs},
                         {"ALSSlowIntegrationTime", kItimeUs},
                     }));
    EXPECT_THAT(hid, HasSubstr("if (&p == &kProfiles[kSwitchHIDLight])\n                addLightProperties(dict);"));

    const std::string backlight = SourceText("../SwitchBacklight.cpp");
    std::map<std::string, std::string> keys;
    std::vector<std::string> names;
    const std::regex key(R"re(\{"([a-zA-Z-]+)", kNits(Max|Mid|Min)\})re");
    for (std::sregex_iterator it(backlight.begin(), backlight.end(), key), end; it != end; ++it) {
        names.push_back((*it)[1]);
        keys[(*it)[1]] = (*it)[2];
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"current-for-max-backlight", "current-for-mid-backlight",
                                               "hardware-max-current-limit", "lMaxPanel", "lMaxProduct", "lMidPanel",
                                               "lMidProduct", "lMinPanel", "lMinProduct"}));
    EXPECT_EQ(keys["lMaxPanel"], "Max");
    EXPECT_EQ(keys["lMinPanel"], "Min");
    EXPECT_EQ(keys["lMidPanel"], "Mid");
    EXPECT_THAT(backlight, HasSubstr("OSNumber::withNumber(entry.value, 32)")) << "already nits * 65536";
    EXPECT_THAT(backlight,
                HasSubstr("OSNumber *nits = OSDynamicCast(OSNumber, dict->getObject(\"brightness-nits\"));"));
    EXPECT_THAT(backlight, HasSubstr("value = nitsToBrightness(requested);"));
    EXPECT_THAT(backlight, HasSubstr("setProperty(\"new-backlight-architecture\", kOSBooleanTrue);"));
}

// CoreBrightness's ReportInterval reaches the light device's Report Interval feature: the sensor takes it as its sample
// period, held at a whole conversion, and the device no longer refuses the report.
TEST(SwitchALSSource, ReportIntervalPacesTheSamples)
{
    const std::string als = SourceText("../SwitchALS.cpp");
    EXPECT_THAT(als, HasSubstr("switch_hid_set_feature_handler(hid, &SwitchALS::reportIntervalSet, this);"));
    EXPECT_THAT(als, HasSubstr("sender->setTimeoutUS(__atomic_load_n(&self->sampleUS, __ATOMIC_RELAXED));"));
    EXPECT_THAT(als, HasSubstr("asked > kSampleUS ? asked : kSampleUS"));
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    EXPECT_THAT(hid, HasSubstr("switch_refusal_k(switch_refusal_key_with(switch_refusal_key(\"hid\", \"set-report\")"));
}

TEST(SwitchALSSource, DisplayCreateWithIDHasWhatItNeedsBeforeBacklightControl)
{
    const std::string backlight = SourceText("../SwitchBacklight.cpp");
    const std::string start = Body(backlight, "bool SwitchBacklight::start(IOService *provider)");
    const std::vector<std::string> order = {
        "publishParameters();",
        "setProperty(\"backlight-calibration-parameters\", parameters);",
        "setProperty(\"backlight-marketing-table\", table)",
        "setProperty(\"new-backlight-architecture\", kOSBooleanTrue);",
        "setProperty(\"backlight-control\", kOSBooleanTrue);",
        "registerService();",
    };
    size_t previous = 0;
    for (const std::string &step : order) {
        const size_t at = start.find(step);
        ASSERT_NE(at, std::string::npos) << step;
        EXPECT_GE(at, previous) << step << ": everything DisplayCreateWithID reads is there before backlight-control";
        previous = at;
    }
    for (const char *key : {"backlight-control", "new-backlight-architecture", "backlight-marketing-table"})
        EXPECT_EQ(Count(backlight, std::string("setProperty(\"") + key + "\""), 1u) << key << ": published once";
    // The new architecture only with its table (0x1b62ae660), backlight-control only with IODisplayParameters
    // (0x1b62acf74), the table as CFData (0x1b62ae0a8 drops others).
    EXPECT_TRUE(std::regex_search(
        start,
        std::regex(
            R"re(const bool marketing = \(table != nullptr\) && setProperty\("backlight-marketing-table", table\);\s*)re"
            R"re(OSSafeReleaseNULL\(table\);\s*if \(marketing\) \{\s*setProperty\("new-backlight-architecture")re")));
    EXPECT_TRUE(std::regex_search(
        start, std::regex(R"re(if \(OSDynamicCast\(OSDictionary, getProperty\("IODisplayParameters"\)\)\) \{\s*)re"
                          R"re(setProperty\("backlight-control", kOSBooleanTrue\);)re")));
    EXPECT_THAT(start, HasSubstr("OSData::withBytes(kMarketingTable, sizeof(kMarketingTable))"));
}
