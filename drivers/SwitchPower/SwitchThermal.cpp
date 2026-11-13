// SPDX-License-Identifier: BSD-2-Clause-Patent
// Thermal safety for the Nintendo Switch OLED (T210B01, "Aula"): SoC and PCB temperature from the
// TMP451 on I2C1 and the fan on PWM1, on this service's own work loop. The fan curve is evaluated
// when the TMP451 ALERT line fires (temperature left the window programmed around the current fan
// level and the DFLL's limits) and whenever SOC_THERM's CPU, GPU or PLLX group moves a degree
// (SwitchSocTherm.h, its own threshold interrupt), which keeps the TMP451 reading thermalmonitord
// sees current; nothing re-reads on a timer. Each evaluation also checks that a pending ALERT reached
// the CPU (tegra_gpio_check_delivery panics when it did not).
//
// Hardware:
//   * TMP451 at 0x4C on I2C1 (switchroot tegra210b01-nx-common.dtsi temp-sensor@4c; hekate
//     bdk/thermal/tmp451.{c,h}). Remote diode = SoC, local = PCB. Integer degrees in 0x01 / 0x00,
//     sixteenths in bits 7:4 of 0x10 / 0x15, status 0x02 (bit 2 remote open; 3/4 remote low/high,
//     5/6 local low/high), config read 0x03 / write 0x09 (bit 7 ALERT masked, bit 6 shutdown, bit 5
//     THERM2 mode), conversion rate 0x0A, limits: local high/low write 0x0B/0x0C, remote high/low
//     0x0D/0x0E (+ sixteenths 0x13/0x14), remote offset 0x11/0x12, THERM limits 0x19 (remote) /
//     0x20 (local), manufacturer ID 0xFE = 0x55 (L4T drivers/misc/nct1008.c register map).
//   * ALERT (interrupt mode, active low) is wired to GPIO PX4 (DT interrupts = <TEGRA_GPIO(X, 4)
//     IRQ_TYPE_LEVEL_LOW>); it asserts while a reading is above a high or at/below a low limit, and
//     reading the status register clears it once the condition is gone. L4T's nct1008 driver handles
//     it the same way: reprogram the limits around the current trip, then read the status.
//   * Initialized like hekate tmp451_init() (CONFIG 0x80 there: ALERT masked, running, 0..127 C;
//     0x00 here: ALERT unmasked). Nyx's hw_deinit (BDK_HW_EXTRA_DEINIT) leaves
//     the sensor in shutdown (tmp451_end, 0xC0), where the registers keep their last value, so it is
//     always written. Remote offset -12.5 C on T210B01 (hekate 0xF3/0x80; L4T DT offset-hi-res
//     0xF38 agrees). THERM limits as L4T (DT loc-shutdown-limit 120, ext-shutdown-limit 99, written
//     by nct1008_{loc,ext}_sensor_init); where THERM goes on this board is unverified.
//   * Fan (hekate bdk/thermal/fan.c and power/regulator_5v.c, T210B01 path): 5 V battery regulator
//     enable on GPIO PA5 (pinmux SATA_LED_ACTIVE), fan PWM on PWM1 (0x7000A010) through pin
//     LCD_GPIO2 / PV4 in SFIO mode. Inverted: the duty field (bits 24:16, 256 = always high) is the
//     time the line is high, and high stops the fan. Hekate enables the PWM clock for the fan only
//     on Aula (no PWM backlight there): CLK_M / 3 = 6.4 MHz, so 25 kHz with PFM 0. The regulator
//     stays on (it also feeds the Joy-Con rails); a stopped fan is PWM always-high, as in hekate.
//     The fan has no interrupt of its own (tachometer on PS7 is not used).
//   * Curve: L4T thermal-fan-est (CTCaer switch-l4t-kernel-nvidia drivers/misc/therm_tskin_fan_est.c)
//     with the OLED's coefficients and profiles (switchroot tegra210b01-fric.dts, hysteresis from
//     tegra210b01-nx-common.dtsi): skin = max over {SoC, PCB} of (T * a + b * 1000) / 10000 + 500
//     (m C); the trip level rises when skin reaches a trip and falls below trip - hysteresis; the
//     fan gets the pwm-fan active_pwm of that level, out of active_pwm_max 256. Console is L4T's
//     default profile, the one used; Handheld is its quieter-skin, louder-fan one.
//
// Fail-safe: start() runs the fan at kSafeDrive with MMIO only, before any I2C. A failed reading
// (I2C error, wrong ID, open SoC diode, sensor found in shutdown) runs it at full speed until a
// good reading comes back; the sensor is re-initialized a second later. The ALERT event source is
// enabled once the first window is programmed, and is off while the sensor is being re-initialized
// (the line cannot be released then; the fan is at full speed meanwhile). An ALERT line that keeps
// firing without being released (more than kAlertBurst times in a second) panics with the GPIO state.
//
// CPU operating point. XNU does no DVFS, so the A57 cluster otherwise stays
// where hekate left it: 998.4 MHz on PLLX at 800 mV (SwitchClocks.h). Only from this service's own work
// loop, once it has a good reading and the ALERT armed, the CPU goes on L4T's closed-loop DFLL with an on-demand
// governor (the default, see "DFLL on demand" below), or climbs a short ladder of L4T rates on PLLX and stays there
// (boot-arg switch-no-dfll; also the fallback whenever the DFLL or its governor fails). Sources: "L4T" =
// CTCaer/switch-l4t-kernel-4.9 2d0059fd; DT = switchroot tegra210b01-nx-pmic.dtsi / nx-common.dtsi.
//   * L4T has no PLLX voltage for these rates on T210B01. Its PLL table (tegra210-dvfs.c:571-601,
//     CPUB01_PLL_CVB_TABLE) is a flat 1120 mV from 1020 MHz, but set_cpu_dvfs_data() adds CVB_PLL_MARGIN
//     (30 %, :39, :1151) = 1456 mV, above the CPU's DVFS ceiling (rail nominal, :1071, :1179), so
//     tegra-dvfs.c:545-549 refuses every PLLX rate above 918 MHz: L4T runs them only on the DFLL, at the
//     per-chip CVB voltage (clk-tegra124-dfll-fcpu.c:610-648 CPUB01_CVB_TABLE, the row every T210B01
//     speedo takes, :650-676). So the PLL ladder here is our rule, built from L4T's numbers: VDD_CPU at
//     kPerfMilliVolts = 1120 mV, L4T's T210B01 CPU maximum (:654; the vdd-cpu rail max is 1125,
//     tegra210-dvfs.c:187), and a rung only if this chip's L4T CVB voltage for it plus kPllMarginPct
//     (20 %) fits under 1120 mV. A fixed clock cannot slow down under droop as the DFLL does; on T210,
//     where L4T has both tables, the raw PLL characterization asks 5-17 % more than the DFLL CVB at
//     1734 MHz (CPU_PLL_CVB_TABLE vs CPU_CVB_TABLE), and the GPU uses the same 20 % (TegraGM20B gpu_clock.c
//     FIXED_PLL_MARGIN_PCT). The CPU speedo is FUSE_CPU_SPEEDO_0 as L4T's speedo-tegra210.c:273,292-293
//     reads it on T210B01 (process 0 below 1650, :52; speedo ID 3 for SKU 0x87, else 0, :180-207; both
//     IDs use CPUB01_CVB_TABLE). Typical speedos (1600-1700) reach 1683 or 1785 MHz; a slow chip stops
//     lower. Every rung is far below L4T's default top, 1963.5 MHz (nx-common.dtsi max-safe-frequency).
//   * Rungs: L4T's freq-table rows 1224/1428/1581/1683/1785 MHz programmed as
//     tegra210b01_pll_fixed_mdiv_cfg() (clk-tegra210b01.c:1260-1293) does: M = 2 (19.2 MHz comparison),
//     P = ceil(vco_min 1300 MHz / rate), N = rate * P / 19.2 MHz rounded down (1219.2, 1420.8, 1574.4,
//     1670.4, 1766.4 MHz). Each rung parks the cluster on PLLP_OUT4 (408 MHz) while PLLX is stopped,
//     given its dividers and relocked, as L4T's _program_pll (clk-pll.c) under tegra124-cpufreq's backup
//     does. That backup is CCLK source 5, pll_p_out4, not source 4 (pll_p): the T210 cpu node's "pll_p" is
//     TEGRA210_CLK_PLL_P_OUT4 (Linux tegra210.dtsi:1950-1954), and L4T builds it as pll_p -> the
//     pll_p_out_cpu gate (CLK_OUT_ENB_Y bit 31, peripheral 223) -> the PLLP_OUTB OUT4 divider and enable
//     (clk-tegra-periph.c:863,1039-1060). Hekate never opens that gate (only CLK_OUT_ENB_Y = MC_CDPA|MC_CCPA,
//     and on T210 only, hw_init.c:253); it leaves OUT4 at 408 MHz, enabled (PLLP_OUTB 0x30003, :393). The
//     gate is opened before the mux moves and closed once the cluster is back on PLLX, as the clock framework
//     does around clk_set_parent; L4T opens it by hand around SC7 (clk-tegra210.c:3990-3993,4155-4158).
//     (On hardware, moving every source to 4 with the gate shut stopped the cluster at once.) The cluster
//     mux is L4T's CCLKG_BURST_POLICY 0x368 (clk-tegra-super-gen4.c:33), and as clk_super_set_parent
//     (clk-super.c:72-120) only the RUN source field changes, followed by 2 us; SUPER_CCLKG_DIVIDER is left
//     alone (1:1 as hekate sets it; L4T only touches its thermal-control bit). The CCLK_BURST_POLICY alias at
//     0x20 that hekate writes must read the same, and the mux must read back PLLP_OUT4 before PLLX is ever
//     stopped: a cluster still on PLLX would stop dead with it.
//   * VDD_CPU is the MAX77812's M4 on I2C5 (DT max77812@33 m4vout "vdd-cpu"). Hekate never configures
//     the chip (bdk/power/max7762x.c), so its current limit is whatever the PMIC came up with; L4T sets
//     M4's to 7.2 A (DT maxim,peak-current-limit-ua; max77812-regulator.c:283-357) and so, by
//     hekate's note, does HOS (max77812.h M4_CFG). That is done before VDD_CPU moves. The DT's slew is
//     5 mV/us; the 1 ms settle covers the slowest code (1.25 mV/us) for any step here. An ES2 MAX77812,
//     which L4T trims first (:229-276), is refused.
//   * Steps, one log line each with perfTrace (otherwise one line at the end): 1 relock PLLX at
//     the boot dividers through PLLP_OUT4 (the mux path with nothing electrical changed), 2 M4 current limit,
//     3 VDD_CPU 1120 mV at the boot clock, 4.. one rung each. perfSteps = N stops after step N (0:
//     read and report only), so a hardware test can stop below a failing step; both are set in source for such a
//     test, never at boot. perfTrace logs before every sub-step and then sleeps kTraceMS, after a kTraceDelayMS start delay, so the lagging
//     console has drawn the step in flight when anything stops; it holds the work loop meanwhile (ALERTs
//     wait). Down: the clock first, then VDD_CPU. Other CPUs and interrupts keep running throughout: the
//     super-clock mux switches glitch-free (L4T clk-super.c switches it under a spinlock with udelay(2)),
//     the generic timer is not on CCLK, and TegraI2C serializes I2C5 with the GPU's VDD_GPU traffic (it
//     writes M1_VOUT and EN_CTRL, never M4).
//   * DFLL on demand (the default): L4T's normal mode, the DFLL in closed loop (drivers/TegraDFLL,
//     vendored clk-dfll.c), entered at the 1683 MHz CVB row and taken to 1785 MHz (Horizon's boost clock, the most this
//     port ever runs: TEGRA_DFLL_CPU_MAX_HZ) only as measured at or under it (settleDfllTop: the first request after
//     the loop closes can run a DFLL step high), then moved by SwitchPerfGovernor.cpp between the DFLL's own OPP rows
//     from the CVB Vmin
//     row (918 MHz at 620 mV on speedo 1659) up to 1785 MHz as the scheduler's load, touches, missed frames and
//     launches ask; the boot holds the top until System Boot Complete. Each pair is L4T's: the DFLL's CVB voltage for
//     that row, set by the DFLL itself over its I2C master. A rate the DFLL refuses logs one line and falls back to
//     the PLL ladder below (today's fixed 1670 MHz at 1120 mV on this chip) for the rest of the boot.
//   * governed false on the DFLL: the same DFLL, fixed at the measured top (the 1785 MHz row, else 1683), at the per-chip
//     CVB voltage (roughly 875-1015 mV for CPU speedos 1850-1450). In both DFLL modes the thermal trips do not drop
//     the clock; they set the DFLL's L4T voltage cap and floor indices (cap 1060 mV from 64 C, 1010 mV from 84 C,
//     floor 800 mV below 20 C; 1 C hysteresis, tegra210b01-nx-common.dtsi dfll-cap-trip*/dfll-floor-trip0), and the
//     ALERT window is
//     cut at the next of those boundaries so each change comes by interrupt (governed: they also become the
//     governor's lowest and highest points, the rows at or under the floor and cap voltages). If the DFLL cannot be
//     prepared or enabled (one log line saying why; it refuses while I2C5 is not in multi-master mode) the
//     PLL mode is used for the rest of the boot. Losing the sensor returns the CPU to PLLX for good.
//     switch_thermal_cpu_quiesce() hands the CPU back to PLLX before SwitchPMIC's shut-down and restart
//     traffic on I2C5, where the DFLL would be a second master.
//   * Thermal trip: L4T caps VDD_CPU at 1060 mV from TEGRA210B01_DFLL_THERMAL_CAP_0 = 64 C
//     (dt-bindings/thermal/tegra210b01-trips.h; tegra210b01_thermal_cap_table, clk-tegra124-dfll-fcpu.c),
//     and 1120 mV is above that cap. So the SoC diode reaching kPerfTripMilliC drops the CPU back to
//     the inherited PLLX and voltage, with one log line. The drop is temporary: L4T's trip
//     (tegra210b01-nx-common.dtsi dfll-cap-trip0) has a 1 C hysteresis, so the CPU climbs again once
//     the SoC reads below kPerfTripMilliC - kPerfHysteresisMilliC, but never sooner than kPerfHoldMS
//     after the drop, so it cannot flap faster than that. While raised, the ALERT window's SoC high
//     limit is capped at the trip; while waiting to go up again, its low limit is raised to the
//     re-raise temperature: both edges come by interrupt (the hold-off expiry is a one-shot of the
//     timer). (L4T reads that trip on SOC_THERM's PLL sensor; this is the TMP451's remote diode with
//     hekate's -12.5 C offset: close, not identical.) Losing the sensor, or a raise that is refused,
//     drops the CPU for the rest of the boot.
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <sys/reboot.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchSuspend/SwitchWake.h"
#include "SwitchClocks.h"
#include "../TegraDFLL/TegraDFLL.h"
#include "SwitchGovernor.h"
#include "SwitchPerfGovernor.h"
#include "SwitchPower.h"
#include "SwitchEMC.h"
#include "SwitchPowerStats.h"
#include "SwitchMAX77620.h"
#include "SwitchSocTherm.h"
#include "SwitchThermalLimits.h"
#include <os/log.h>
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

