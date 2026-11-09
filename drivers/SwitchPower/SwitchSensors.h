// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch's temperature sensors as iPadOS's thermalmonitord (CLTM) reads them: which physical measurement answers
// each sensor the thermal model of the IPSW's iPad expects (SwitchSensorModels.h: J120AP, J181AP), and the unit
// conversions on the way. Plain C with no kernel dependency, so the host tests (test/switch_thermal_sensors_test.cpp,
// test/switch_thermal_sensors_j181_test.cpp) check this same header; SwitchSensors.cpp is the kernel side and
// drivers/SwitchPower/CONTRACT.md the full contract with its evidence.
//
// thermalmonitord (21H461, ThermalMonitor-1952.120.7) on J120AP: /System/Library/ThermalMonitor/J120AP-Info.plist
// "Sensors" lists 43 entries; the 38 that are not synthSensor are HID sensors, and each must answer every 5 s cycle or
// thermalmonitord tells watchdogd it is unhealthy ("current 0, mask 3fffffffff"). A sensor is an IOHIDEventService
// with PrimaryUsagePage 0xff00 / PrimaryUsage 5 whose Product equals the entry's matchName; thermalmonitord reads it
// with IOHIDServiceClientCopyEvent(kIOHIDEventTypeTemperature) and takes IOHIDEventGetFloatValue(event, 0xf0000)
// (kIOHIDEventFieldTemperatureLevel, degrees C) times 100 as an int; -40 C < value < 150 C or it is a read failure.
// 24B119's thermalmonitord on J181AP reads the same way (-[HidSensors getHIDSensorDict]: the SMC first, then
// IOHIDServiceClientCopyEvent type 15, field 0xf0000), with its readings keyed by matchName.
//
// Mapping rule: every value is a real measurement, from the physical sensor nearest to what the iPad's sensor
// measures. The Switch (T210B01 "Mariko", OLED) has: SOC_THERM's six on-die tsensors (cpu0-cpu3 beside each A57 core,
// gpu, pllx; T210B01 has no mem sensors), the TMP451 (remote = the SoC's thermal diode, local = the PCB beside the
// SoC), and the MAX17050 fuel gauge's battery thermistor. The BQ24193 charger, the MAX77620/MAX77812 PMICs and the
// BM92T36 have no temperature readout (comparator/alarm bits only), so they back nothing here.
#ifndef SWITCH_SENSORS_H
#define SWITCH_SENSORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The physical sources.
enum switch_sensor_source {
    SWITCH_SRC_SOC_CPU0 = 0,  // SOC_THERM tsensor cpu0 (TS_CPU0 at +0xc0), beside A57 core 0
    SWITCH_SRC_SOC_CPU1,      // cpu1 (+0xe0)
    SWITCH_SRC_SOC_CPU2,      // cpu2 (+0x100)
    SWITCH_SRC_SOC_CPU3,      // cpu3 (+0x120)
    SWITCH_SRC_SOC_GPU,       // gpu (+0x180), in the GM20B
    SWITCH_SRC_SOC_PLLX,      // pllx (+0x1a0), at the CPU PLL, mid-die
    SWITCH_SRC_TMP451_SOC,    // TMP451 remote channel: the SoC's thermal diode (hekate's -12.5 C offset applied)
    SWITCH_SRC_TMP451_PCB,    // TMP451 local channel: the PCB at the sensor, next to the SoC
    SWITCH_SRC_BATTERY,       // MAX17050 TEMP: the battery pack's thermistor
    SWITCH_SRC_TCAL,          // the PMU's calibration channel: answers the model's reference, so no correction
    SWITCH_SRC_COUNT
};

#define SWITCH_SOCTHERM_SENSORS 6u  // SWITCH_SRC_SOC_CPU0 .. SWITCH_SRC_SOC_PLLX

struct switch_sensor {
    const char *fourcc;     // the plist's 4CharCode
    const char *matchName;  // the plist's matchName: the HID service's Product
    enum switch_sensor_source source;
    int info_only;  // the plist's infoOnly: never read (a -127 C placeholder), but its service must exist
};

