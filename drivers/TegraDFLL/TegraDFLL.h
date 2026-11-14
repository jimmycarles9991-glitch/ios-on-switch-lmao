// SPDX-License-Identifier: GPL-2.0
// Tegra210B01 (Switch OLED) CPU DFLL (CL-DVFS, closed-loop CPU DVFS) for statically linked Switch drivers.
//
// The DFLL is a CPU-voltage-tracking oscillator (DVCO) plus a control loop that drives VDD_CPU over its own I2C
// master: given a target frequency it raises or lowers VDD_CPU until the DVCO, which clocks the A57 cluster, runs at
// that frequency. Because the clock follows the real rail voltage, a voltage that is too low (a thermal cap, a slow
// PMIC) makes the CPU run slower, never out of spec. This is L4T's driver, vendored from CTCaer/switch-l4t-kernel-4.9
// linux-dev 2d0059fd (linux/: clk-dfll.c, clk-tegra124-dfll-fcpu.c with the tegra210b01 CVB/thermal tables, cvb.c,
// speedo-tegra210.c and their headers) over a freestanding Linux shim (shim/), so voltages, LUT and tuning are exactly
// L4T's. Its changes are marked SWITCH; one is the Vmin calibration: at its voltage floor the DVCO of this chip runs
// faster than L4T's DVCO minimum allowed for, so rates skipped from that minimum ran up to a step fast (boot 21).
// platform.c holds the board: the DT node
// (switchroot OLED DT), CAR clocks/reset, fuses, VDD_CPU = MAX77812 M4 on I2C5.
//
// Nothing here runs unless called. The caller (SwitchThermal's on-demand governor) owns the policy: it picks the rate, maps temperature to the L4T thermal indices below and
// decides when to leave DFLL mode. Every call is thread context only (I2C5 and IOSleep), serialized internally; none may
// run from an interrupt or with a spinlock held. Interrupts: none needed. L4T keeps the DFLL interrupt (GIC SPI 62,
// output min/max limits) masked (DFLL_INTR_EN = 0) and drives the loop from two timers, both XNU thread calls here: the
// low->high tuning step (clk-dfll.c dfll_tune_timer_cb: re-checks every 2 ms that the DFLL's I2C output reached the
// tuning threshold, then waits 500 us for the PMIC; there is no interrupt for "PMIC reached a voltage") and the Vmin
// calibration, 40 ms after each rate request.
//
// Coexistence with drivers/TegraI2C on PWR_I2C (I2C5 pads PY3/PY4): the pads have one pinmux function (I2CPMU, L4T
// pinctrl-tegra210.c:1605-1606) and two masters sit on them, the I2C5 controller (0x7000d000) and the DFLL's
// integrated controller (0x70110100). L4T does not lock between them in software, nor need it: CL-DVFS grants the
// pads to one of the two at a time, round robin, while it is clocked (Tegra X1 TRM 35.4); and:
//   * the DFLL sets DFLL_I2C_CFG ARB_ENABLE (clk-dfll.c dfll_init_i2c_if), and
//   * i2c-tegra.c puts every T210 controller, I2C5 included, in I2C_CNFG MULTI_MASTER_MODE (bit 17;
//     tegra210_i2c_hw.has_multi_master_mode, tegra_i2c_init), and on arbitration loss resets the controller, issues a
//     bus clear and returns -EAGAIN to the client (tegra_i2c_handle_xfer_error; adapter retries are 0);
//   * the CL-DVFS clock (CAR W27) stays on whenever I2C5 is used: L4T's init table enables dfll_soc/dfll_ref for good
//     (clk-tegra210.c:3746-3747; the I2C5 DT node also carries nvidia,require-cldvfs-clock, which no 4.9 code reads)
//     and hekate enables it "to avoid issues with I2C5 sharing" (hw_init.c:413-414);
//   * the DFLL's I2C master divides I2C5's module clock (DT clocks "i2c" = TEGRA210_CLK_I2C5), read once at probe,
//     and L4T only sets that clock at I2C5 probe; it must not change afterwards.
// TegraI2C sets MULTI_MASTER_MODE on I2C5 unless switch-no-dfll (tegra_dfll_cpu_enable() refuses without it;
// this driver only reads I2C_CNFG); a transfer that loses arbitration anyway is a fault there: one line, and
// TEGRA_I2C_EARB to its caller.
//
// While the DFLL owns the CPU (enable succeeded, disable not yet called) nobody else may write VDD_CPU (MAX77812
// M4_VOUT 0x26), CCLK_BURST_POLICY or PLLX: the DFLL writes VDD_CPU on its own, and disable returns to the PLLX the
// caller left running at the VDD_CPU code it found at enable. Shut down / restart: SwitchPMIC's PE_halt_restart talks
// to the MAX77620 over the same I2C5, where the DFLL is a second master until disabled; call
// tegra_dfll_cpu_disable_to_pllx() before that PMIC traffic (L4T's I2C5 node carries nvidia,bit-bang-after-shutdown
// for this situation). A panic path cannot do that: the DFLL keeps regulating VDD_CPU on its own through a panic.
//
// Return values: 0, or a negative Linux errno; `why` (may be NULL) receives one line naming the reason.
#ifndef TEGRA_DFLL_H
#define TEGRA_DFLL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// L4T thermal indices (= cooling-device states of DT tegra210b01-nx-common.dtsi:1024-1054,1157-1214 with
// dt-bindings/thermal/tegra210b01-trips.h; tables in clk-tegra124-dfll-fcpu.c tegra210b01_thermal_*_table). L4T
// switches them on the SoC (Tdiode) and PLL sensor trips with 1 C hysteresis; the caller does that mapping here.
// Until the caller sets them the driver keeps its own boot defaults: the cold floor and the tightest cap.
enum {
	TEGRA_DFLL_FLOOR_COLD = 0,  // below 20 C: VDD_CPU never below 800 mV
	TEGRA_DFLL_FLOOR_WARM =
		1,  // from 20 C: floor = the CVB Vmin (620 mV for the T210B01 tables)
	TEGRA_DFLL_FLOOR_STATES =
		3,  // (index 2, from 70 C in the table, equals index 1 on T210B01; no DT trip uses it)
	TEGRA_DFLL_CAP_NONE = 0,    // below 64 C
	TEGRA_DFLL_CAP_1060MV = 1,  // from 64 C (TEGRA210B01_DFLL_THERMAL_CAP_0)
	TEGRA_DFLL_CAP_1010MV = 2,  // from 84 C (TEGRA210B01_DFLL_THERMAL_CAP_1)
	TEGRA_DFLL_CAP_STATES = 3,
};