extern "C" int tegra_gm20b_bringup_busy(void);  // drivers/TegraGM20B/TegraGM20B.cpp
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" int reboot_kernel(int howto, char *message);  // bsd/kern/kern_shutdown.c, reboot(2)'s kernel half

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000;
constexpr vm_offset_t kPwmPA = 0x7000A000, kPmcPA = 0x7000E400;
constexpr vm_offset_t kFusePA = 0x7000F800, kApbMiscPA = 0x70000800;

// Clock and reset controller (hekate bdk/soc/clock.{c,h}); PWM is device L17.
enum : uint32_t {
    RST_DEVICES_L = 0x004,
    CLK_OUT_ENB_L = 0x010,
    MISC_CLK_ENB = 0x048,
    CLK_SOURCE_PWM = 0x110,
    RST_DEV_L_SET = 0x300,
    RST_DEV_L_CLR = 0x304,
    CLK_ENB_L_SET = 0x320,
    CLK_ENB_L_CLR = 0x324,
    CLK_ENB_H_SET = 0x328,
    CCLKG_BURST_POLICY = 0x368,
    SUPER_CCLKG_DIVIDER = 0x36c,
    PLLX_MISC3 = 0x518,
    PLLP_BASE = 0x0a0,
    PLLP_OUTB = 0x0a8,
    CLK_OUT_ENB_Y = 0x298,
    CLK_ENB_Y_SET = 0x29c,
    CLK_ENB_Y_CLR = 0x2a0,
    RST_DEVICES_Y = 0x2a4,
    RST_DEV_Y_CLR = 0x2ac,
};

constexpr uint32_t kPwmClock = 1u << 17;
constexpr uint32_t kFuseClock = 1u << 7, kFuseVisible = 1u << 28;  // CLK_ENB_H bit 7, MISC_CLK_ENB bit 28
constexpr uint32_t kPllxIddq = 1u << 3;                            // PLLX_MISC3 (clk-tegra210b01.c:373)
constexpr uint32_t kPllpOutCpu = 1u << 31;   // bank Y bit 31: the PLLP branch to the CPU (hekate CLK_Y_PLLP_OUT_CPU)
constexpr uint32_t kOut4Running = 3u << 16;  // PLLP_OUTB OUT4 RSTN (16) and CLKEN (17), divider 31:24
constexpr unsigned kSourcePllpOut4 = 5;      // CCLK_G source pll_p_out4 (clk-tegra-super-gen4.c cclk_g_parents_gen5)
constexpr uint32_t kPwmSource = (6u << 29) | 4u;  // CLK_M, 7.1 divider 4 (/3): 6.4 MHz (hekate _clock_pwm)

// Power management controller (hekate bdk/soc/pmc_t210.h): GPIO pads powered and at 1.8 V.
enum : uint32_t {
    NO_IOPOWER = 0x44,
    PWR_DET_VAL = 0xe4
};

constexpr uint32_t kPmcGpio = 1u << 21;

// Pinmux (hekate bdk/soc/pinmux.h): function 1 = GPIO PA5 / PWM1, no pull, driven.
enum : uint32_t {
    PINMUX_SATA_LED_ACTIVE = 0x04c,
    PINMUX_LCD_GPIO2 = 0x20c
};

// GPIO masked registers (hekate gpio.c port offsets; bits 15:8 select, 7:0 value as in Linux
// gpio-tegra tegra_gpio_mask_write). Port A = bank 0 port 0, port V = bank 5 port 1.
enum : uint32_t {
    MSK_CNF = 0x80,
    MSK_OE = 0x90,
    MSK_OUT = 0xa0
};

constexpr uint32_t kPortA = 0x000, kPortV = 0x504;
constexpr unsigned kPinPA5 = 5, kPinPV4 = 4;

constexpr uint32_t PWM_CSR_1 = 0x10, kPwmEnable = 1u << 31;

constexpr unsigned kBus = 1;
constexpr uint8_t kSensor = 0x4c;

enum : uint8_t {
    LocalTemp = 0x00,
    RemoteTemp = 0x01,
    Status = 0x02,
    ConfigRead = 0x03,
    ConfigWrite = 0x09,
    RateWrite = 0x0a,
    LocalHighWrite = 0x0b,
    LocalLowWrite = 0x0c,
    RemoteHighWrite = 0x0d,
    RemoteLowWrite = 0x0e,
    RemoteFraction = 0x10,
    RemoteOffsetHigh = 0x11,
    RemoteOffsetLow = 0x12,
    RemoteHighFraction = 0x13,
    RemoteLowFraction = 0x14,
    LocalFraction = 0x15,
    RemoteThermLimit = 0x19,
    LocalThermLimit = 0x20,
    ManufacturerID = 0xfe,
};

constexpr uint8_t kConfigShutdown = 0x40, kStatusOpen = 0x04;
constexpr uint8_t kTexasInstruments = 0x55;
constexpr uint8_t kRateFourPerSecond = 6;

constexpr uint32_t kRetryMS = 1000;      // re-initialize a failed sensor (fan at full speed meanwhile)
constexpr uint32_t kConversionMS = 300;  // > one conversion at 4/s: the status then reflects new limits
constexpr unsigned kAlertBurst = 10;
constexpr unsigned kSafeDrive = 153;  // of 256, until the first good reading
constexpr unsigned kFullDrive = 256;
constexpr int kCriticalMilliC = 90000;
constexpr unsigned kLevels = 10;

// CPU operating point (see the top of the file).
constexpr unsigned kPerfMilliVolts = 1120;  // L4T's T210B01 CPU max_millivolts
constexpr uint8_t kPerfVout = (kPerfMilliVolts - 250) / 5;
constexpr unsigned kPllMarginPct = 20;  // over the per-chip DFLL CVB voltage, for a fixed clock
constexpr int kPerfTripMilliC = 64000;
constexpr int kPerfHysteresisMilliC = 1000;               // L4T dfll-cap-trip0 hysteresis = <1000>
constexpr uint32_t kPerfHoldMS = 5000;                    // shortest time from a drop to the next raise
constexpr uint32_t kRailSettleMS = 1;                     // 520 mV at the slowest MAX77812 slew, 1.25 mV/us, is 416 us
constexpr uint32_t kTraceMS = 400, kTraceDelayMS = 2000;  // perfTrace pacing
constexpr unsigned kStepRelock = 1, kStepCurrentLimit = 2, kStepRail = 3, kStepFirstRung = 4;

// L4T's T210B01 CPU ladder above the boot clock: freq-table row, PLLX N and P code with M = 2 (top of the
// file), and the row's CPUB01_CVB_TABLE coefficients (clk-tegra124-dfll-fcpu.c:625-630).
constexpr uint32_t kPllxM = 2;

struct Rung {
    unsigned rowMHz;
    uint32_t n, pdivCode;  // P code 0 = /1, 1 = /2 (pll_qlin_pdiv_to_hw)
    int32_t c0, c1, c2;
};

constexpr Rung kRungs[] = {
    {1224, 127, 1, 1141084, -27735, 27},  // 38.4 MHz * 127 / 2 / 2 = 1219.2 MHz
    {1428, 74, 0, 1268729, -30745, 27},   // 1420.8 MHz
    {1581, 82, 0, 1374032, -33005, 27},   // 1574.4 MHz
    {1683, 87, 0, 1448791, -34505, 27},   // 1670.4 MHz
    {1785, 92, 0, 1527196, -36015, 27},   // 1766.4 MHz
};
constexpr unsigned kRungCount = sizeof(kRungs) / sizeof(kRungs[0]);

// Fuses (L4T offsets from the fuse cache at +0x100: fuse-tegra30.c, speedo-tegra210.c) and APB_MISC HIDREV.
constexpr uint32_t FUSE_CACHE = 0x100, FUSE_SKU_INFO = 0x010, FUSE_CPU_SPEEDO_0 = 0x014, APB_MISC_HIDREV = 0x004;
constexpr unsigned kB01ProcessSpeedo = 1650;  // speedo-tegra210.c:52

// MAX77812 (L4T max77812-regulator.c:28-101).
constexpr uint8_t kMax77812Version = 0x14, kMax77812M4Cfg = 0x32, kMax77812GlbCfg2 = 0x34;
constexpr uint8_t kPeakCurrentMask = 0x70, kPeakCurrent7200mA = 7u << 4, kVersionEs2 = 4;

// DFLL mode: the 1785 MHz CVB row (an exact multiple of the DFLL's 25.5 MHz step) and L4T's
// T210B01 thermal boundaries for the DFLL's floor and cap indices (dt-bindings/thermal/tegra210b01-trips.h
// TEGRA210B01_DFLL_THERMAL_FLOOR_0 / _CAP_0 / _CAP_1; 1 C hysteresis as the DT trips).
constexpr unsigned long kDfllRateHz = 1785000000UL;
static_assert(kDfllRateHz <= TEGRA_DFLL_CPU_MAX_HZ, "never above Horizon's boost clock");
// The top, by measurement (settleDfllTop): enable asks for the CVB row below it, re-asks it from the row below
// that, and asks for 1785 MHz only once a request has measured right. One DFLL step is the 51 MHz reference / 2.
constexpr unsigned long kDfllSafeHz = 1683000000UL, kDfllBounceHz = 1581000000UL, kDfllStepHz = 25500000UL;
static_assert(kDfllSafeHz + kDfllStepHz < kDfllRateHz, "one step over the safe row stays under the top");
constexpr uint32_t kDfllSettleMS = 10;  // a request's closed-loop settle, tune transition included (enable waits this)
constexpr uint32_t kDfllCeilingCheckMS = 10;  // after a governor move to its ceiling, before it is measured
constexpr int kDfllFloorWarmMilliC = 20000, kDfllCap1MilliC = 64000, kDfllCap2MilliC = 84000;
constexpr int kDfllBoundaries[] = {kDfllFloorWarmMilliC, kDfllCap1MilliC, kDfllCap2MilliC};

struct Profile {
    const char *name;
    int32_t soc[2], pcb[2];  // skin estimate scale, offset
    int32_t trip[kLevels];   // m C
    int32_t hysteresis[kLevels];
    uint16_t pwm[kLevels];  // of 256
};

