// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210B01 CPU and EMC clocks as the clock and reset controller (CAR) has them, and the CPU rail as the MAX77812
// has it. Header-only: the boot report (SwitchPMIC.cpp) and the CPU operating point (SwitchThermal.cpp) both read
// them, and neither object may depend on the other being linked.
//
// What the boot chain leaves (static reading of the sources; the boot report prints what is really there):
//   * hekate launch_l4t() (bootloader/l4t/l4t.c) -> ccplex_boot_cpu0() (bdk/soc/ccplex.c): VDD_CPU = MAX77812 M4 at
//     800 mV (_ccplex_enable_power_t210b01), PLLX = 38.4 MHz * 156 / 2 / 3 = 998.4 MHz (clock_enable_pllx),
//     CCLK_BURST_POLICY 0x20008888 (every state from PLLX_OUT0_LJ), SUPER_CCLK_DIVIDER enabled 1:1.
//   * EMC: hekate's sdram_init() leaves T210B01 at 204 MHz (sdram_config_t210b01.inl emc_clock_source 0x40188002 =
//     PLLP_OUT0, 7.1 divider 2). Minerva does not train on T210B01 (bdk/mem/minerva.c no_table: no MTC table, a
//     plain switch only, and minerva_deinit() returns to 204 MHz). launch_l4t() then starts the BPMP firmware
//     (bootloader/sys/l4t/bpmpfw_b01.bin + mtc_tbl_b01.bin, _l4t_bpmpfw_b01_config: CC_INIT_OP = OP_TRAIN, periodic
//     training interval CC_PT_TIME = 100), which trains the DRAM itself and owns every EMC rate change from then on
//     (L4T: the T210B01 EMC clock is BPMP clock 0, tegra210b01-soc-base.dtsi bpmp-emc / external-memory-controller).
//     The rate it leaves at handoff is not in any source we have.
//   * switchroot ATF (CTCaer/switch-l4t-atf, plat/nvidia/tegra/soc/t210) touches neither clock (it keeps the BPMP
//     running on T210B01: tegra_fc_bpmp_off() only with an sc7entry-fw, which hekate passes on T210 only), and
//     U-Boot (CTCaer/switch-l4t-uboot linux-hekatf) skips clock_early_init() (clock_early_init_done() is true) and is
//     built without CONFIG_TEGRA_PMU / CONFIG_TEGRA_CLOCK_SCALING (configs/nintendo-switch_defconfig).
//
// Registers (hekate bdk/soc/clock.h; Linux clk-tegra210.c, clk-tegra-super-gen4.c, tegra210-emc.c):
//   CCLK_BURST_POLICY: bits 31:28 the CPU state, one-hot (1 idle, 2 run, 4 IRQ, 8 FIQ), and a 4-bit source per state
//   (idle 3:0, run 7:4, IRQ 11:8, FIQ 15:12). CCLK_G sources: 0 CLK_M, 1 PLLC, 2 CLK_S, 3 PLLM, 4 PLLP_OUT0,
//   5 PLLP_OUT4, 8 PLLX_OUT0_LJ, 15 DFLL. SUPER_CCLK_DIVIDER: bit 31 enable, rate * (15:8 + 1) / (7:0 + 1).
//   PLLX/PLLM/PLLMB_BASE: 31 bypass, 30 enable, 27 lock, DIVP 24:20 (hardware code, pll_qlin_pdiv_to_hw), DIVN 15:8,
//   DIVM 7:0, all from pll_ref = OSC (38.4 MHz) / PLL_REF_DIV (OSC_CTRL 27:26). PLLX_MISC bit 18 lock enable.
//   CLK_SOURCE_EMC: source 31:29 (0 PLLM_OUT0, 1 PLLC_OUT0, 2 PLLP_OUT0, 3 CLK_M, 4 PLLM_UD, 5 PLLMB_UD, 6 PLLMB_OUT0,
//   7 PLLP_UD; tegra210-emc.c tegra_emc_src_names), 7.1 divider 7:0: EMC = parent * 2 / (divider + 2).
//   MAX77812 (hekate bdk/power/max77812.h, Linux max77812-regulator.c): M4 = VDD_CPU in both phase configurations,
//   M4_VOUT 0x26 = (mV - 250) / 5. Its I2C5 address is 0x33 in switchroot's tegra210b01-nx-pmic.dtsi (max77812@33,
//   m4vout "vdd-cpu") and 0x31 or 0x33 by fuse in hekate (_max77812_get_address(): FUSE_RESERVED_ODM28_B01 bit 0), so
//   both are probed, 0x33 first. (The T210 consoles' MAX77621 at 0x1B does not exist on T210B01.)
#ifndef SWITCH_CLOCKS_H
#define SWITCH_CLOCKS_H
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOLib.h>
#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