// The highest CPU rate this driver will run: Horizon's own boost-mode clock on Mariko, 1785 MHz (the CPU table HOS's
// PCV uses; also sys-clk's "1785 -> max clock, boost mode"). CTCaer's L4T tables go on to 2397 MHz (overclocks,
// clk-tegra124-dfll-fcpu.c tegra210b01_cpu_max_freq_table "Overclocked from ..."), with max_millivolts raised to
// 1235 mV for them; enable and set_rate refuse anything above this instead (-EINVAL).
#define TEGRA_DFLL_CPU_MAX_HZ 1785000000UL

// Reads the fuses (CPU speedo/process, L4T speedo-tegra210.c), builds the CVB OPP table and the DFLL LUT, finds
// VDD_CPU's MAX77812 and initializes the DFLL block in its disabled state with its I2C output off (L4T's probe).
// Never touches the CPU clock or VDD_CPU. Idempotent. -ENODEV is final for the boot: the chip is not a T210B01 A01,
// its CPU speedo is not fused, or no CVB row, table or DT node of the DFLL fits it. Any other failure (the MAX77812
// not answering on I2C5, I2C5 not clocked yet, the CPU clock or the DFLL in another state, an allocation) the next
// call tries again from where this one stopped.
int tegra_dfll_cpu_prepare(char *why, size_t len);

// Prepares if needed, then moves the A57 cluster from PLLX to the DFLL in closed loop, as L4T does
// (tegra210-cpufreq enable_cpu_clk + the clock framework): DFLL rate = the current PLLX rate, open loop, cclk_g mux
// to the DFLL, loop closed; then the DFLL is set to rate_hz. Requires the CPU on PLLX in the RUN state, undivided,
// PLLX locked, I2C5 multi-master. After a 10 ms settle it checks once that the loop is closed and that MAX77812
// M4_VOUT (read over TegraI2C) equals the LUT entry the DFLL last sent; on any failure after the switch it returns to
// PLLX (as tegra_dfll_cpu_disable_to_pllx) before returning the error. The DVCO target is rate_hz rounded down to a
// 25.5 MHz step (the 51 MHz reference / 2); below the DVCO's minimum (set by the thermal floor) the output skipper
// delivers the rate in 1/256 steps (clk-dfll.c dfll_calculate_rate_request). A rate above the table's top or above
// TEGRA_DFLL_CPU_MAX_HZ fails with -EINVAL. Already enabled: same as set_rate.
int tegra_dfll_cpu_enable(unsigned long rate_hz, char *why, size_t len);