constexpr Profile kProfiles[] = {
    {"Console",
     {7831, 57590},
     {8051, -45213},
     {0, 36000, 40000, 43000, 49000, 53000, 70000, 80000, 90000, 100000},
     {0, 0, 3900, 2900, 0, 0, 0, 0, 0, 0},
     {0, 0, 51, 51, 128, 255, 255, 255, 255, 255}},
    {"Handheld",
     {9029, 4274},
     {7176, -33954},
     {0, 36000, 40000, 43000, 44000, 45000, 46500, 80000, 90000, 100000},
     {0, 0, 3900, 2900, 0, 0, 1000, 0, 0, 0},
     {0, 0, 51, 51, 76, 128, 255, 255, 255, 255}},
};

int skin(const int32_t coefficient[2], int milliC)
{
    return static_cast<int>(
        (static_cast<int64_t>(milliC) * coefficient[0] + static_cast<int64_t>(coefficient[1]) * 1000) / 10000 + 500);
}

// The sensor temperature (m C) whose skin estimate is `estimate`: skin() solved for T.
int sensorFor(const int32_t coefficient[2], int estimate)
{
    return static_cast<int>(
        ((static_cast<int64_t>(estimate) - 500) * 10000 - static_cast<int64_t>(coefficient[1]) * 1000) /
        coefficient[0]);
}

// ALERT window in whole degrees around one sensor: high when its estimate reaches `up`, low when it
// falls to `down`, and never so tight that the current reading already violates it (which would
// keep the level-triggered line asserted). The TMP451 alerts above the high and at/below the low.
void window(int milliC, int up, int down, uint8_t *high, uint8_t *low)
{
    int h = (up + 999) / 1000;
    int l = down / 1000;
    if (h < milliC / 1000 + 1)
        h = milliC / 1000 + 1;
    if (l > (milliC + 999) / 1000 - 1)
        l = (milliC + 999) / 1000 - 1;
    *high = static_cast<uint8_t>(h > 127 ? 127 : h);
    *low = static_cast<uint8_t>(l < 0 ? 0 : l);
}

int readByte(uint8_t reg, uint8_t *value)
{
    return tegra_i2c_read(kBus, kSensor, reg, value, 1);
}

int writeByte(uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kBus, kSensor, reg, &value, 1);
}

int milliC(uint8_t whole, uint8_t fraction)
{
    return whole * 1000 + (fraction >> 4) * 625 / 10;
}

// Linux DIV_ROUND_CLOSEST for a positive divisor.
int64_t divRoundClosest(int64_t x, int64_t d)
{
    return x >= 0 ? (x + d / 2) / d : (x - d / 2) / d;
}

// L4T's DFLL CVB voltage for `rung` at CPU speedo `speedo` (cvb.c tegra_get_cvb_voltage, speedo scale 100,
// in uV), plus `marginPct`, rounded up to the rail's 5 mV step as tegra_round_cvb_voltage does: mV.
unsigned cvbMilliVolts(const Rung &rung, int speedo, unsigned marginPct)
{
    int64_t uv = divRoundClosest(static_cast<int64_t>(rung.c2) * speedo, 100);
    uv = divRoundClosest((uv + rung.c1) * speedo, 100) + rung.c0;
    uv = uv * (100 + marginPct) / 100;
    return uv > 0 ? static_cast<unsigned>((uv + 4999) / 5000 * 5) : 0;
}

unsigned rungKHz(const Rung &rung)
{
    return 38400u * rung.n / kPllxM / (rung.pdivCode + 1);
}

// GLB_CFG2 bits 2:0, the ramp-up slew (max77812-regulator.c slew_rate_table), in uV/us.
unsigned rampUpUvPerUs(uint8_t glbCfg2)
{
    static const uint16_t kSlew[8] = {1250, 2500, 5000, 10000, 20000, 40000, 60000, 60000};
    return kSlew[glbCfg2 & 7];
}

}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchThermal : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchThermal) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOInterruptEventSource *alert = nullptr;
    volatile uint8_t *car = nullptr, *gpio = nullptr, *pinmux = nullptr, *pwm = nullptr, *pmc = nullptr;
    volatile uint8_t *fuse = nullptr, *apbMisc = nullptr;
    const Profile *profile = &kProfiles[0];
    unsigned level = 0, drive = ~0u, alertCount = 0;
    uint64_t alertWindowStart = 0;
    bool alertArmed = false, sensorReady = false, announced = false, reportedFailure = false;
    bool asleep = false;  // from suspend to resume: SOC_THERM's events start no evaluation
    bool reportedCritical = false;
    // CPU operating point: wanted, raised now (VDD_CPU above the boot value), or settled for this boot (sensor
    // lost, a raise refused or stopped by perfSteps: never raised again). A
    // thermal trip only holds it down until perfHoldUntil.
    bool perfWanted = false, perfRaised = false, perfSettled = false, perfKept = false, perfTrace = false;
    bool perfRelocked = false, perfLimited = false;  // steps 1 and 2 are done once per boot (and SC7)
    bool pllxLost = false;                           // SC7 left PLLX as the warm boot set it
    bool dfllMode = false, dfllPrepared = false;     // the CPU goes on the DFLL, and it is still usable this boot
    bool governed = false;                           // on the DFLL, the clock follows the load (switchperf)
    unsigned long dfllTopHz = kDfllRateHz;           // the top this boot measured at or under 1785 MHz
    IOInterruptEventSource *governor = nullptr;      // switchperf's signal: a software interrupt of this work loop
    unsigned dfllFloor = TEGRA_DFLL_FLOOR_COLD, dfllCap = TEGRA_DFLL_CAP_1010MV;  // the driver's boot defaults
    unsigned perfRaises = 0, perfSteps = ~0u;
    unsigned perfTop = 0, perfRung = 0;  // rungs this chip may use; the rung running now (0: boot clock)
    int cpuSpeedo = 0, cpuSpeedoId = 0, cpuProcess = 0;
    uint64_t perfHoldUntil = 0;         // mach_absolute_time() before which no raise happens
    uint8_t cpuRail = 0, keptVout = 0;  // MAX77812 address; VDD_CPU code as the boot chain left it
    uint8_t m4Cfg = 0, m4CfgBoot = 0, glbCfg2 = 0, pmicVersion = 0;  // M4_CFG now, and as first read this boot
    bool m4CfgKept = false;
    char clockState[256] = "";              // CCLKG, divider, PLLP and its CPU branch as found, for the log
    uint32_t keptPolicy = 0, keptPllx = 0;  // CCLKG_BURST_POLICY and PLLX_BASE as the boot chain left them
    uint32_t keptOutb = 0;                  // PLLP_OUTB with OUT4 running, the cluster's park clock

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static void socThermMoved(void *ctx, enum switch_soctherm_group group, int millic);
    static void thermalShutdown(enum switch_thermal_sensor sensor, int millic);
    static void alertFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    bool mapRegisters();
    void gpioMasked(uint32_t port, uint32_t reg, unsigned pin, bool value);
    void fanPowerOn();
    void setDrive(unsigned value);
    bool initSensor();
    bool readTemperatures(int *soc, int *pcb);
    bool armAlert(int soc, int pcb);
    void failSafe(const char *what, int err);
    void publish(const char *key, int64_t value);
    bool evaluate();
    bool raiseCpu(int soc);
    bool preparePerf();
    bool setCurrentLimit();
    bool raiseRail();
    bool raiseDfll(int soc);
    bool startGovernor(char *why, size_t len);
    static unsigned long settleDfllTop();
    static bool dfllOverCeiling(uint32_t *measuredKHz);
    static void governorLimits();
    static void governorSignal(void *ctx);
    static void governorFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    void trace(const char *format, ...) const __printflike(2, 3);
    void updateDfllThermal(int soc);

public:
    void gpuReady();
    void quiesceCpu();
    void suspend();
    void resume();

private:
    void restoreCpu(const char *why, bool forGood, bool keepDfll = false);

    bool perfPending() const
    {
        return perfWanted && !perfSettled && !perfRaised;
    }

    uint32_t holdRemainingMS() const;
    uint32_t nextTickMS(bool ok) const;
    bool runCpuFromPllx(uint32_t pllxBase, unsigned step);
    uint32_t rungBase(unsigned rung) const;

public:
    bool start(IOService *provider) override;
    void free() override;
};

OSDefineMetaClassAndStructors(SwitchThermal, IOService)

    static SwitchThermal *gThermal;
// The whole last good reading for thermalmonitord's sensors (switch_thermal_tmp451): written by the work loop only, as
// generation (odd while writing), values, generation.
static uint32_t gReadingGen;
static int gReadingSoc, gReadingPcb;
static uint64_t gReadingAt;  // 0: none

static void storeReading(bool good, int soc, int pcb)
{
    __atomic_store_n(&gReadingGen, gReadingGen + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&gReadingSoc, soc, __ATOMIC_RELAXED);
    __atomic_store_n(&gReadingPcb, pcb, __ATOMIC_RELAXED);
    const uint64_t before = __atomic_load_n(&gReadingAt, __ATOMIC_RELAXED);
    __atomic_store_n(&gReadingAt, good ? mach_absolute_time() : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gReadingGen, gReadingGen + 1, __ATOMIC_RELEASE);
    if ((before == 0u) != !good)
        switch_sensors_note();  // thermalmonitord's sensors gained or lost the TMP451
}

bool SwitchThermal::mapRegisters()
{
    car = switch_mmio_map(kCarPA, 0x1000);
    gpio = switch_mmio_map(kGpioPA, 0x1000);
    pinmux = switch_mmio_map(kPinmuxPA, 0x1000);
    pwm = switch_mmio_map(kPwmPA, 0x100);
    pmc = switch_mmio_map(kPmcPA, 0x100);
    return (car != nullptr) && (gpio != nullptr) && (pinmux != nullptr) && (pwm != nullptr) && (pmc != nullptr);
}

void SwitchThermal::gpioMasked(uint32_t port, uint32_t reg, unsigned pin, bool value)
{
    switch_mmio_write32(gpio, port + reg, (1u << (pin + 8)) | ((value ? 1u : 0u) << pin));
    (void)switch_mmio_read32(gpio, port + reg);
}

// hekate fan_set_duty() first-call setup plus regulator_5v_enable(), T210B01 branch.
void SwitchThermal::fanPowerOn()
{
    // The PWM block must be clocked and out of reset before its registers are touched. Left alone
    // if something already runs it (hekate's Nyx enables it and keeps it on after its fan stops).
    if (!(switch_mmio_read32(car, CLK_OUT_ENB_L) & kPwmClock) || (switch_mmio_read32(car, RST_DEVICES_L) & kPwmClock)) {
        switch_mmio_write32(car, RST_DEV_L_SET, kPwmClock);
        switch_mmio_write32(car, CLK_ENB_L_CLR, kPwmClock);
        switch_mmio_write32(car, CLK_SOURCE_PWM, kPwmSource);
        switch_mmio_write32(car, CLK_ENB_L_SET, kPwmClock);
        IODelay(2);
        switch_mmio_write32(car, RST_DEV_L_CLR, kPwmClock);
        (void)switch_mmio_read32(car, RST_DEVICES_L);
    }
    switch_mmio_write32(pwm, PWM_CSR_1, kPwmEnable | (256u << 16));  // stopped until the pin is switched over

    switch_mmio_write32(pinmux, PINMUX_SATA_LED_ACTIVE, 1);
    gpioMasked(kPortA, MSK_CNF, kPinPA5, true);
    gpioMasked(kPortA, MSK_OUT, kPinPA5, true);
    gpioMasked(kPortA, MSK_OE, kPinPA5, true);
    if (switch_mmio_read32(pmc, NO_IOPOWER) & kPmcGpio) {
        switch_mmio_write32(pmc, NO_IOPOWER, switch_mmio_read32(pmc, NO_IOPOWER) & ~kPmcGpio);
        (void)switch_mmio_read32(pmc, NO_IOPOWER);
    }
    if (switch_mmio_read32(pmc, PWR_DET_VAL) & kPmcGpio) {
        switch_mmio_write32(pmc, PWR_DET_VAL, switch_mmio_read32(pmc, PWR_DET_VAL) & ~kPmcGpio);
        (void)switch_mmio_read32(pmc, PWR_DET_VAL);
    }

    switch_mmio_write32(pinmux, PINMUX_LCD_GPIO2, 1);
    (void)switch_mmio_read32(pinmux, PINMUX_LCD_GPIO2);
    gpioMasked(kPortV, MSK_CNF, kPinPV4, false);
}