#include "SwitchSensorModels.h"

// The table of the IPSW's own iPad (switch_ipsw_volumes.h's SWITCH_IPSW_DEVICE_*): J181 for iOS 27, J120 for iOS 17.
#ifdef SWITCH_IPSW_DEVICE_J181AP
#define SWITCH_SENSOR_COUNT       SWITCH_SENSOR_COUNT_J181AP
#define switch_sensors            switch_sensors_j181ap
#define SWITCH_SENSOR_TCAL_MILLIC SWITCH_SENSOR_TCAL_MILLIC_J181AP
#else
#define SWITCH_SENSOR_COUNT       SWITCH_SENSOR_COUNT_J120AP
#define switch_sensors            switch_sensors_j120ap
#define SWITCH_SENSOR_TCAL_MILLIC SWITCH_SENSOR_TCAL_MILLIC_J120AP
#endif

// The sensors thermalmonitord reads every 5 s (copyEvent), as a mask of a table's indices. It sets an infoOnly
// sensor's health bit whenever its service exists, so with no service hooked its "current" mask is exactly the infoOnly
// bits (J120: 0x9249276ba).
static inline uint64_t switch_sensor_table_read_mask(const struct switch_sensor *sensors, unsigned count)
{
    uint64_t mask = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!sensors[i].info_only)
            mask |= 1ULL << i;
    }
    return mask;
}

// NOLINTNEXTLINE(modernize-redundant-void-arg): C (the host harness) reads this header too
static inline uint64_t switch_sensors_read_mask(void)
{
    return switch_sensor_table_read_mask(switch_sensors, SWITCH_SENSOR_COUNT);
}

// A 4CC as the 32-bit big-endian integer thermalmonitord compares a service's LocationID with.
static inline uint32_t switch_sensor_fourcc(const char *fourcc)
{
    return (uint32_t)(uint8_t)fourcc[0] << 24 | (uint32_t)(uint8_t)fourcc[1] << 16 | (uint32_t)(uint8_t)fourcc[2] << 8 |
           (uint32_t)(uint8_t)fourcc[3];
}

// m C as the IOFixed (16.16 degrees C) the temperature event carries, rounded to nearest.
static inline int32_t switch_sensor_fixed(int32_t milli_c)
{
    const int64_t scaled = (int64_t)milli_c * 65536;
    return (int32_t)(scaled >= 0 ? (scaled + 500) / 1000 : (scaled - 500) / 1000);
}

// What thermalmonitord makes of that IOFixed: (int)(IOHIDEventGetFloatValue * 100), centi-degrees.
static inline int32_t switch_sensor_cltm_centi(int32_t fixed)
{
    return (int32_t)((double)fixed / 65536.0 * 100.0);
}

// thermalmonitord's -[HidSensors isTempSensorDataValid:value:] on centi-degrees: (uint32)(v - 15000) > 0xffffb5c8.
static inline int switch_sensor_cltm_valid(int32_t centi)
{
    return (uint32_t)(centi - 15000) > 0xffffb5c8u ? 1 : 0;
}

// Only physically plausible readings are handed to thermalmonitord; anything else is a read failure (no event).
static inline int switch_sensor_plausible(int32_t milli_c)
{
    return milli_c > -40000 && milli_c < 150000 ? 1 : 0;
}

// SOC_THERM readback (TS_*_STATUS1 bits 15:0, SENSOR_TEMP1/2 halves): TTTTTTTT H______N, T = degrees, H = +0.5, N =
// negate (Linux soctherm.c translate_temp). m C.
static inline int32_t switch_soctherm_millic(uint32_t readback)
{
    int32_t t = (int32_t)((readback & 0xff00u) >> 8) * 1000;
    if (readback & 0x80u)
        t += 500;
    if (readback & 0x1u)
        t = -t;
    return t;
}