namespace switchclk {
constexpr uint64_t kCarPA = 0x60006000;

enum : uint32_t {
    CCLK_BURST_POLICY = 0x020,
    SUPER_CCLK_DIVIDER = 0x024,
    OSC_CTRL = 0x050,
    PLLC_BASE = 0x080,
    PLLM_BASE = 0x090,
    PLLX_BASE = 0x0e0,
    PLLX_MISC = 0x0e4,
    CLK_SOURCE_EMC = 0x19c,
    SPARE_REG0 = 0x55c,
    PLLMB_BASE = 0x5e8,
};

constexpr uint32_t kPllBypass = 1u << 31, kPllEnable = 1u << 30, kPllLock = 1u << 27, kPllxLockEnable = 1u << 18;
constexpr uint32_t kPllDividers = (0x1fu << 20) | (0xffu << 8) | 0xffu;
constexpr unsigned kSourcePllp = 4, kSourcePllx = 8, kSourceDfll = 15;
constexpr unsigned kPllpKHz = 408000;

constexpr unsigned kPmicBus = 5;
constexpr uint8_t kMax77812Addresses[] = {0x33, 0x31};
constexpr uint8_t kM4Vout = 0x26;

inline unsigned voutMilliVolts(uint8_t code)
{
    return 250u + 5u * code;
}

inline unsigned refKHz(volatile uint8_t *car)
{
    return 38400u >> ((switch_mmio_read32(car, OSC_CTRL) >> 26) & 3);
}

inline unsigned clkmKHz(volatile uint8_t *car)
{
    return 38400u / (((switch_mmio_read32(car, SPARE_REG0) >> 2) & 3) + 1);
}

// The post divider a DIVP code selects (Linux pll_qlin_pdiv_to_hw), 0 for an undefined code.
inline unsigned qlinDivider(uint32_t base)
{
    static const uint8_t kQlin[15] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32};
    const uint32_t code = (base >> 20) & 0x1f;
    return code < sizeof(kQlin) ? kQlin[code] : 0;
}

// PLL output in kHz (0 when off or unreadable). `divnShift` is 8 for PLLX/PLLM/PLLMB and 10 for PLLC.
inline unsigned pllKHz(uint32_t base, unsigned ref, unsigned divnShift, bool undivided)
{
    if (!(base & kPllEnable))
        return 0;
    if (base & kPllBypass)
        return ref;
    const uint32_t m = base & 0xff;
    const uint32_t n = (base >> divnShift) & 0xff;
    const uint32_t p = qlinDivider(base);
    if (!m || !p)
        return 0;
    return static_cast<unsigned>(static_cast<uint64_t>(ref) * n / m / (undivided ? 1 : p));
}

struct Cpu {
    uint32_t policy, divider, pllx;
    unsigned source, kHz;  // kHz 0: unknown (DFLL, or an unexpected source)
};

inline Cpu readCpu(volatile uint8_t *car)
{
    Cpu cpu{switch_mmio_read32(car, CCLK_BURST_POLICY), switch_mmio_read32(car, SUPER_CCLK_DIVIDER),
            switch_mmio_read32(car, PLLX_BASE), 0, 0};
    const uint32_t state = cpu.policy >> 28;
    unsigned index = 1;
    if (state == 1) {
        index = 0;
    } else if (state == 4) {
        index = 2;
    } else if (state == 8) {
        index = 3;
    }
    cpu.source = (cpu.policy >> (index * 4)) & 0xf;
    uint64_t kHz = 0;
    if (cpu.source == 0) {
        kHz = clkmKHz(car);
    } else if (cpu.source == kSourcePllp) {
        kHz = kPllpKHz;
    } else if (cpu.source == kSourcePllx) {
        kHz = pllKHz(cpu.pllx, refKHz(car), 8, false);
    }
    if (cpu.divider & (1u << 31))
        kHz = kHz * (((cpu.divider >> 8) & 0xff) + 1) / ((cpu.divider & 0xff) + 1);
    cpu.kHz = static_cast<unsigned>(kHz);
    return cpu;
}