void SwitchThermal::setDrive(unsigned value)
{
    if (value > kFullDrive)
        value = kFullDrive;
    if (value == drive)
        return;
    drive = value;
    switch_mmio_write32(pwm, PWM_CSR_1, kPwmEnable | ((kFullDrive - value) << 16));
    (void)switch_mmio_read32(pwm, PWM_CSR_1);
}

void SwitchThermal::publish(const char *key, int64_t value)
{
    if (OSNumber *number = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        setProperty(key, number);
        number->release();
    }
}

void SwitchThermal::failSafe(const char *what, int err)
{
    setDrive(kFullDrive);
    publish("FanDrive", kFullDrive);
    sensorReady = false;
    storeReading(false, 0, 0);
    restoreCpu(what, true);  // no raised clocks without a working sensor, for the rest of the boot
    if (!reportedFailure)
        IOLog("SwitchThermal: %s (%d), fan at full speed\n", what, err);
    reportedFailure = true;
}

bool SwitchThermal::initSensor()
{
    const uint8_t setup[][2] = {
        {ConfigWrite, 0},  // running, ALERT unmasked, 0..127 C
        {RemoteOffsetHigh, 0xf3}, {RemoteOffsetLow, 0x80},         {RemoteThermLimit, 99},
        {LocalThermLimit, 120},   {RateWrite, kRateFourPerSecond}, {RemoteHighFraction, 0},
        {RemoteLowFraction, 0},
    };
    uint8_t id = 0;
    int err = tegra_i2c_init(kBus);
    if (!err)
        err = readByte(ManufacturerID, &id);
    if (!err && id != kTexasInstruments) {
        failSafe("TMP451 not found on I2C1 (manufacturer ID)", id);
        return false;
    }
    for (const auto &setting : setup) {
        if (!err)
            err = writeByte(setting[0], setting[1]);
    }
    if (err) {
        failSafe("TMP451 init failed", err);
        return false;
    }
    return true;
}

// Reading the status first also clears the latched ALERT bits.
bool SwitchThermal::readTemperatures(int *soc, int *pcb)
{
    uint8_t status = 0;
    uint8_t config = 0;
    uint8_t remote = 0;
    uint8_t remoteFraction = 0;
    uint8_t local = 0;
    uint8_t localFraction = 0;
    int err = readByte(Status, &status);
    if (!err)
        err = readByte(ConfigRead, &config);
    if (!err)
        err = readByte(RemoteTemp, &remote);
    if (!err)
        err = readByte(RemoteFraction, &remoteFraction);
    if (!err)
        err = readByte(LocalTemp, &local);
    if (!err)
        err = readByte(LocalFraction, &localFraction);
    if (err) {
        failSafe("TMP451 read failed", err);
        return false;
    }
    if (config & kConfigShutdown) {
        failSafe("TMP451 found in shutdown", config);
        return false;
    }
    if (status & kStatusOpen) {
        failSafe("TMP451 SoC diode open", status);
        return false;
    }
    *soc = milliC(remote, remoteFraction);
    *pcb = milliC(local, localFraction);
    return true;
}

// The window in which the fan level cannot change: up to the next trip, down to this level's trip
// minus hysteresis (each converted back to the sensor's own temperature), capped at the critical
// temperature for the SoC, and at the CPU's thermal trip while the CPU is raised. False (fan at full
// speed, sensor to be re-initialized) if the limits could not be written.
bool SwitchThermal::armAlert(int soc, int pcb)
{
    const int up = level + 1 < kLevels ? profile->trip[level + 1] : INT32_MAX / 2;
    const int down = level ? profile->trip[level] - profile->hysteresis[level] : -1000000;
    int socUp = up < INT32_MAX / 2 ? sensorFor(profile->soc, up) : 127000;
    const int socCap = soc < kCriticalMilliC ? kCriticalMilliC : SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC;
    if (socUp > socCap)
        socUp = socCap;
    int socDown = level ? sensorFor(profile->soc, down) : -1000;
    if (perfRaised && dfllMode) {
        // The next DFLL floor/cap boundary either way, so each index change comes by interrupt.
        for (const int boundary : kDfllBoundaries) {
            if (boundary > soc && socUp > boundary)
                socUp = boundary;
            if (boundary <= soc && socDown < boundary - kPerfHysteresisMilliC)
                socDown = boundary - kPerfHysteresisMilliC;
        }
    } else if (perfRaised && socUp > kPerfTripMilliC) {
        socUp = kPerfTripMilliC;
    }
    if (perfPending() && soc >= kPerfTripMilliC - kPerfHysteresisMilliC &&
        socDown < kPerfTripMilliC - kPerfHysteresisMilliC) {
        socDown = kPerfTripMilliC - kPerfHysteresisMilliC;  // ALERT once the CPU may go up again
    }
    const int pcbUp = up < INT32_MAX / 2 ? sensorFor(profile->pcb, up) : 127000;
    uint8_t socHigh = 0;
    uint8_t socLow = 0;
    uint8_t pcbHigh = 0;
    uint8_t pcbLow = 0;
    window(soc, socUp, socDown, &socHigh, &socLow);
    window(pcb, pcbUp, level ? sensorFor(profile->pcb, down) : -1000, &pcbHigh, &pcbLow);
    int err = writeByte(RemoteHighWrite, socHigh);
    if (!err)
        err = writeByte(RemoteLowWrite, socLow);
    if (!err)
        err = writeByte(LocalHighWrite, pcbHigh);
    if (!err)
        err = writeByte(LocalLowWrite, pcbLow);
    if (err)
        failSafe("TMP451 limit write failed", err);
    return err == 0;
}

// One evaluation of the fan curve; false if there was no good reading.
bool SwitchThermal::evaluate()
{
    if (!sensorReady) {
        // A sensor leaving shutdown needs one conversion first: read on the next tick.
        sensorReady = initSensor();
        return false;
    }
    int soc = 0;
    int pcb = 0;
    if (!readTemperatures(&soc, &pcb))
        return false;
    if (soc >= SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC)
        thermalShutdown(SWITCH_THERMAL_SOC_DIODE, soc);
    reportedFailure = false;
    storeReading(true, soc, pcb);

    const int socSkin = skin(profile->soc, soc);
    const int pcbSkin = skin(profile->pcb, pcb);
    const int estimate = socSkin > pcbSkin ? socSkin : pcbSkin;
    const unsigned before = level;
    while (level + 1 < kLevels && estimate >= profile->trip[level + 1])
        ++level;
    while (level > 0 && estimate < profile->trip[level] - profile->hysteresis[level])
        --level;
    unsigned target = profile->pwm[level];
    if (level != before) {
        IOLog("SwitchThermal: fan level %u -> %u (%u/256): skin %d mC, SoC %d mC, PCB %d mC\n", before, level, target,
              estimate, soc, pcb);
    }
    if (soc >= kCriticalMilliC) {
        target = kFullDrive;
        if (!reportedCritical)
            IOLog("SwitchThermal: SoC at %d mC, fan at full speed\n", soc);
        reportedCritical = true;
    } else {
        reportedCritical = false;
    }
    setDrive(target);
    if (perfRaised && dfllMode)
        updateDfllThermal(soc);
    if (perfRaised && !dfllMode && soc >= kPerfTripMilliC) {
        char why[48];
        snprintf(why, sizeof(why), "SoC %d.%d C reached the %d C trip", soc / 1000, soc % 1000 / 100,
                 kPerfTripMilliC / 1000);
        restoreCpu(why, false);
    }
    if (!armAlert(soc, pcb))
        return false;
    if (!alertArmed) {
        // First window programmed (or the sensor is back): from here on the line can be released.
        alertArmed = true;
        alert->enable();
    }
    if (perfPending() && soc < kPerfTripMilliC - kPerfHysteresisMilliC && !tegra_gm20b_bringup_busy()) {
        // Not while the GPU comes up on the same MAX77812 (VDD_GPU on, GPCPLL raise, GR init): r3o hung once at GPU M2
        // with the untraced raise running alongside. The bring-up's end evaluates again at once (gpuReady). (Until
        // 11-05 a 250 ms re-check here was overwritten by timerFired's 5 s staleness shot: r8a raised 4.5 s after the
        // GPU was up, its first 7.3 s of uptime at 998 MHz.)
        const uint32_t hold = holdRemainingMS();
        if (hold) {
            timer->setTimeoutMS(hold);  // one-shot: the next evaluation after the hold-off raises it
        } else if (raiseCpu(soc) && !armAlert(soc, pcb)) {
            return false;
        }
    }

    publish("SoCTemperature", soc);
    publish("PCBTemperature", pcb);
    publish("SkinTemperature", estimate);
    publish("FanDrive", drive);
    if (!announced) {
        announced = true;
        const switchclk::Cpu cpu = switchclk::readCpu(car);
        publish("CPUClockMHz", cpu.kHz / 1000);
        publish("CPUClockSource", cpu.source);
        IOLog("SwitchThermal: started (%s, SoC %d mC, PCB %d mC, fan %u/256, ALERT on GPIO %u, CPU %u MHz%s)\n",
              profile->name, soc, pcb, drive, static_cast<unsigned>(TEGRA_GPIO_TEMP_ALERT), cpu.kHz / 1000,
              perfPending() ? ", CPU raise pending" : "");
    }
    return true;
}

// Reads what the operating point depends on, once per boot (top of the file): the cluster on PLLX as hekate
// leaves it, L4T's CCLKG registers agreeing with the alias, a 38.4 MHz pll_ref, a T210B01 with its CPU speedo
// fused, PLLP_OUT4 running, VDD_CPU's MAX77812 (address, M4_VOUT, M4_CFG, slew, silicon version), and the
// rungs this chip's CVB allows. The clock registers involved go into clockState for the log. Writes nothing but the fuse clock enables (as TegraDFLL and TegraGM20B do). A refusal logs one line
// and settles the operating point for the boot.
bool SwitchThermal::preparePerf()
{
    using namespace switchclk;
    const Cpu cpu = readCpu(car);
    const uint32_t policy = switch_mmio_read32(car, CCLKG_BURST_POLICY);
    const uint32_t divider = switch_mmio_read32(car, SUPER_CCLKG_DIVIDER);
    const uint32_t pllp = switch_mmio_read32(car, PLLP_BASE);
    const uint32_t outb = switch_mmio_read32(car, PLLP_OUTB);
    const uint32_t enbY = switch_mmio_read32(car, CLK_OUT_ENB_Y);
    const char *refusal = nullptr;
    int err = 0;
    snprintf(clockState, sizeof(clockState),
             "CCLKG %#x (state %u, run source %u), SUPER_CCLKG_DIVIDER %#x, PLLP_BASE "
             "%#x, PLLP_OUTB %#x (OUT4 %s, %u kHz), CLK_OUT_ENB_Y %#x (PLLP_OUT_CPU %s), RST_DEVICES_Y bit 31 %u",
             policy, policy >> 28, (policy >> 4) & 0xf, divider, pllp, outb,
             (outb & kOut4Running) == kOut4Running ? "on" : "off", kPllpKHz * 2 / ((outb >> 24) + 2), enbY,
             enbY & kPllpOutCpu ? "on" : "off", (switch_mmio_read32(car, RST_DEVICES_Y) >> 31) & 1);
    trace("step 0: %s", clockState);
    if (!fuse)
        fuse = switch_mmio_map(kFusePA, 0x400);
    if (!apbMisc)
        apbMisc = switch_mmio_map(kApbMiscPA, 0x100);
    if (!fuse || !apbMisc) {
        refusal = "FUSE/APB_MISC mapping failed";
    } else {
        switch_mmio_write32(car, CLK_ENB_H_SET, kFuseClock);
        switch_mmio_write32(car, MISC_CLK_ENB, switch_mmio_read32(car, MISC_CLK_ENB) | kFuseVisible);
        (void)switch_mmio_read32(car, MISC_CLK_ENB);
        const uint32_t hidrev = switch_mmio_read32(apbMisc, APB_MISC_HIDREV);
        const uint32_t sku = switch_mmio_read32(fuse, FUSE_CACHE + FUSE_SKU_INFO) & 0xff;
        cpuSpeedo = static_cast<int>(switch_mmio_read32(fuse, FUSE_CACHE + FUSE_CPU_SPEEDO_0));
        cpuSpeedoId = sku == 0x87 ? 3 : 0;
        cpuProcess = cpuSpeedo < static_cast<int>(kB01ProcessSpeedo) ? 0 : 1;
        // An ordered refusal chain: each read runs only once every check before it has passed.
        // NOLINTBEGIN(bugprone-assignment-in-if-condition,bugprone-assignment-in-selection-statement)
        if (policy != cpu.policy || divider != cpu.divider) {
            refusal = "CCLKG_BURST_POLICY/SUPER_CCLKG_DIVIDER differ from the 0x20/0x24 alias";
        } else if ((policy >> 28) != 2) {
            refusal = "the cluster is not in the RUN state";
        } else if (cpu.source != kSourcePllx) {
            refusal = "the CPU does not run from PLLX";
        } else if ((cpu.pllx & (kPllEnable | kPllBypass | kPllLock)) != (kPllEnable | kPllLock)) {
            refusal = "PLLX is not locked";
        } else if ((divider & (1u << 31)) && ((divider >> 8) & 0xff) != (divider & 0xff)) {
            refusal = "the CCLK divider is in use";
        } else if (refKHz(car) != 38400) {
            refusal = "pll_ref is not 38.4 MHz";
        } else if ((outb & kOut4Running) != kOut4Running) {
            refusal = "PLLP_OUT4, the cluster's backup clock, is off";
        } else if (((hidrev >> 8) & 0xff) != 0x21 || ((hidrev >> 4) & 0xf) != 2) {
            refusal = "not a T210B01 (HIDREV)";
        } else if (cpuSpeedo <= 0 || cpuSpeedo > 4000) {
            refusal = "the CPU speedo is not fused";
        } else if ((err = readCpuRail(&cpuRail, &keptVout))) {
            refusal = "VDD_CPU (MAX77812) does not answer on I2C5";
        } else if (voutMilliVolts(keptVout) > kPerfMilliVolts) {
            refusal = "VDD_CPU is already above 1120 mV";
        } else if ((err = tegra_i2c_read(kPmicBus, cpuRail, kMax77812Version, &pmicVersion, 1)) ||
                   (err = tegra_i2c_read(kPmicBus, cpuRail, kMax77812M4Cfg, &m4Cfg, 1)) ||
                   (err = tegra_i2c_read(kPmicBus, cpuRail, kMax77812GlbCfg2, &glbCfg2, 1))) {
            refusal = "MAX77812 configuration read failed on I2C5";
        } else if ((pmicVersion & 7) == kVersionEs2) {
            refusal = "the MAX77812 is ES2 silicon, which L4T trims first";
        }
        // NOLINTEND(bugprone-assignment-in-if-condition,bugprone-assignment-in-selection-statement)
    }
    if (refusal) {
        perfSettled = true;
        IOLog("SwitchPerf: CPU raise not applied for this boot: %s (CCLK %#x, PLLX %#x, CPU speedo %d, I2C5 %d; %s)\n",
              refusal, cpu.policy, cpu.pllx, cpuSpeedo, err, clockState);
        return false;
    }
    if (!m4CfgKept) {
        // The limit the PMIC came up with, for the log: after SC7 M4_CFG still holds the one written here.
        m4CfgKept = true;
        m4CfgBoot = m4Cfg;
    }
    perfTop = 0;
    while (perfTop < kRungCount && cvbMilliVolts(kRungs[perfTop], cpuSpeedo, kPllMarginPct) <= kPerfMilliVolts)
        ++perfTop;
    keptPolicy = policy;
    keptPllx = cpu.pllx;
    keptOutb = outb;
    perfKept = true;
    return true;
}