// MAX17050 TEMP (0x08): signed, 1/256 C per LSB (hekate max17050.c). m C, truncated toward zero.
static inline int32_t switch_max17050_millic(uint16_t raw)
{
    return (int32_t)((int64_t)(int16_t)raw * 1000 / 256);
}

// The MAX17050 temperature alert window (TAlrtTh 0x02: max in 15:8, min in 7:0, signed whole degrees; ALRT when TEMP
// leaves it) around a reading: one degree either side of the reading rounded to a whole degree, so a change of about
// a degree either way raises ALRT and the current reading never violates it.
static inline uint16_t switch_max17050_talrt(int32_t milli_c)
{
    const int32_t whole = milli_c >= 0 ? (milli_c + 500) / 1000 : -((-milli_c + 500) / 1000);  // nearest
    int32_t hi = whole + 1;
    int32_t lo = whole - 1;
    if (hi > 127)
        hi = 127;
    if (hi < -127)
        hi = -127;
    if (lo < -128)
        lo = -128;
    if (lo > 126)
        lo = 126;
    return (uint16_t)((unsigned)(uint8_t)(int8_t)hi << 8 | (uint8_t)(int8_t)lo);
}

// --- SOC_THERM fuse calibration (Linux drivers/thermal/tegra/tsensor-fuse.c and tegra210-soctherm.c, T210B01) ----

struct switch_tsensor_config {
    uint32_t tall, tiddq_en, ten_count, tsample, tsample_ate, pdiv, pdiv_ate;
};
// tegra210b01_tsensor_config
static const struct switch_tsensor_config switch_tsensor_b01 = {16300, 1, 1, 240, 480, 12, 6};

struct switch_tsensor_desc {
    const char *name;
    uint32_t base;        // register block in SOC_THERM
    uint32_t calib_fuse;  // FUSE_TSENSORn_CALIB, from the fuse cache (fuse block + 0x100)
    int32_t alpha, beta;  // fuse_corr
};
// tegra210b01_tsensors, in SWITCH_SRC_SOC_* order.
static const struct switch_tsensor_desc switch_tsensors[SWITCH_SOCTHERM_SENSORS] = {
    {"cpu0", 0xc0, 0x098, 1085000, 3244200},  {"cpu1", 0xe0, 0x084, 1126200, -67500},
    {"cpu2", 0x100, 0x088, 1098400, 2251100}, {"cpu3", 0x120, 0x12c, 1108000, 602700},
    {"gpu", 0x180, 0x154, 1074300, 2734900},  {"pllx", 0x1a0, 0x160, 1039700, 6829100},
};
#define SWITCH_FUSE_TSENSOR_COMMON 0x180u

struct switch_tsensor_shared {
    int32_t base_cp, base_ft, actual_temp_cp, actual_temp_ft;
};

static inline int32_t switch_sign_extend32(uint32_t value, unsigned index)
{
    const unsigned shift = 31 - index;
    // NOLINTNEXTLINE(bugprone-signed-bitwise): an arithmetic shift right sign-extends the field
    return (int32_t)(value << shift) >> shift;
}

// div64_s64_precise(s64 a, s32 b): a scaled up by 16 bits, rounded division, scaled back (arithmetic shift).
static inline int64_t switch_div64_precise(int64_t a, int32_t b)
{
    const int64_t al = a * 65536;
    const int64_t r = (al * 2 + 1) / ((int64_t)b * 2);
    // NOLINTNEXTLINE(bugprone-signed-bitwise): an arithmetic shift, as Linux's div64_s64_precise
    return r >> 16;
}

// tegra_calc_shared_calib with tegra210_soctherm_fuse (no spare realignment: shifted_cp is the common word's low bits).
static inline struct switch_tsensor_shared switch_tsensor_shared_calib(uint32_t common)
{
    struct switch_tsensor_shared s;
    s.base_cp = (int32_t)((common & (0x3ffu << 11)) >> 11);
    s.base_ft = (int32_t)((common & (0x7ffu << 21)) >> 21);
    const int32_t shifted_ft = switch_sign_extend32((common & (0x1fu << 6)) >> 6, 4);
    const int32_t shifted_cp = switch_sign_extend32(common, 5);
    s.actual_temp_cp = 2 * 25 + shifted_cp;   // NOMINAL_CALIB_CP
    s.actual_temp_ft = 2 * 105 + shifted_ft;  // NOMINAL_CALIB_FT
    return s;
}