// New closed-loop target (clk-dfll.c dfll_request_rate). -EPERM if not enabled, -EINVAL above TEGRA_DFLL_CPU_MAX_HZ.
// A few register writes under the driver's spin lock (the voltage follows in hardware, over the DFLL's own I2C
// master; crossing the tuning threshold arms L4T's 2 ms tune timer until the PMIC got there).
int tegra_dfll_cpu_set_rate(unsigned long rate_hz);

// Applies L4T thermal indices (tegra_dfll_update_thermal_index): floor 0..2, cap 0..2. Before prepare they are
// remembered and applied by it. -ERANGE for an index out of range.
int tegra_dfll_cpu_set_thermal(unsigned floor_index, unsigned cap_index);

// Back to the PLLX the caller left running, as L4T tegra124_cpu_switch_to_pllx does: cclk_g to PLLP (408 MHz; the
// DFLL drops to open loop first and is disabled after), VDD_CPU back to the code enable found (written and read back
// over TegraI2C, then 1 ms for the 5 mV/us slew), cclk_g to PLLX if PLLX is still locked. If VDD_CPU cannot be
// restored or PLLX is not locked the CPU stays on PLLP at 408 MHz and this fails. 0 if not enabled.
int tegra_dfll_cpu_disable_to_pllx(char *why, size_t len);

// After SC7 (the DFLL disabled before it): its CAR clocks and its own registers programmed again as after prepare, the
// thermal indices re-applied; enable may follow. 0 before a prepare; -EBUSY if it was still enabled.
int tegra_dfll_cpu_resume(char *why, size_t len);

// The OPP table prepare built (L4T cvb.c build_opp_table from this chip's CVB row: rates with their CVB voltages,
// rounded up to the 5 mV step and clamped to [the CVB Vmin, the table's max_millivolts]), ascending, rates up to
// max_hz (never above TEGRA_DFLL_CPU_MAX_HZ): kHz into khz[], mV into mv[], at most cap rows. Returns the row count,
// or a negative errno before a successful prepare.
int tegra_dfll_cpu_opps(unsigned long max_hz, unsigned int *khz, unsigned int *mv,
			unsigned int cap);

// The thermal floor and cap now in force, in mV (L4T tegra_dfll_get_thermal_floor_mv / _cap_mv: the LUT voltages the
// floor and cap indices select). -EPERM before a successful prepare.
int tegra_dfll_cpu_thermal_mv(unsigned int *floor_mv, unsigned int *cap_mv);

// For a periodic log line: the closed-loop target, the rate the DFLL's monitor measured (one read, as describe) and
// the VDD_CPU code its I2C master last sent, in mV; all 0 when the CPU is not on the DFLL. Takes the API lock.
void tegra_dfll_cpu_status(unsigned long *target_hz, unsigned long *measured_hz,
			   unsigned int *vdd_mv);

#if DEVELOPMENT || DEBUG
// HW Test: the loop's registers as they read now, all 0 when the CPU is not on the DFLL (Tegra X1 TRM 6.1.5
// DFLL_TUNE1, 6.1.6 DFLL_FREQ_REQ, 6.1.9 DFLL_OUTPUT_CFG, 6.1.12 DFLL_MONITOR_DATA, 6.1.15 DFLL_I2C_STS). Takes the API
// lock.
struct tegra_dfll_cpu_loop {
	uint32_t freq_req, tune1, output_cfg, monitor, i2c_sts;
};

void tegra_dfll_cpu_loop_registers(struct tegra_dfll_cpu_loop *loop);
#endif

// One line for the boot log: SKU, CPU speedo id/value and process id; after prepare also the CVB table chosen, its
// voltage range and top rate, the CVB voltage at rate_hz (0: the current DFLL target); when enabled the DFLL's mode,
// target, measured rate and the VDD_CPU it last sent. Does not prepare.
void tegra_dfll_cpu_describe(unsigned long rate_hz, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