// Step 2: M4's peak current limit to L4T's 7.2 A (max77812_config_init: M4_CFG bits 6:4), read back.
bool SwitchThermal::setCurrentLimit()
{
    using namespace switchclk;
    const uint8_t want = static_cast<uint8_t>((m4Cfg & ~static_cast<unsigned>(kPeakCurrentMask)) | kPeakCurrent7200mA);
    uint8_t check = 0;
    int err = 0;
    trace("step %u: MAX77812 0x%02x M4_CFG 0x%02x -> 0x%02x (7.2 A)", kStepCurrentLimit, cpuRail, m4Cfg, want);
    if (want != m4Cfg)
        err = tegra_i2c_write(kPmicBus, cpuRail, kMax77812M4Cfg, &want, 1);
    if (!err)
        err = tegra_i2c_read(kPmicBus, cpuRail, kMax77812M4Cfg, &check, 1);
    if (err || check != want) {
        IOLog("SwitchPerf: CPU raise stopped at step %u: MAX77812 M4_CFG write failed (I2C5 %d, reads 0x%02x)\n",
              kStepCurrentLimit, err, check);
        return false;
    }
    m4Cfg = check;
    perfLimited = true;
    return true;
}

// Step 3: VDD_CPU to kPerfMilliVolts at the clock running now, read back, then the slew settle.
bool SwitchThermal::raiseRail()
{
    using namespace switchclk;
    uint8_t check = 0;
    trace("step %u: VDD_CPU %u -> %u mV (M4_VOUT 0x%02x)", kStepRail, voutMilliVolts(keptVout), kPerfMilliVolts,
          kPerfVout);
    int err = tegra_i2c_write(kPmicBus, cpuRail, kM4Vout, &kPerfVout, 1);
    if (!err)
        err = tegra_i2c_read(kPmicBus, cpuRail, kM4Vout, &check, 1);
    if (err || check != kPerfVout) {
        (void)tegra_i2c_write(kPmicBus, cpuRail, kM4Vout, &keptVout, 1);
        IOLog("SwitchPerf: CPU raise stopped at step %u: VDD_CPU write failed (I2C5 %d, reads back %u mV)\n", kStepRail,
              err, voutMilliVolts(check));
        return false;
    }
    IOSleep(kRailSettleMS);
    perfRaised = true;
    return true;
}

uint32_t SwitchThermal::rungBase(unsigned rung) const
{
    const Rung &r = kRungs[rung];
    return (keptPllx & ~switchclk::kPllDividers) | (r.pdivCode << 20) | (r.n << 8) | kPllxM;
}

// Moves the CPU up its ladder (top of the file): steps 1 and 2 once, then VDD_CPU, then one rung after
// another, up to what this chip's CVB allows and perfSteps asks. Runs again after every temporary
// drop. A refusal or a failed step logs one line and settles it for the boot. Work loop only.
bool SwitchThermal::raiseCpu(int soc)
{
    using namespace switchclk;
    if (!perfKept && !preparePerf())
        return false;
    if (pllxLost) {
        const unsigned found = readCpu(car).kHz;
        pllxLost = false;
        // Every relock parks the cluster on PLLP_OUT4, which preparePerf found running at boot.
        if ((switch_mmio_read32(car, PLLP_OUTB) & kOut4Running) != kOut4Running) {
            switch_mmio_write32(car, PLLP_OUTB, keptOutb);
            (void)switch_mmio_read32(car, PLLP_OUTB);
            IODelay(2);
        }
        if ((switch_mmio_read32(car, PLLP_OUTB) & kOut4Running) != kOut4Running) {
            IOLog("SwitchPerf: after sleep, PLLP_OUT4 is off (PLLP_OUTB %#x); the CPU stays at %u kHz for this boot\n",
                  switch_mmio_read32(car, PLLP_OUTB), found);
            dfllMode = governed = false;
            perfSettled = true;
            return false;
        }
        // The DFLL enable starts from PLLX at the boot rate (the PLL ladder relocks in its own first step).
        if (dfllMode && !runCpuFromPllx(keptPllx, kStepRelock)) {
            IOLog("SwitchPerf: after sleep, PLLX did not relock (CPU at %u kHz, CCLKG %#x); PLL mode for this boot\n",
                  found, switch_mmio_read32(car, CCLKG_BURST_POLICY));
            dfllMode = governed = false;
        } else if (dfllMode) {
            perfRelocked = true;
            switch_power_journal("SwitchPerf: after sleep: CPU %u -> %u kHz on PLLX, then the DFLL", found / 1000,
                                 readCpu(car).kHz / 1000);
        }
    }
    if (dfllMode) {
        if (!perfLimited && perfSteps >= kStepCurrentLimit && !setCurrentLimit()) {
            perfSettled = true;
            return false;
        }
        if (raiseDfll(soc))
            return true;
    }
    const unsigned bootKHz = readCpu(car).kHz;
    const unsigned last = kStepFirstRung - 1 + perfTop;
    const unsigned stop = perfSteps < last ? perfSteps : last;
    const char *failed = nullptr;
    if (!perfTop) {
        perfSettled = true;
        IOLog("SwitchPerf: CPU raise not applied for this boot: CPU speedo %d needs %u mV at %u MHz (L4T CVB + %u %%), "
              "above %u mV\n",
              cpuSpeedo, cvbMilliVolts(kRungs[0], cpuSpeedo, kPllMarginPct), kRungs[0].rowMHz, kPllMarginPct,
              kPerfMilliVolts);
        return false;
    }
    for (unsigned step = perfRelocked ? kStepRail : kStepRelock; step <= stop && !failed; ++step) {
        if (step == kStepRelock) {
            trace("step %u: relock PLLX at the boot dividers (%#x) through PLLP_OUT4", step, keptPllx);
            if (runCpuFromPllx(keptPllx, step)) {
                perfRelocked = true;
            } else {
                failed = "the PLLP_OUT_CPU gate or the cluster mux did not read back; CPU left on PLLX";
            }
        } else if (step == kStepCurrentLimit) {
            if (!perfLimited && !setCurrentLimit())
                failed = "";
        } else if (step == kStepRail) {
            if (!raiseRail())
                failed = "";
        } else {
            const unsigned rung = step - kStepFirstRung;
            trace("step %u: rung %u MHz = %u kHz (PLLX M%u N%u P%u, L4T CVB + %u %% = %u mV)", step,
                  kRungs[rung].rowMHz, rungKHz(kRungs[rung]), kPllxM, kRungs[rung].n, kRungs[rung].pdivCode + 1,
                  kPllMarginPct, cvbMilliVolts(kRungs[rung], cpuSpeedo, kPllMarginPct));
            if (runCpuFromPllx(rungBase(rung), step)) {
                perfRung = rung + 1;
            } else {
                failed = "the PLLP_OUT_CPU gate or the cluster mux did not read back; CPU left on PLLX";
            }
        }
        if (failed && *failed) {
            IOLog("SwitchPerf: CPU raise stopped at step %u: %s (CCLKG %#x, CCLK %#x)\n", step, failed,
                  switch_mmio_read32(car, CCLKG_BURST_POLICY), switch_mmio_read32(car, CCLK_BURST_POLICY));
        }
    }
    if (failed) {
        restoreCpu("raise stopped", true);
        perfSettled = true;
        return false;
    }
    if (!perfRaised) {
        // perfSteps stopped it before VDD_CPU: nothing raised, nothing to come back to.
        perfSettled = true;
        // NOLINTBEGIN(clang-analyzer-security.ArrayBound): perfTop is 1 to kRungCount (0 returned above)
        IOLog("SwitchPerf: stopped after step %u of %u as perfSteps asks (CPU speedo %d id %d process %d; "
              "MAX77812 0x%02x v%u M4_CFG 0x%02x -> 0x%02x, ramp up %u uV/us; top %u MHz; %s)\n",
              stop, last, cpuSpeedo, cpuSpeedoId, cpuProcess, cpuRail, pmicVersion & 7, m4CfgBoot, m4Cfg,
              rampUpUvPerUs(glbCfg2), kRungs[perfTop - 1].rowMHz, clockState);
        // NOLINTEND(clang-analyzer-security.ArrayBound)
        return false;
    }
    ++perfRaises;
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): perfTop is 1 to kRungCount (0 returned above)
    const Rung &top = kRungs[perfTop - 1];
    const unsigned mhz = readCpu(car).kHz / 1000;
    publish("CPUClockMHz", mhz);
    publish("CPURaises", perfRaises);
    char next[48] = "";
    if (perfTop < kRungCount) {
        snprintf(next, sizeof(next), "; %u MHz would need %u mV", kRungs[perfTop].rowMHz,
                 cvbMilliVolts(kRungs[perfTop], cpuSpeedo, kPllMarginPct));
    }
    IOLog("SwitchPerf: CPU %u -> %u MHz at %u mV (step %u of %u, raise %u; CPU speedo %d id %d process %d, top %u MHz: "
          "L4T CVB %u mV + %u %%%s; MAX77812 0x%02x v%u M4_CFG 0x%02x -> 0x%02x, ramp up %u uV/us); SoC %d.%d C, "
          "down at %d C\n",
          bootKHz / 1000, mhz, kPerfMilliVolts, stop, last, perfRaises, cpuSpeedo, cpuSpeedoId, cpuProcess, top.rowMHz,
          cvbMilliVolts(top, cpuSpeedo, 0), kPllMarginPct, next, cpuRail, pmicVersion & 7, m4CfgBoot, m4Cfg,
          rampUpUvPerUs(glbCfg2), soc / 1000, soc % 1000 / 100, kPerfTripMilliC / 1000);
    return true;
}

