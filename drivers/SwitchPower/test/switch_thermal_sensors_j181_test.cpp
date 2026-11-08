// thermalmonitord's J181AP sensors on the Switch (drivers/SwitchPower/SwitchSensorModels.h), what iOS 27 reads.
//
// Checks, against what iOS 27.2 (24B119) itself has (recorded below from /System/Library/ThermalMonitor/
// J181AP-Info.plist and -[HidSensors loadProductTemperatureParameters:] / getHIDSensorDict in its thermalmonitord):
//   * one service per distinct matchName of the non-synth entries, in plist order: thermalmonitord keys its readings by
//     matchName and expects as many services as it has keys less its shadowSensor ones;
//   * the infoOnly entries, and the watchdog mask: requiresWatchDog defaults to true, one bit per non-synth entry;
//   * every mapping is a real measurement of the nearest point.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "switch_thermal_sensors_harness.h"

namespace {

constexpr unsigned kInfo = 1, kShadow = 2;

struct PlistSensor {
    const char *fourcc, *matchName;
    unsigned flags;
};

// J181AP-Info.plist "Sensors", 24B119: every entry without synthSensor (TK0F TK0C TK1C TK2C TSFD TSBH are synth, with
// requiresWatchDog false).
const PlistSensor kJ181apHidSensors[] = {
    {"TG0B", "TG0B", 0},
    {"TG0V", "TG0V", kInfo},
    {"TP1A", "TP1d", kInfo},
    {"TP2C", "TP2d", 0},
    {"TP3R", "TP3d", 0},
    {"TP4H", "TP4d", kInfo},
    {"TP5d", "TP5d", 0},
    {"TP0Z", "PMU tcal", 0},
    {"Th0a", "Avg: PMGR SOC Die Temp Sensor0", 0},
    {"Th0f", "Avg: PMGR SOC Die Temp Sensor0", kShadow},
    {"Th0x", "Max: PMGR SOC Die Temp Sensor0", 0},
    {"Th1a", "Avg: PMGR SOC Die Temp Sensor1", 0},
    {"Th1f", "Avg: PMGR SOC Die Temp Sensor1", kShadow},
    {"Th1x", "Max: PMGR SOC Die Temp Sensor1", 0},
    {"Th2a", "Avg: PMGR SOC Die Temp Sensor2", 0},
    {"Th2f", "Avg: PMGR SOC Die Temp Sensor2", kShadow},
    {"Th2x", "Max: PMGR SOC Die Temp Sensor2", 0},
    {"Tc0a", "Avg: pACC Temp Sensor0", 0},
    {"Tc0f", "Avg: pACC Temp Sensor0", kShadow},
    {"Tc0x", "Max: pACC Temp Sensor0", 0},
    {"Tc1a", "Avg: pACC Temp Sensor1", 0},
    {"Tc1f", "Avg: pACC Temp Sensor1", kShadow},
    {"Tc1x", "Max: pACC Temp Sensor1", 0},
    {"Tc2a", "Avg: eACC Temp Sensor0", 0},
    {"Tc2f", "Avg: eACC Temp Sensor0", kShadow},
    {"Tc2x", "Max: eACC Temp Sensor0", 0},
    {"Ts0a", "Avg: SOC MTR Temp Sensor0", 0},
    {"Ts0f", "Avg: SOC MTR Temp Sensor0", kShadow},
    {"Ts0x", "Max: SOC MTR Temp Sensor0", 0},
    {"Ts3a", "Avg: SOC MTR Temp Sensor3", 0},
    {"Ts3f", "Avg: SOC MTR Temp Sensor3", kShadow},
    {"Ts3x", "Max: SOC MTR Temp Sensor3", 0},
    {"Ta0a", "Avg: ANE MTR Temp Sensor0", 0},
    {"Ta0f", "Avg: ANE MTR Temp Sensor0", kShadow},
    {"Ta0x", "Max: ANE MTR Temp Sensor0", 0},
    {"Tp1a", "Avg: pACC MTR Temp Sensor1", 0},
    {"Tp1f", "Avg: pACC MTR Temp Sensor1", kShadow},
    {"Tp1x", "Max: pACC MTR Temp Sensor1", 0},
    {"Tp4a", "Avg: pACC MTR Temp Sensor4", 0},
    {"Tp4f", "Avg: pACC MTR Temp Sensor4", kShadow},
    {"Tp4x", "Max: pACC MTR Temp Sensor4", 0},
    {"Tg2x", "Max: GPU MTR Temp Sensor2", 0},
    {"Tg2a", "Avg: GPU MTR Temp Sensor2", 0},
    {"Tg2f", "Avg: GPU MTR Temp Sensor2", kShadow},
    {"Tg3a", "Avg: GPU MTR Temp Sensor3", 0},
    {"Tg3f", "Avg: GPU MTR Temp Sensor3", kShadow},
    {"TN0n", "NAND CH0 temp", kInfo},
};

// SwitchSensors.h switch_sensor_source
constexpr int kSocGpu = 4, kSocPllx = 5, kTmp451Soc = 6, kTmp451Pcb = 7, kBattery = 8, kTcal = 9;
const std::set<int> kCores = {0, 1, 2, 3};
const std::set<int> kDiePoints = {kSocGpu, kSocPllx, kTmp451Soc};

struct Service {
    std::string fourcc, name;
    int source, infoOnly;
};

std::vector<Service> services()
{
    std::vector<Service> out;
    for (unsigned i = 0; i < j181_sensors_count(); ++i)
        out.push_back({j181_sensor_fourcc(i), j181_sensor_name(i), j181_sensor_source(i), j181_sensor_info_only(i)});
    return out;
}

TEST(SwitchThermalSensorsJ181, OneServicePerMatchName)
{
    std::vector<std::pair<std::string, std::string>> expected;
    std::set<std::string> seen;
    unsigned shadows = 0;
    for (const PlistSensor &s : kJ181apHidSensors) {
        if (s.flags & kShadow) {
            EXPECT_TRUE(seen.count(s.matchName)) << s.fourcc << ": a shadow follows the entry it shadows";
            ++shadows;
            continue;
        }
        if (seen.insert(s.matchName).second)
            expected.emplace_back(s.fourcc, s.matchName);
    }
    std::vector<std::pair<std::string, std::string>> table;
    for (const Service &s : services())
        table.emplace_back(s.fourcc, s.name);
    EXPECT_EQ(table, expected);
    EXPECT_EQ(table.size(), std::size(kJ181apHidSensors) - shadows);  // getHIDSensorDict's expected service count
    EXPECT_EQ(table.size(), 34u);
}

TEST(SwitchThermalSensorsJ181, InfoOnlyAndWatchdogMask)
{
    std::map<std::string, bool> infoOnly;
    for (const PlistSensor &s : kJ181apHidSensors)
        if (!(s.flags & kShadow))
            infoOnly[s.matchName] = (s.flags & kInfo) != 0;
    uint64_t reads = 0;
    const std::vector<Service> table = services();
    for (unsigned i = 0; i < table.size(); ++i) {
        EXPECT_EQ(table[i].infoOnly, infoOnly.at(table[i].name) ? 1 : 0) << table[i].fourcc;
        if (!infoOnly.at(table[i].name))
            reads |= 1ull << i;
    }
    EXPECT_EQ(j181_sensors_read_mask(), reads);
    EXPECT_EQ(__builtin_popcountll(reads), 30);
    EXPECT_EQ(std::size(kJ181apHidSensors), 47u);  // watchdog mask 0x7fffffffffff
}

TEST(SwitchThermalSensorsJ181, EveryValueIsTheNearestMeasurement)
{
    const std::regex board("TP\\dd|NAND CH0 temp");
    std::set<int> sources;
    std::map<std::string, std::set<int>> pairs;  // Avg and Max of one sensor read one point
    for (const Service &s : services()) {
        SCOPED_TRACE(s.fourcc);
        sources.insert(s.source);
        pairs[s.name.substr(s.name.find(": ") + 1)].insert(s.source);
        if (s.name.find("ACC") != std::string::npos) {
            EXPECT_TRUE(kCores.count(s.source));  // the CPU complex: a core's tsensor
        } else if (s.name.find("PMGR SOC Die") != std::string::npos || s.name.find("MTR") != std::string::npos) {
            EXPECT_TRUE(kDiePoints.count(s.source) || kCores.count(s.source));  // on the die
            if (s.name.find("GPU") != std::string::npos)
                EXPECT_EQ(s.source, kSocGpu);
        } else if (s.name == "TG0B" || s.name == "TG0V") {
            EXPECT_EQ(s.source, kBattery);  // the gas gauge: the pack's thermistor
        } else if (s.name == "PMU tcal") {
            EXPECT_EQ(s.source, kTcal);
        } else if (std::regex_match(s.name, board)) {
            EXPECT_EQ(s.source, kTmp451Pcb);  // board thermistors: the PCB
        } else {
            ADD_FAILURE() << "unmapped " << s.name;
        }
    }
    for (const auto &[sensor, points] : pairs)
        EXPECT_EQ(points.size(), 1u) << sensor;
    for (int s = 0; s < 10; ++s)
        EXPECT_TRUE(sources.count(s)) << "source " << s;
}

}  // namespace
