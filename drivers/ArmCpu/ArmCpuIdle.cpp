// SPDX-License-Identifier: BSD-2-Clause-Patent
// C7's platform half (kits/SwitchIdle/switch_idle.h): PSCI CPU_SUSPEND powers the core down, the core's own Tegra
// timer wakes it, and the banked GIC state the firmware resets on the way back is set again.
//
// The Cortex-A57 generic timer does not survive a core power-down and cannot wake a core from C7 (upstream Linux
// timer-tegra.c, "clocksource: tegra: add Tegra210 timer support"), so each core arms its own TMR10-13 (registers at
// 0x90 + 8 * cpu, INTID 208 + cpu; L4T tegra210_timer.c and the console's timer@60005000 node) for the deadline. A
// pending SPI targeted at the core wakes it through the flow controller even with its GIC CPU interface off (switchroot
// ATF tegra_fc_prepare_suspend: FLOWCTRL_HALT_GIC_IRQ | FLOWCTRL_HALT_LIC_IRQ). Each line is a registered ArmGIC
// vector steered to its core, so one that fires while the core runs is cleared by the handler instead of being taken
// as an unclaimed SPI and disabled for good.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <machine/machine_routines.h>
#include <arm64/switch_smp.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "ArmCpuIdle.h"

extern "C" vm_offset_t ml_static_vtop(vm_offset_t);
extern "C" volatile uint32_t armcpu_gic_targets[8];

namespace {
constexpr uint64_t kPsciVersion = 0x84000000, kPsciFeatures = 0x8400000a, kPsciCpuSuspend = 0xc4000001;
// switchroot ATF PSTATE_ID_CORE_POWERDN in the extended StateID format (StateType bit 30: power down).
constexpr uint64_t kPowerStateC7 = 0x40000007;
constexpr uint64_t kPsciFeatureExtendedStateId = 1U << 1;

constexpr uint64_t kTmrPA = 0x60005000;
constexpr uint32_t kTimerUsecConfig = 0x014;
// TIMERUS_USEC_CFG for a 19.2 MHz clk_m ((95 + 1) / (4 + 1), L4T tegra210_timer.c): TMR10-13 count clk_m, so their
// ticks are timebase ticks.
constexpr uint32_t kUsecConfig19M2 = 0x045f;
constexpr uint32_t kTmr10 = 0x090, kPtv = 0x0, kPcr = 0x4;
constexpr uint32_t kPtvEnable = 1U << 31, kPcrInterruptClear = 1U << 30;
// The count field is 29 bits in the n + 1 scheme.
constexpr uint64_t kTmrTicksMax = UINT64_C(1) << 29;
constexpr uint32_t kTmr10Intid = 32 + 176;

volatile uint8_t *gTmr = nullptr;
uint64_t gEntryPA = 0;
uint64_t gRecordPA[SWITCH_SMP_CORE_COUNT];

void tmrWrite(uint32_t cpu, uint32_t offset, uint32_t value)
{
    switch_mmio_write32(gTmr, kTmr10 + cpu * 8 + offset, value);
}

void disarm(uint32_t cpu)
{
    tmrWrite(cpu, kPtv, 0);
    tmrWrite(cpu, kPcr, kPcrInterruptClear);
}

int64_t powerDown(uint32_t cpu, uint64_t ticks)
{
    tmrWrite(cpu, kPtv, kPtvEnable | static_cast<uint32_t>(ticks - 1));
    __asm__ volatile("dsb sy" ::: "memory");
    const int64_t rc = armcpu::psciSmc(kPsciCpuSuspend, kPowerStateC7, gEntryPA, gRecordPA[cpu]);
    disarm(cpu);
    return rc;
}

void resumed(uint32_t cpu)
{
    disarm(cpu);
    armcpu::restoreBankedGic();
}

void timerInterrupt(OSObject *, void *refCon, IOService *, int)
{
    disarm(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(refCon)));
}

const switch_idle_platform kPlatform = {
    .power_down = powerDown,
    .resumed = resumed,
    .wake_ticks_max = kTmrTicksMax,
};

const char *check(void)
{
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!armcpu::nativeGicV2Smp())
        return "not every core runs on the native GICv2";
    if (!armcpu::psciSmcConduit())
        return "no PSCI SMC conduit in /psci";
    if ((static_cast<uint64_t>(armcpu::psciSmc(kPsciVersion, 0, 0, 0)) >> 16) < 1)
        return "PSCI older than 1.0";
    const int64_t features = armcpu::psciSmc(kPsciFeatures, kPsciCpuSuspend, 0, 0);
    if (features < 0)
        return "PSCI_FEATURES: no CPU_SUSPEND";
    if (!(static_cast<uint64_t>(features) & kPsciFeatureExtendedStateId))
        return "PSCI CPU_SUSPEND takes the original power-state format, not ATF's extended one";
    if (frequency != UINT64_C(19200000))
        return "CNTFRQ is not 19.2 MHz";
    gTmr = switch_mmio_map(kTmrPA, 0x400);
    if (!gTmr)
        return "TMR not mapped";
    if (switch_mmio_read32(gTmr, kTimerUsecConfig) != kUsecConfig19M2)
        return "the timers' clock is not the 19.2 MHz timebase (TIMERUS_USEC_CFG)";
    gEntryPA = ml_static_vtop(reinterpret_cast<vm_offset_t>(switch_secondary_entry));
    if (!gEntryPA || gEntryPA >= UINT64_C(0x100000000))
        return "the PSCI entry is not a usable physical address";
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; ++cpu) {
        gRecordPA[cpu] = ml_static_vtop(reinterpret_cast<vm_offset_t>(&switch_secondary_records[cpu]));
        if (!gRecordPA[cpu] || (gRecordPA[cpu] & 63))
            return "a PSCI entry record is not a usable physical address";
    }
    return nullptr;
}

// Each core's TMR line, on that core only. False if a line did not land on its core.
bool claimTimers(char *why, size_t size)
{
    const uintptr_t dist = armcpu::gicDistributor();
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; ++cpu) {
        disarm(cpu);
        const uint32_t intid = kTmr10Intid + cpu;
        IOService *nub = switch_interrupt_nub(intid, false);
        if (nub->registerInterrupt(0, nullptr, timerInterrupt, reinterpret_cast<void *>(uintptr_t{cpu})) !=
                kIOReturnSuccess ||
            nub->enableInterrupt(0) != kIOReturnSuccess) {
            snprintf(why, size, "INTID %u (CPU%u's TMR) not registered", intid, cpu);
            return false;
        }
        const uint8_t target = *reinterpret_cast<volatile uint8_t *>(dist + 0x800 + intid);
        if (target != armcpu_gic_targets[cpu]) {
            snprintf(why, size, "INTID %u (CPU%u's TMR) targets %#x, not the core's %#x", intid, cpu, target,
                     armcpu_gic_targets[cpu]);
            return false;
        }
    }
    return true;
}
}  // namespace

void armcpu_idle_start(void)
{
    char why[128];
    if (const char *reason = check()) {
        IOLog("ArmCpu: C7 off: %s\n", reason);
        return;
    }
    if (!claimTimers(why, sizeof(why))) {
        IOLog("ArmCpu: C7 off: %s\n", why);
        return;
    }
    armcpu::fillBootRecord();
    switch_idle_start(&kPlatform);
    IOLog("ArmCpu: C7 core power-down in idle on (PSCI CPU_SUSPEND %#llx, TMR10-13 wake)\n", kPowerStateC7);
}