// DFLL mode: L4T's floor/cap indices for this SoC temperature (1 C hysteresis on the way down), applied when they
// change. Work loop only.
void SwitchThermal::updateDfllThermal(int soc)
{
    unsigned floorIndex = soc >= kDfllFloorWarmMilliC ? TEGRA_DFLL_FLOOR_WARM : TEGRA_DFLL_FLOOR_COLD;
    if (dfllFloor == TEGRA_DFLL_FLOOR_WARM && soc >= kDfllFloorWarmMilliC - kPerfHysteresisMilliC) {
        floorIndex = TEGRA_DFLL_FLOOR_WARM;
    }
    unsigned capIndex = TEGRA_DFLL_CAP_NONE;
    if (soc >= kDfllCap2MilliC) {
        capIndex = TEGRA_DFLL_CAP_1010MV;
    } else if (soc >= kDfllCap1MilliC) {
        capIndex = TEGRA_DFLL_CAP_1060MV;
    }
    if (dfllCap == TEGRA_DFLL_CAP_1010MV && soc >= kDfllCap2MilliC - kPerfHysteresisMilliC)
        capIndex = TEGRA_DFLL_CAP_1010MV;
    if (dfllCap == TEGRA_DFLL_CAP_1060MV && capIndex == TEGRA_DFLL_CAP_NONE &&
        soc >= kDfllCap1MilliC - kPerfHysteresisMilliC) {
        capIndex = TEGRA_DFLL_CAP_1060MV;
    }
    if (floorIndex == dfllFloor && capIndex == dfllCap)
        return;
    const int err = tegra_dfll_cpu_set_thermal(floorIndex, capIndex);
    if (capIndex != dfllCap) {
        const char *cap = "1010 mV";
        if (capIndex == TEGRA_DFLL_CAP_NONE) {
            cap = "off";
        } else if (capIndex == TEGRA_DFLL_CAP_1060MV) {
            cap = "1060 mV";
        }
        IOLog("SwitchPerf: SoC %d.%d C: DFLL voltage cap %s (%d)\n", soc / 1000, soc % 1000 / 100, cap, err);
    }
    dfllFloor = floorIndex;
    dfllCap = capIndex;
    governorLimits();
}

// The DFLL's floor and cap now in force, as the governor's lowest and highest points. Work loop only.
void SwitchThermal::governorLimits()
{
    unsigned floorMv = 0;
    unsigned capMv = 0;
    if (switchperf::running() && !tegra_dfll_cpu_thermal_mv(&floorMv, &capMv))
        switchperf::limits(floorMv, capMv);
}

// DFLL mode raise: prepare once, set the thermal indices for this temperature, then enable at the 1785 MHz row.
// Any failure (one log line with the driver's reason) leaves the CPU on PLLX and the PLL mode for the boot.
bool SwitchThermal::raiseDfll(int soc)
{
    char why[256] = "";
    int err = dfllPrepared ? 0 : tegra_dfll_cpu_prepare(why, sizeof(why));
    if (!err) {
        dfllPrepared = true;
        dfllFloor = dfllCap = ~0u;
        updateDfllThermal(soc);
        err = tegra_dfll_cpu_enable(kDfllSafeHz, why, sizeof(why));
    }
    if (err) {
        dfllMode = false;
        governed = false;
        IOLog("SwitchPerf: DFLL not used (%d: %s); PLL mode for this boot\n", err, why);
        return false;
    }
    perfRaised = true;
    ++perfRaises;
    dfllTopHz = settleDfllTop();  // 1785 MHz only as measured at or under it (fixed and governed modes alike)
    if (governed && !startGovernor(why, sizeof(why))) {
        // One line, then today's fixed point: off the DFLL, and raiseCpu() goes on up the PLL ladder.
        IOLog("SwitchPerf: CPU governor not started (%s); PLL mode for this boot\n", why);
        governed = false;
        restoreCpu("no governor", false);
        return false;
    }
    char state[224];
    tegra_dfll_cpu_describe(dfllTopHz, state, sizeof(state));
    const unsigned mhz = switchclk::readCpu(car).kHz / 1000;
    publish("CPUClockMHz", mhz);
    publish("CPURaises", perfRaises);
    IOLog("SwitchPerf: CPU on the DFLL at %lu MHz%s (raise %u; SoC %d.%d C): %s\n", dfllTopHz / 1000000,
          governed ? ", on demand from here" : "", perfRaises, soc / 1000, soc % 1000 / 100, state);
    if (governed)
        IOLog("SwitchPerf: %s\n", why);  // the governor's table and tunables
    return true;
}

// The cluster's clock, measured where it runs: a chain of dependent adds (one per cycle on the A57) timed by the
// generic timer with interrupts masked, as XNU's kprof_clock_probe (switch_kprof.c) does, 4x longer (131072
// adds: about 73 us at 1785 MHz, 0.07 % timer resolution). kprof's probe read 995-997 MHz at 998.4, 1664-1668 at
// 1670.4 and 1782-1787 at 1785 on this console: within 0.3 % low, against DFLL steps 1.4 % apart. In kHz.
static uint32_t probeCpuKhz()
{
    constexpr uint64_t kLoops = 16384;
    constexpr uint64_t kAdds = kLoops * 8;
    uint64_t n = 0;
    // NOLINTNEXTLINE(misc-const-correctness): the probe's adds write it ("+r")
    uint64_t x = 0;
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    for (int pass = 0; pass < 2; pass++) {  // the first pass warms the branch predictor and the I-cache
        n = pass ? kLoops : 64;
        t0 = mach_absolute_time();
        __asm__ volatile("1:\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "subs %0, %0, #1\n"
                         "b.ne 1b\n"
                         : "+r"(n), "+r"(x)
                         :
                         : "cc");
        t1 = mach_absolute_time();
    }
    ml_set_interrupts_enabled(enabled);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    return ns ? static_cast<uint32_t>(kAdds * 1000000 / ns) : 0;
}

// The clock now against the governor's ceiling (the highest row under the DFLL's thermal cap and thermalmonitord's CLTM
// ceiling that is not held down): over it by half a DFLL step or more (one step over 1785 MHz is 1810.5), the ceiling is
// asked again from the row below; still over, and that row alone is held down (switchperf::hold): the ceiling is the
// next row down at once, and the row is tried again from below at a later 10 s line (the next one, then 2, 4 and 8
// lines apart while it keeps measuring high). Measured right with the clock on it, a row on trial is restored. One
// line each (a trial that measures high again: os_log only). Below the ceiling a step high cannot reach it: rows are
// four DFLL steps or more apart.
// `measuredKHz` gets the probe. Returns whether it moved the clock. Work loop, governed.
bool SwitchThermal::dfllOverCeiling(uint32_t *measuredKHz)
{
    const uint32_t khz = probeCpuKhz();
    const uint32_t step = static_cast<uint32_t>(kDfllStepHz / 1000);
    if (measuredKHz)
        *measuredKHz = khz;
    const unsigned hi = switchperf::ceiling();
    const uint32_t ceiling = switchperf::khzAt(hi);
    // NOLINTNEXTLINE(readability-suspicious-call-argument): khz is the measured clock, step the DFLL's
    if (!switch_gov_over_step(khz, ceiling, step)) {
        if (switch_perf_cpu_khz() == ceiling && switchperf::landed(hi)) {
            IOLog("SwitchPerf: DFLL measured %u MHz at its %u MHz ceiling, tried again from below: restored\n",
                  khz / 1000, ceiling / 1000);
        }
        return false;
    }
    const uint32_t below = hi ? switchperf::khzAt(hi - 1) : ceiling;
    int err = tegra_dfll_cpu_set_rate(static_cast<unsigned long>(below) * 1000);
    if (!err) {
        IOSleep(2);
        err = tegra_dfll_cpu_set_rate(static_cast<unsigned long>(ceiling) * 1000);
    }
    if (!err)
        IOSleep(kDfllSettleMS);
    const uint32_t again = probeCpuKhz();
    if (!err && !switch_gov_over_step(again, ceiling, step)) {
        switchperf::applied(hi);
        const bool restored = switchperf::landed(hi);
        IOLog("SwitchPerf: DFLL measured %u MHz at its %u MHz ceiling; re-asked from %u MHz: %u MHz%s\n", khz / 1000,
              ceiling / 1000, below / 1000, again / 1000, restored ? " (tried again from below: restored)" : "");
        return true;
    }
    bool retried = false;
    const unsigned wait = switchperf::hold(hi, &retried);
    const unsigned lower = switchperf::ceiling();
    const int moved = tegra_dfll_cpu_set_rate(static_cast<unsigned long>(switchperf::khzAt(lower)) * 1000);
    if (!moved) {
        IOSleep(kDfllSettleMS);
        switchperf::applied(lower);
    }
    if (retried) {  // a row on trial that measured high again: log only, at most one line per its (doubling) wait
        os_log(OS_LOG_DEFAULT,
               "SwitchPerf: DFLL measured %u MHz at its %u MHz ceiling on trial, %u MHz re-asked (%d): held down "
               "again, tried in ~%u0 s; %u MHz now (%d; measured %u MHz)",
               khz / 1000, ceiling / 1000, again / 1000, err, wait, switchperf::khzAt(lower) / 1000, moved,
               probeCpuKhz() / 1000);
    } else {
        IOLog("SwitchPerf: DFLL measured %u MHz at its %u MHz ceiling, %u MHz re-asked (%d): %u MHz held down, tried "
              "again from below in ~%u0 s; %u MHz now (%d; measured %u MHz)\n",
              khz / 1000, ceiling / 1000, again / 1000, err, ceiling / 1000, wait, switchperf::khzAt(lower) / 1000,
              moved, probeCpuKhz() / 1000);
    }
    return true;
}

// The top this boot can run without exceeding 1785 MHz, by measurement (11-05 r7z, kprof's probe: the rate asked right
// after the DFLL closed its loop ran one DFLL step high, 1807-1813 MHz for 1785 MHz asked, for the 80 s nothing asked
// again; every later request, from below, landed at 1782-1787. The DT's loop gains, cf 6 ci 0, have no integral term,
// so where the loop settles depends on how it got there). Enable has asked for kDfllSafeHz (1683 MHz: one step high
// is 1708.5, still under 1785) and waited kDfllSettleMS; this asks it again from kDfllBounceHz, measures it, and only
// if that request landed right asks for 1785 MHz, measures that, and steps back to 1683 if it is high. Fixed and
// governed DFLL modes both run their top from here. One log line. Work loop, on the DFLL.
unsigned long SwitchThermal::settleDfllTop()
{
    uint32_t safe = 0;
    uint32_t top = 0;
    int err = tegra_dfll_cpu_set_rate(kDfllBounceHz);
    if (!err)
        IOSleep(2);
    if (!err)
        err = tegra_dfll_cpu_set_rate(kDfllSafeHz);
    if (!err)
        IOSleep(kDfllSettleMS);
    safe = probeCpuKhz();
    unsigned long chosen = kDfllSafeHz;
    const char *why = "";
    if (err) {
        why = ", re-request refused";
    } else if (switch_gov_over_step(safe, kDfllSafeHz / 1000, kDfllStepHz / 1000)) {
        why = ", which measured high, so 1785 MHz was not asked";
    } else {
        err = tegra_dfll_cpu_set_rate(kDfllRateHz);
        if (err) {
            why = ", 1785 MHz refused";
        } else {
            IOSleep(kDfllSettleMS);
            top = probeCpuKhz();
            if (!switch_gov_over_step(top, kDfllRateHz / 1000, kDfllStepHz / 1000)) {
                chosen = kDfllRateHz;
            } else {
                why = ", 1785 MHz measured high";
                err = tegra_dfll_cpu_set_rate(kDfllSafeHz);
                if (!err)
                    IOSleep(kDfllSettleMS);
            }
        }
    }
    char at[40] = "";
    if (top)
        snprintf(at, sizeof(at), ", %u MHz at %lu MHz", top / 1000, kDfllRateHz / 1000000);
    IOLog("SwitchPerf: DFLL measured %u MHz at %lu MHz (re-asked from %lu MHz)%s%s; top %lu MHz (probe; %d)\n",
          safe / 1000, kDfllSafeHz / 1000000, kDfllBounceHz / 1000000, at, why, chosen / 1000000, err);
    return chosen;
}

