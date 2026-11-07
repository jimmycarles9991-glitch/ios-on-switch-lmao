// SPDX-License-Identifier: BSD-2-Clause-Patent
// The MAX77620 on I2C5 as SwitchPMIC.cpp (wall clock, wake alarm, power-off) and SwitchPowerMark.cpp (the durable power
// mark) share it: addresses, the RTC's registers (Linux rtc-max77686.c max77620_drv_data, hekate bdk/rtc/max77620-rtc.h)
// and one rule for its updates. The RTC's registers are buffers: writing RTCUPDATE0 UDR copies them into the RTC,
// RBUDR copies the RTC into them, and either takes up to 16 ms (Linux .delay = 16000 us), during which the RTC takes no
// other register write or update. Every writer waits for max77620::rtcQuiet() before its first write and calls
// max77620::rtcUpdated() after writing UDR or RBUDR.
#ifndef SWITCH_MAX77620_H
#define SWITCH_MAX77620_H
#include <stdint.h>
#include "../TegraI2C/TegraI2C.h"
#include "../../diagnostics/switch/SwitchResetCause.h"

namespace max77620 {
constexpr unsigned kBus = 5;
constexpr uint8_t kPmic = 0x3c, kRtc = 0x68;

enum : uint8_t {
    RTC_INT = 0x00,
    RTC_INT_MASK = 0x01,
    RTC_CONTROLM = 0x02,
    RTC_CONTROL = 0x03,
    RTC_UPDATE0 = 0x04,
    RTC_SEC = 0x07,
    RTC_ALARM1 = 0x0e,
    RTC_ALARM2 = 0x15
};

// The fields of the time and both alarms, in register order from RTC_SEC / RTC_ALARM1 / RTC_ALARM2.
enum {
    SEC,
    MIN,
    HOUR,
    WEEKDAY,
    MONTH,
    YEAR,
    DATE,
    kTimeRegs
};

constexpr uint8_t kRtcWriteUpdate = 1u << 0, kRtcReadUpdate = 1u << 4;
constexpr uint32_t kRtcUpdateMS = 16;

inline int readByte(uint8_t addr, uint8_t reg, uint8_t *value)
{
    return tegra_i2c_read(kBus, addr, reg, value, 1);
}

inline int writeByte(uint8_t addr, uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kBus, addr, reg, &value, 1);
}

// Until the last update is done: asleep when `canSleep`, else spinning (interrupts off). SwitchPowerMark.cpp.
void rtcQuiet(bool canSleep);
void rtcUpdated();
}  // namespace max77620

#if DEVELOPMENT || DEBUG
// SwitchPowerMark.cpp, from switch_pmic_start once the PMIC answers (thread context): the last boot's durable mark is
// logged, and this boot's first replaces it.
extern "C" void switch_pmic_mark_start(void);
#endif

// SwitchPowerMark.cpp, thread context, I2C5 up (SwitchThermalRecord.h). record: a software thermal shutdown writes its
// sensor and temperature into ALARM2 and holds it there against debug marks; 0 or the I2C error. take: switch_pmic_start,
// before the debug marks start: the last boot's record, cleared once read; SWITCH_THERMAL_NONE when there is none.
extern "C" int switch_pmic_thermal_record(enum switch_thermal_sensor sensor, int millic);
extern "C" enum switch_thermal_sensor switch_pmic_thermal_take(int *millic);

#endif