// tegra_calc_tsensor_calib: the TS_*_CONFIG2 word (therma 31:16, thermb 15:0) for one sensor's calibration fuse.
static inline uint32_t switch_tsensor_calib(const struct switch_tsensor_config *cfg,
                                            const struct switch_tsensor_shared *shared,
                                            const struct switch_tsensor_desc *sensor, uint32_t fuse)
{
    const int32_t cp = shared->base_cp * 64 + switch_sign_extend32(fuse & 0x1fffu, 12);
    const int32_t ft = shared->base_ft * 32 + switch_sign_extend32((fuse >> 13) & 0x1fffu, 12);
    const int32_t delta_sens = ft - cp;
    const int32_t delta_temp = shared->actual_temp_ft - shared->actual_temp_cp;
    const int32_t mult = (int32_t)(cfg->pdiv * cfg->tsample_ate);
    const int32_t div = (int32_t)(cfg->tsample * cfg->pdiv_ate);
    if (!delta_sens)
        return 0;
    int64_t temp = (int64_t)delta_temp * (1LL << 13) * mult;
    int16_t therma = (int16_t)switch_div64_precise(temp, (int32_t)((int64_t)delta_sens * div));
    temp = (int64_t)ft * shared->actual_temp_cp - (int64_t)cp * shared->actual_temp_ft;
    int16_t thermb = (int16_t)switch_div64_precise(temp, delta_sens);
    temp = (int64_t)therma * sensor->alpha;
    therma = (int16_t)switch_div64_precise(temp, 1000000);
    temp = (int64_t)thermb * sensor->alpha + sensor->beta;
    thermb = (int16_t)switch_div64_precise(temp, 1000000);
    return (uint32_t)(uint16_t)therma << 16 | (uint32_t)(uint16_t)thermb;
}

// enable_tsensor: CONFIG0 (TALL 27:8, STOP 0 clear) and CONFIG1 (TSAMPLE-1 9:0, TIDDQ_EN 20:15, TEN_COUNT 29:24,
// TEMP_ENABLE 31).
static inline uint32_t switch_tsensor_config0(const struct switch_tsensor_config *cfg)
{
    return cfg->tall << 8;
}

static inline uint32_t switch_tsensor_config1(const struct switch_tsensor_config *cfg)
{
    return (cfg->tsample - 1) | cfg->tiddq_en << 15 | cfg->ten_count << 24 | 1u << 31;
}

// --- thermalmonitord's CPU ceiling (CONTRACT.md "Mitigation") --------------------------------------------------------

// The four DVD factor contributors CLTM sets on ApplePMGR, each a 16.16 fraction (0x10000 = no limit; CLTM writes
// value < 100 ? value * 65536 / 100 : 0x10000). The ceiling in force is the lowest.
#define SWITCH_CLTM_CONTRIBUTORS 4u
static const char *const switch_cltm_keys[SWITCH_CLTM_CONTRIBUTORS] = {
    "AppleARMPerformanceControllerDVDFactor1-slowloop",
    "AppleARMPerformanceControllerDVDFactor1-hip",
    "AppleARMPerformanceControllerDVDFactor1-upo",
    "AppleARMPerformanceControllerDVDFactor1-lpm",
};

static inline uint32_t switch_cltm_ceiling(const uint32_t fraction[SWITCH_CLTM_CONTRIBUTORS])
{
    uint32_t c = 0x10000u;
    for (unsigned i = 0; i < SWITCH_CLTM_CONTRIBUTORS; ++i) {
        if (fraction[i] < c)
            c = fraction[i];
    }
    return c;
}

#ifdef __cplusplus
}
#endif
#endif