// Governed DFLL mode: the governor's points from the DFLL's own OPP table (switch_gov_cpu_table: the CVB Vmin row up to
// 1785 MHz), the clock already at the top (enable's rate). Work loop only. `why` gets the reason on failure, the
// table on success.
bool SwitchThermal::startGovernor(char *why, size_t len)
{
    uint32_t rowKhz[32];
    uint32_t rowMv[32];
    uint32_t khz[SWITCH_GOV_MAX_OPPS];
    uint32_t mv[SWITCH_GOV_MAX_OPPS];
    const int rows = tegra_dfll_cpu_opps(kDfllRateHz, rowKhz, rowMv, 32);
    if (rows <= 0) {
        snprintf(why, len, "no DFLL operating points (%d)", rows);
        return false;
    }
    // NOLINTNEXTLINE(readability-suspicious-call-argument): rowKhz is the DFLL's rows, khz the table made from them
    const unsigned count = switch_gov_cpu_table(rowKhz, rowMv, static_cast<unsigned>(rows), kDfllRateHz / 1000,
                                                rowMv[0], khz, mv, SWITCH_GOV_MAX_OPPS);
    if (!count || khz[count - 1] != kDfllRateHz / 1000) {
        snprintf(why, len, "the DFLL table ends at %u kHz, not %lu", count ? khz[count - 1] : 0, kDfllRateHz / 1000);
        return false;
    }
    if (!governor) {
        governor = IOInterruptEventSource::interruptEventSource(this, &SwitchThermal::governorFired);
        if (!governor || loop->addEventSource(governor) != kIOReturnSuccess) {
            OSSafeReleaseNULL(governor);
            snprintf(why, len, "no work loop source");
            return false;
        }
        governor->enable();
    }
    unsigned top = count - 1;
    while (top && khz[top] > dfllTopHz / 1000)
        --top;          // the measured top (1785 MHz, or 1683 if 1785 measured high)
    uint32_t held = 0;  // rows above it: held down, so the boot floor never asks them before they are tried from below
    for (unsigned i = top + 1; i < count; ++i)
        held |= 1u << i;
    if (!switchperf::start(khz, mv, count, top, held, &SwitchThermal::governorSignal, this, why, len))
        return false;
    governorLimits();
    size_t n = static_cast<size_t>(snprintf(why, len, "CPU governor: %u points (L4T CVB, MHz@mV):", count));
    for (unsigned i = 0; i < count && n < len; ++i)
        n += static_cast<size_t>(snprintf(why + n, len - n, " %u@%u", khz[i] / 1000, mv[i]));
    if (n < len) {
        n += static_cast<size_t>(snprintf(why + n, len - n,
                                          "; %u MHz until System Boot Complete (at the latest %u s of uptime)",
                                          khz[top] / 1000, switchperf::kBootFallbackSeconds));
    }
    if (held && n < len)
        snprintf(why + n, len - n, ", %u MHz tried from below at the first 10 s line", khz[count - 1] / 1000);
    return true;
}

// From any context (switchperf's decisions: the scheduler's callout, a hint's interrupt or thread): wake the work loop.
void SwitchThermal::governorSignal(void *ctx)
{
    const auto *self = static_cast<SwitchThermal *>(ctx);
    if (self->governor)
        self->governor->interruptOccurred(nullptr, nullptr, 0);
}

// Work loop: the point switchperf decided, onto the DFLL. A refusal logs one line and falls back to today's fixed
// point: governor stopped, CPU back on PLLX, then the PLL ladder (1670 MHz on this chip) from the next evaluation, a
// millisecond later (the ALERT window is re-armed for the PLL mode's trip there).
void SwitchThermal::governorFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchThermal, owner);
    if (!self || !switchperf::running())
        return;
    char line[760];
    const unsigned index = switchperf::take(line, sizeof(line));
    const bool onDfll = self->perfRaised && self->dfllMode && self->governed;
    uint32_t measured = 0;
    if (line[0]) {
        // The 10 s line also carries the clock as measured, and checks it against the ceiling.
        const bool moved = onDfll && dfllOverCeiling(&measured);
#if DEVELOPMENT || DEBUG
        const size_t n = strlen(line);
        if (onDfll && n + 32 < sizeof(line))
            snprintf(line + n, sizeof(line) - n, "; probe %u MHz", measured / 1000);
        os_log(OS_LOG_DEFAULT, "%s", line);  // log only, never the boot console
        switch_power_stats_tick();
#endif
        // Then rows held down whose wait is over go back on trial as the ceiling (not a row this check just held: its
        // wait is a line at least): the governor gets there from below at its next window, and that move to the
        // ceiling is measured below (restored, or held down again).
        if (onDfll) {
            const uint32_t back = switchperf::retryHeld();
#if DEVELOPMENT || DEBUG
            for (unsigned i = 0; back >> i; ++i) {
                if (back >> i & 1u) {
                    os_log(OS_LOG_DEFAULT, "SwitchPerf: %u MHz back on trial as the ceiling",
                           switchperf::khzAt(i) / 1000);
                }
            }
#else
            (void)back;
#endif
        }
        if (moved)
            return;  // the check set the DFLL itself
    }
    if (!onDfll)
        return;
    const unsigned long hz = static_cast<unsigned long>(switchperf::khzAt(index)) * 1000;
    if (switch_perf_cpu_khz() * 1000UL == hz)
        return;
    const int err = hz ? tegra_dfll_cpu_set_rate(hz) : -22;  // -EINVAL
    if (!err) {
        switchperf::applied(index);
        if (index == switchperf::ceiling()) {
            IOSleep(kDfllCeilingCheckMS);  // a new request at the ceiling: measured once it has settled
            (void)dfllOverCeiling(&measured);
        }
        return;
    }
    IOLog("SwitchPerf: the DFLL refused %lu MHz (%d); CPU governor off, PLL mode for this boot\n", hz / 1000000, err);
    self->governed = false;
    self->restoreCpu("governor off", false);
    self->timer->setTimeoutMS(1);  // the next evaluation raises it the PLL way (no hold-off after a DFLL restore)
}

// drivers/TegraGM20B's bring-up thread, at the bring-up's end (done or failed): a CPU raise that waited for it is
// evaluated at once, on the work loop (after any evaluation in flight, so its staleness shot cannot overwrite this).
void SwitchThermal::gpuReady()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            const auto *self = static_cast<SwitchThermal *>(owner);
            if (self->perfPending())
                self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this);
}

// Hands the CPU back to PLLX (DFLL mode only) on this service's work loop, before SwitchPMIC's shut-down/restart
// traffic on I2C5. The PLL mode needs nothing: it has no second master on the bus.
void SwitchThermal::quiesceCpu()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchThermal *>(owner);
            if (self->perfRaised && self->dfllMode)
                self->restoreCpu("shutting down", true);
            return kIOReturnSuccess;
        },
        this);
}

// System sleep (SwitchSuspend's sleep and wake actions, thread context, I2C up). Before SC7: no more readings, the CPU
// back on the clocks and VDD_CPU the boot chain left, the fan and its 5 V regulator (PA5) off. After SC7 the PWM
// block, its clock and the GPIOs' state were lost: the fan comes back at the safe duty, the sensor is re-initialized
// from the timer, and the CPU raise starts over from the clocks the console resumed with (captured again).
void SwitchThermal::suspend()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchThermal *>(owner);
            __atomic_store_n(&self->asleep, true, __ATOMIC_RELEASE);
            self->timer->cancelTimeout();
            if (self->alertArmed) {
                self->alert->disable();
                self->alertArmed = false;
            }
            self->restoreCpu("system sleep", false, true);
            self->setDrive(0);
            self->gpioMasked(kPortA, MSK_OUT, kPinPA5, false);
            return kIOReturnSuccess;
        },
        this);
}

void SwitchThermal::resume()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchThermal *>(owner);
            self->fanPowerOn();
            self->setDrive(kSafeDrive);
            self->sensorReady = false;
            if (self->dfllMode) {
                char why[160] = "";
                const int err = tegra_dfll_cpu_resume(why, sizeof(why));
                if (err) {
                    IOLog("SwitchPerf: DFLL not restored after sleep (%d: %s); PLL mode for this boot\n", err, why);
                    self->dfllMode = self->governed = false;
                }
            }
            // The boot's clocks and VDD_CPU code stay the baseline (the MAX77812 keeps its registers through SC7);
            // PLLX itself is relocked before the next raise.
            self->pllxLost = self->perfKept;
            self->perfRelocked = self->perfLimited = false;
            self->perfRung = 0;
            self->perfHoldUntil = 0;
            __atomic_store_n(&self->asleep, false, __ATOMIC_RELEASE);
            self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this);
}

uint32_t SwitchThermal::holdRemainingMS() const
{
    const uint64_t now = mach_absolute_time();
    if (now >= perfHoldUntil)
        return 0;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(perfHoldUntil - now, &ns);
    return static_cast<uint32_t>(ns / 1000000) + 1;
}

// The timer's next shot, 0 for none: a sensor being (re)initialized, or the end of a CPU hold-off.
uint32_t SwitchThermal::nextTickMS(bool ok) const
{
    if (!ok)
        return sensorReady ? kConversionMS : kRetryMS;
    return perfPending() ? holdRemainingMS() : 0;
}

// Back to the inherited clocks: the frequency first, then VDD_CPU, with one log line. `forGood`
// (the sensor was lost while raised) keeps it there for the rest of the boot; otherwise (thermal
// trip) it goes up again below the hysteresis and after the hold-off. No-op unless raised: a sensor
// failure before the first raise only delays it (the fan runs at full speed meanwhile).
void SwitchThermal::restoreCpu(const char *why, bool forGood, bool keepDfll)
{
    using namespace switchclk;
    if (!perfRaised)
        return;
    perfRaised = false;
    if (forGood)
        perfSettled = true;
    switchperf::stop();  // no more decisions; the DFLL is about to go
    if (dfllMode) {
        char because[160] = "";
        const int err = tegra_dfll_cpu_disable_to_pllx(because, sizeof(because));
        dfllMode = keepDfll;  // otherwise the rest of the boot is the PLL mode
        publish("CPUClockMHz", switchclk::readCpu(car).kHz / 1000);
        IOLog("SwitchPerf: %s: CPU off the DFLL, back on %s%s%s\n", why, err ? "PLLP 408 MHz" : "the inherited PLLX",
              err ? ": " : "", because);
        return;
    }
    clock_interval_to_deadline(kPerfHoldMS, kMillisecondScale, &perfHoldUntil);
    if (perfRung && !runCpuFromPllx(keptPllx, 0)) {
        perfSettled = true;
        IOLog(
            "SwitchPerf: %s: the PLLP_OUT_CPU gate or the cluster mux did not read back; CPU left at %u MHz and %u mV "
            "for this boot\n",
            why, readCpu(car).kHz / 1000, kPerfMilliVolts);
        return;
    }
    perfRung = 0;
    const int err = tegra_i2c_write(kPmicBus, cpuRail, kM4Vout, &keptVout, 1);
    if (!err)
        IOSleep(kRailSettleMS);
    const unsigned mhz = readCpu(car).kHz / 1000;
    publish("CPUClockMHz", mhz);
    char again[64];
    if (forGood) {
        snprintf(again, sizeof(again), "for the rest of this boot");
    } else {
        snprintf(again, sizeof(again), "up again below %d C, not before %u s",
                 (kPerfTripMilliC - kPerfHysteresisMilliC) / 1000, kPerfHoldMS / 1000);
    }
    IOLog("SwitchPerf: %s: CPU back to %u MHz at %u mV%s, %s\n", why, mhz, voutMilliVolts(err ? kPerfVout : keptVout),
          err ? " (VDD_CPU write failed, it stays raised)" : "", again);
}