inline const char *cpuSourceName(unsigned source)
{
    static const char *const kNames[16] = {"CLK_M", "PLLC", "CLK_S",        "PLLM", "PLLP_OUT0", "PLLP_OUT4",
                                           "?6",    "?7",   "PLLX_OUT0_LJ", "?9",   "?10",       "?11",
                                           "?12",   "?13",  "?14",          "DFLL"};
    return kNames[source & 15];
}

struct Emc {
    uint32_t clockSource;
    unsigned source, divider, kHz;
};

inline Emc readEmc(volatile uint8_t *car)
{
    Emc emc{switch_mmio_read32(car, CLK_SOURCE_EMC), 0, 0, 0};
    emc.source = emc.clockSource >> 29;
    emc.divider = emc.clockSource & 0xff;
    const unsigned ref = refKHz(car);
    unsigned parent = 0;
    switch (emc.source) {
    case 0:
        parent = pllKHz(switch_mmio_read32(car, PLLM_BASE), ref, 8, false);
        break;
    case 1:
        parent = pllKHz(switch_mmio_read32(car, PLLC_BASE), ref, 10, false);
        break;
    case 2:
    case 7:
        parent = kPllpKHz;
        break;
    case 3:
        parent = clkmKHz(car);
        break;
    case 4:
        parent = pllKHz(switch_mmio_read32(car, PLLM_BASE), ref, 8, true);
        break;
    case 5:
        parent = pllKHz(switch_mmio_read32(car, PLLMB_BASE), ref, 8, true);
        break;
    case 6:
        parent = pllKHz(switch_mmio_read32(car, PLLMB_BASE), ref, 8, false);
        break;
    default:  // the field is three bits, all named above
        break;
    }
    emc.kHz = static_cast<unsigned>(static_cast<uint64_t>(parent) * 2 / (emc.divider + 2));
    return emc;
}

inline const char *emcSourceName(unsigned source)
{
    static const char *const kNames[8] = {"PLLM_OUT0", "PLLC_OUT0", "PLLP_OUT0",  "CLK_M",
                                          "PLLM_UD",   "PLLMB_UD",  "PLLMB_OUT0", "PLLP_UD"};
    return kNames[source & 7];
}

// VDD_CPU's MAX77812 address and M4_VOUT code; TEGRA_I2C_* error when neither address answers. Thread context
// (I2C5 is interrupt-driven and may sleep until the GIC is up).
inline int readCpuRail(uint8_t *address, uint8_t *vout)
{
    int err = tegra_i2c_init(kPmicBus);
    for (unsigned i = 0; !err && i < sizeof(kMax77812Addresses); ++i) {
        err = tegra_i2c_read(kPmicBus, kMax77812Addresses[i], kM4Vout, vout, 1);
        if (err == TEGRA_I2C_EARB)
            err = tegra_i2c_read(kPmicBus, kMax77812Addresses[i], kM4Vout, vout, 1);
        if (!err) {
            // Only a MAX77812 with its CPU output (M4, EN_CTRL 0x06 bit 6, hekate max77812.h) on and a VDD_CPU code inside
            // hekate's CPU range (600..1525 mV) is VDD_CPU: anything else at that address is never written.
            uint8_t enable = 0;
            const int checked = tegra_i2c_read(kPmicBus, kMax77812Addresses[i], 0x06, &enable, 1);
            const unsigned mv = voutMilliVolts(*vout);
            if (checked || !(enable & (1u << 6)) || mv < 600 || mv > 1525)
                return TEGRA_I2C_ENACK;
            *address = kMax77812Addresses[i];
            return 0;
        }
        if (err == TEGRA_I2C_ENACK && i + 1 < sizeof(kMax77812Addresses))
            err = 0;
    }
    return err ? err : TEGRA_I2C_ENACK;
}

inline void formatMHz(char *buffer, size_t size, unsigned kHz)
{
    if (kHz) {
        snprintf(buffer, size, "%u.%u MHz", kHz / 1000, (kHz % 1000) / 100);
    } else {
        snprintf(buffer, size, "? MHz");
    }
}
}  // namespace switchclk
#endif
