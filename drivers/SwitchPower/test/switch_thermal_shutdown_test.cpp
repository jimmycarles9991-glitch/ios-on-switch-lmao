// The thermal shutdown's order (SwitchThermalLimits.h, THERMTRIP in SwitchSocThermWindow.h), its record in the RTC
// (SwitchThermalRecord.h) and the next boot's reading of why the last one ended (SwitchResetCause.h).
#include "drivers/SwitchPower/SwitchSocThermWindow.h"
#include "drivers/SwitchPower/SwitchThermalLimits.h"
#include "drivers/SwitchPower/SwitchThermalRecord.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

constexpr int kFanFullMilliC = 90000;  // SwitchThermal.cpp kCriticalMilliC: the fan at full speed

TEST(SwitchThermalShutdown, SoftwareShutsDownBeforeTheHardwareTrip)
{
    EXPECT_LT(kFanFullMilliC, SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC);
    EXPECT_LT(SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC, SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC);
    EXPECT_LT(SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC, SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC);
    EXPECT_LT(SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC, SWITCH_THERMAL_THERMTRIP_CPU_MILLIC);
    EXPECT_LT(SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC, SWITCH_THERMAL_THERMTRIP_GPU_MILLIC);
}

TEST(SwitchThermalShutdown, AOneDegreeWindowReportsThePllxLimitBeforeTheTrip)
{
    // The window SOC_THERM arms one degree below the limit fires at the limit, short of THERMTRIP.
    const uint32_t lvl0 = switch_soctherm_lvl0_window(0, SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC - 1000);
    const uint32_t up = (lvl0 >> 18) & 0x1ffu;
    const uint32_t trip = switch_soctherm_thermtrip_ctl(0) & 0x1ffu;
    EXPECT_EQ(up * 500, static_cast<uint32_t>(SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC));
    EXPECT_LT(up, trip);
}

TEST(SwitchThermalShutdown, TheThresholdsAreL4TsForThisConsole)
{
    EXPECT_EQ(SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC, 92500);  // Tdiode_tegra gpu_shutdown (critical)
    EXPECT_EQ(SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC, 94500);   // PLL-therm cpu_heavy (hot)
    EXPECT_EQ(SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC, 96000);  // nvidia,thermtrips <3 96000>
}

TEST(SwitchThermalRecord, RoundTripsInRtcRange)
{
    for (const auto sensor : {SWITCH_THERMAL_SOC_DIODE, SWITCH_THERMAL_PLLX}) {
        for (const int millic : {92500, 94500, 95100, 119900}) {
            uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS];
            switch_thermal_record_encode(sensor, millic, fields);
            EXPECT_LE(fields[0], 59);  // SEC
            EXPECT_LE(fields[1], 59);  // MIN
            EXPECT_LE(fields[2], 23);  // HOUR
            for (const uint8_t f : fields)
                EXPECT_EQ(f & 0x80u, 0u) << "compare bits stay clear: the alarm never matches";
            int back = 0;
            EXPECT_EQ(switch_thermal_record_decode(fields, &back), sensor);
            EXPECT_EQ(back, millic);
        }
    }
}

TEST(SwitchThermalRecord, NoRecordWithoutItsMagicOrWithAnAlarmSet)
{
    uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS];
    int millic = 0;
    switch_thermal_record_encode(SWITCH_THERMAL_PLLX, 95000, fields);
    fields[5] = 91;  // a debug durable mark
    EXPECT_EQ(switch_thermal_record_decode(fields, &millic), SWITCH_THERMAL_NONE);
    switch_thermal_record_encode(SWITCH_THERMAL_PLLX, 95000, fields);
    fields[1] |= 0x80u;
    EXPECT_EQ(switch_thermal_record_decode(fields, &millic), SWITCH_THERMAL_NONE);
    const uint8_t cleared[SWITCH_THERMAL_RECORD_FIELDS] = {0, 35, 2, 1, 1, 0, 1};
    EXPECT_EQ(switch_thermal_record_decode(cleared, &millic), SWITCH_THERMAL_NONE);
}

TEST(SwitchResetCause, DecodesT210sResetSource)
{
    EXPECT_EQ(switch_reset_cause(0x0), SWITCH_RESET_POWER_ON);
    EXPECT_EQ(switch_reset_cause(0x1), SWITCH_RESET_WATCHDOG);
    EXPECT_EQ(switch_reset_cause(0x2), SWITCH_RESET_THERMTRIP);
    EXPECT_EQ(switch_reset_cause(0x3), SWITCH_RESET_SOFTWARE);
    EXPECT_EQ(switch_reset_cause(0x4), SWITCH_RESET_LP0);
    EXPECT_EQ(switch_reset_cause(0x5), SWITCH_RESET_AOTAG);
    EXPECT_EQ(switch_reset_cause(0x6), SWITCH_RESET_UNKNOWN);
    EXPECT_EQ(switch_reset_cause(0xfffffff2u), SWITCH_RESET_THERMTRIP) << "only bits 2:0";
    EXPECT_EQ(std::string(switch_reset_cause_name(SWITCH_RESET_THERMTRIP)), "thermal trip (THERMTRIP)");
}

TEST(SwitchResetCause, FilesWatchdogThermalAndShutdownRecordsUnlessAPanicDid)
{
    EXPECT_TRUE(switch_reset_files_report(SWITCH_RESET_WATCHDOG, 0, SWITCH_THERMAL_NONE));
    EXPECT_TRUE(switch_reset_files_report(SWITCH_RESET_THERMTRIP, 0, SWITCH_THERMAL_NONE));
    EXPECT_TRUE(switch_reset_files_report(SWITCH_RESET_POWER_ON, 0, SWITCH_THERMAL_PLLX));
    EXPECT_FALSE(switch_reset_files_report(SWITCH_RESET_POWER_ON, 0, SWITCH_THERMAL_NONE));
    EXPECT_FALSE(switch_reset_files_report(SWITCH_RESET_SOFTWARE, 0, SWITCH_THERMAL_NONE));
    EXPECT_FALSE(switch_reset_files_report(SWITCH_RESET_WATCHDOG, 1, SWITCH_THERMAL_NONE));
}

}  // namespace