// Runs the cluster from PLLP_OUT4 (408 MHz) while PLLX is stopped, given `pllxBase`'s dividers and
// relocked, then from PLLX again with the inherited burst policy. This is how Linux reprograms an enabled
// PLLX whose M or P changes (clk-pll.c _program_pll: disable, dividers, enable, wait for lock), with the
// CPU on its pll_p_out4 backup meanwhile (tegra124-cpufreq.c:63-68; top of the file for the gate). The
// super clock mux switches without glitches. PLLX is never stopped unless the gate, the mux and its 0x20
// alias read back (false otherwise, with the mux and gate as they were). PLLX has no lock interrupt: the
// lock time (L4T pll_x_params lock_delay 300 us) is slept out, then checked once. `step` only labels the
// trace and the panic (0: the way down).
bool SwitchThermal::runCpuFromPllx(uint32_t pllxBase, unsigned step)
{
    using namespace switchclk;
    const uint32_t parked = (keptPolicy & ~0xf0u) | (kSourcePllpOut4 << 4);  // the RUN source only
    const uint32_t gateClose =
        switch_mmio_read32(car, CLK_OUT_ENB_Y) & kPllpOutCpu ? 0 : kPllpOutCpu;  // only if it was shut
    trace("step %u: PLLP_OUT_CPU gate on (CLK_OUT_ENB_Y %#x)", step, switch_mmio_read32(car, CLK_OUT_ENB_Y));
    // clk-periph-gate.c clk_periph_enable: set, 2 us, then out of reset if it is in reset (none on hardware so far).
    switch_mmio_write32(car, CLK_ENB_Y_SET, kPllpOutCpu);
    (void)switch_mmio_read32(car, CLK_OUT_ENB_Y);
    IODelay(2);
    if (switch_mmio_read32(car, RST_DEVICES_Y) & kPllpOutCpu) {
        IODelay(5);
        switch_mmio_write32(car, RST_DEV_Y_CLR, kPllpOutCpu);
        (void)switch_mmio_read32(car, RST_DEVICES_Y);
        IODelay(2);
    }
    if (!(switch_mmio_read32(car, CLK_OUT_ENB_Y) & kPllpOutCpu) ||
        (switch_mmio_read32(car, RST_DEVICES_Y) & kPllpOutCpu)) {
        if (gateClose)
            switch_mmio_write32(car, CLK_ENB_Y_CLR, gateClose);
        return false;
    }
    trace("step %u: cluster mux to PLLP_OUT4 (CCLKG %#x)", step, parked);
    switch_mmio_write32(car, CCLKG_BURST_POLICY, parked);
    IODelay(2);  // clk-super.c clk_super_set_parent: udelay(2) after the mux write
    if (switch_mmio_read32(car, CCLKG_BURST_POLICY) != parked || switch_mmio_read32(car, CCLK_BURST_POLICY) != parked) {
        switch_mmio_write32(car, CCLKG_BURST_POLICY, keptPolicy);
        (void)switch_mmio_read32(car, CCLKG_BURST_POLICY);
        IODelay(2);
        if (gateClose)
            switch_mmio_write32(car, CLK_ENB_Y_CLR, gateClose);
        return false;
    }
    const uint32_t stopped = pllxBase & ~(kPllEnable | kPllBypass | kPllLock);
    trace("step %u: PLLX off, dividers %#x", step, stopped);
    switch_mmio_write32(car, PLLX_BASE, switch_mmio_read32(car, PLLX_BASE) & ~kPllEnable);
    switch_mmio_write32(car, PLLX_BASE, stopped);
    switch_mmio_write32(car, PLLX_MISC, switch_mmio_read32(car, PLLX_MISC) | kPllxLockEnable);
    if (switch_mmio_read32(car, PLLX_MISC3) & kPllxIddq) {
        // clk-pll.c _clk_pll_enable: out of IDDQ, 5 us, before the enable (hekate already leaves it out).
        switch_mmio_write32(car, PLLX_MISC3, switch_mmio_read32(car, PLLX_MISC3) & ~kPllxIddq);
        (void)switch_mmio_read32(car, PLLX_MISC3);
        IODelay(5);
    }
    trace("step %u: PLLX on", step);
    switch_mmio_write32(car, PLLX_BASE, stopped | kPllEnable);
    (void)switch_mmio_read32(car, PLLX_BASE);
    IOSleep(1);
    const uint32_t locked = switch_mmio_read32(car, PLLX_BASE);
    if (!(locked & kPllLock)) {
        panic("SwitchPerf: PLLX did not lock at step %u (PLLX_BASE %#x, PLLX_MISC %#x); the CPU is on PLLP_OUT4 at 408 "
              "MHz",
              step, locked, switch_mmio_read32(car, PLLX_MISC));
    }
    trace("step %u: PLLX locked (%#x), cluster mux back to PLLX, then PLLP_OUT_CPU gate off", step, locked);
    switch_mmio_write32(car, CCLKG_BURST_POLICY, keptPolicy);
    (void)switch_mmio_read32(car, CCLKG_BURST_POLICY);
    IODelay(2);
    if (gateClose)  // the clock framework closes the old parent's chain after it
        switch_mmio_write32(car, CLK_ENB_Y_CLR, gateClose);
    return true;
}

// perfTrace: one line before a sub-step, then kTraceMS for the console to draw it.
void SwitchThermal::trace(const char *format, ...) const
{
    if (!perfTrace)
        return;
    char line[288];
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    IOLog("SwitchPerf trace: %s\n", line);
    IOSleep(kTraceMS);
}

// The staleness re-read (and the sensor's (re)initialization). A pending ALERT that never reached the
// CPU panics here rather than being picked up silently by the re-read.
void SwitchThermal::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchThermal, owner);
    if (!self)
        return;
    if (__atomic_load_n(&self->asleep, __ATOMIC_ACQUIRE))
        return;
    if (self->alertArmed)
        tegra_gpio_check_delivery(TEGRA_GPIO_TEMP_ALERT, "SwitchThermal TMP451 ALERT");
    const bool ok = self->evaluate();
    if (const uint32_t ms = self->nextTickMS(ok))
        sender->setTimeoutMS(ms);
}

// SOC_THERM's work loop: a die group moved a degree, so the curve is evaluated again on this service's loop.
void SwitchThermal::socThermMoved(void *ctx, enum switch_soctherm_group group, int millic)
{
    auto *self = static_cast<SwitchThermal *>(ctx);
    if (group == SWITCH_SOCTHERM_PLLX && millic >= SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC)
        thermalShutdown(SWITCH_THERMAL_PLLX, millic);
    if (!__atomic_load_n(&self->asleep, __ATOMIC_ACQUIRE))
        self->timer->setTimeoutMS(1);
}

namespace {
struct ThermalShutdown {
    enum switch_thermal_sensor sensor;
    int millic;
};

ThermalShutdown gShutdown;
uint32_t gShutdownStarted;

// Its own thread: the record in the RTC first (the next boot files it), then reboot(2)'s own path, as Shut Down takes:
// processes ended, file systems synced and unmounted, the drivers told, the MAX77620 off (SwitchPMIC.cpp).
void shutdownThread(void *, wait_result_t)
{
    const int err = switch_pmic_thermal_record(gShutdown.sensor, gShutdown.millic);
    if (err)
        IOLog("SwitchThermal: thermal shutdown record not written (I2C5 %d); the next boot will not know why\n", err);
    char message[] = "thermal shutdown";
    reboot_kernel(RB_HALT, message);
    IOLog("SwitchThermal: thermal shutdown: a shut down was already under way\n");
}
}  // namespace

// Once per boot, from any thread: past the software shutdown threshold, the console shuts down in order before
// SOC_THERM's hardware trip would reset it.
void SwitchThermal::thermalShutdown(enum switch_thermal_sensor sensor, int millic)
{
    if (__atomic_exchange_n(&gShutdownStarted, 1u, __ATOMIC_ACQ_REL))
        return;
    gShutdown = {sensor, millic};
    const int limit =
        sensor == SWITCH_THERMAL_PLLX ? SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC : SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC;
    os_log_error(OS_LOG_DEFAULT,
                 "SwitchThermal: thermal shutdown %s %d.%d C, past its %d.%d C limit (hardware trip PLLX %d.%d C): "
                 "shutting down",
                 switch_thermal_sensor_name(sensor), millic / 1000, millic % 1000 / 100, limit / 1000,
                 limit % 1000 / 100, SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC / 1000,
                 SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC % 1000 / 100);
    thread_t thread = nullptr;
    if (kernel_thread_start(shutdownThread, nullptr, &thread) != KERN_SUCCESS) {
        panic("SwitchThermal: thermal shutdown %s %d mC: no thread to shut down on", switch_thermal_sensor_name(sensor),
              millic);
    }
    thread_deallocate(thread);
}

// ALERT asserted (level low on PX4; the GPIO line stays masked while this runs). Re-evaluate and
// move the window, then give the sensor one conversion against the new limits and read the status
// again, so the line is released before it is unmasked (as nct1008_work_func does).
void SwitchThermal::alertFired(OSObject *owner, IOInterruptEventSource *sender, int count)
{
    auto *self = OSDynamicCast(SwitchThermal, owner);
    if (!self || !self->alertArmed)
        return;
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindowStart, &elapsedNS);
    if (elapsedNS > 1000000000ULL) {
        self->alertWindowStart = now;
        self->alertCount = 0;
    }
    if (++self->alertCount > kAlertBurst) {
        char state[192];
        tegra_gpio_describe(TEGRA_GPIO_TEMP_ALERT, state, sizeof(state));
        panic("SwitchThermal: TMP451 ALERT fired %u times in 1 s and is not released: %s", self->alertCount, state);
    }
    if (!self->evaluate()) {
        // No good reading, so the line cannot be released: the fan is at full speed, the sensor is
        // re-initialized from the timer, and the next good evaluation enables the source again.
        sender->disable();
        self->alertArmed = false;
        self->timer->setTimeoutMS(self->sensorReady ? kConversionMS : kRetryMS);
        return;
    }
    IOSleep(kConversionMS);
    uint8_t status = 0;
    (void)readByte(Status, &status);
}

bool SwitchThermal::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    if (!mapRegisters()) {
        IOLog("SwitchThermal: register mapping failed\n");
        return false;
    }
    fanPowerOn();
    setDrive(kSafeDrive);

    perfWanted = true;
    // Traced: the first raise waits until the console has caught up with the boot (a one-shot of the timer).
    if (perfTrace)
        clock_interval_to_deadline(kTraceDelayMS, kMillisecondScale, &perfHoldUntil);
    // The DFLL with the on-demand governor, or with switch-no-dfll the PLL ladder (see the top of the file).
    unsigned noDfll = 0;
    dfllMode = governed = PE_parse_boot_argn("switch-no-dfll", &noDfll, sizeof(noDfll)) == 0 || noDfll == 0u;
    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchThermal::timerFired) : nullptr;
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("SwitchThermal: work loop setup failed, fan left at %u/256\n", drive);
        return false;
    }
    // The ALERT interrupt is required. Its source stays disabled until evaluate() has programmed the
    // first window, so a line still asserted from whatever state the sensor was left in cannot storm.
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_TEMP_ALERT, TEGRA_GPIO_LEVEL_LOW);
    if (!nub)
        panic("SwitchThermal: no interrupt for the TMP451 ALERT (GPIO %u, level low)", TEGRA_GPIO_TEMP_ALERT);
    alert = IOInterruptEventSource::interruptEventSource(this, &SwitchThermal::alertFired, nub, 0);
    if (!alert || loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchThermal: cannot attach the TMP451 ALERT interrupt (GPIO %u)", TEGRA_GPIO_TEMP_ALERT);
    }
    setName("SwitchThermal");
    setProperty("FanProfile", profile->name);
    publish("FanDrive", drive);
    registerService();
    timer->setTimeoutMS(1);
    if (switch_soctherm_listen(&SwitchThermal::socThermMoved, this))
        IOLog("SwitchThermal: no SOC_THERM events; the fan curve follows the TMP451 ALERT alone\n");
    return true;
}

void SwitchThermal::free()
{
    switchperf::stop();
    if (governor) {
        governor->disable();
        if (loop)
            loop->removeEventSource(governor);
        governor->release();
    }
    if (alert) {
        alert->disable();
        if (loop)
            loop->removeEventSource(alert);
        alert->release();
    }
    if (timer) {
        timer->cancelTimeout();
        if (loop)
            loop->removeEventSource(timer);
        timer->release();
    }
    OSSafeReleaseNULL(loop);
    IOService::free();
}

extern "C" int switch_thermal_start(void)
{
    if (gThermal)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchThermal *candidate = OSTypeAlloc(SwitchThermal);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchThermal: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    gThermal = candidate;
    return 0;
}

// thermalmonitord's TMP451-backed sensors (SwitchSensors.cpp): the last good reading and its time. Any context.
extern "C" int switch_thermal_tmp451(int *soc_millic, int *pcb_millic, unsigned long long *at)
{
    for (unsigned tries = 0; tries < 4; ++tries) {
        const uint32_t g1 = __atomic_load_n(&gReadingGen, __ATOMIC_ACQUIRE);
        const int soc = __atomic_load_n(&gReadingSoc, __ATOMIC_RELAXED);
        const int pcb = __atomic_load_n(&gReadingPcb, __ATOMIC_RELAXED);
        const uint64_t when = __atomic_load_n(&gReadingAt, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if ((g1 & 1) || g1 != __atomic_load_n(&gReadingGen, __ATOMIC_RELAXED))
            continue;
        if (!when)
            return 0;
        *soc_millic = soc;
        *pcb_millic = pcb;
        *at = when;
        return 1;
    }
    return 0;  // the work loop kept writing: this read fails, the next one will not
}

extern "C" void switch_thermal_gpu_ready(void)
{
    if (gThermal)
        gThermal->gpuReady();
}

extern "C" void switch_thermal_cpu_quiesce(void)
{
    if (gThermal)
        gThermal->quiesceCpu();
}

extern "C" void switch_thermal_suspend(void)
{
    switch_emc_suspend();
    if (gThermal)
        gThermal->suspend();
}

extern "C" void switch_thermal_resume(void)
{
    if (gThermal)
        gThermal->resume();
    switch_emc_resume();
}
