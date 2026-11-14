// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine's power partition sequenced as L4T's T210 powergate code does it (OE4T linux-tegra-4.9 r32.7.4
// drivers/platform/tegra/powergate/powergate-t21x.c tegra21x_powergate / tegra21x_unpowergate, mc.c flush and
// flush_done), then the module reset and clock-gating prod registers of nvhost's finalize_poweron (nvhost_acm.c,
// t210/cg_regs.c). Mainline's tegra_powergate_power_up/down does neither the memory-client flush nor the second reset.
//   off: clock on; the memory client flushed and drained, and held in hot reset while the partition is off; module
//        reset; clock off; partition off
//   on:  partition on; clock on; module reset; a partition just ungated has the other clocks its TRM procedure starts
//        pulsed (NVJPG's: NVDEC's, 12.2.7.10 steps 3-4, on every revision); clamps removed (until the partition's REMOVE_CLAMPING_CMD bit clears, the
//        handshake every procedure of Tegra X1 TRM 12.2.7 polls: CLAMP_STATUS is a legacy register, 12.6.12); reset
//        released; the client's hot reset released; on T210 before B01 only, the MBIST workaround's LVL2 pulse; the
//        module reset again, then host1x's ownership state for the module torn down (TRM 14.9.14 MOD_TEARDOWN)
// The caller writes its prod registers (switch_engine_prod) and boots its falcon after on. Waits are bounded reads of
// hardware handshakes that raise no interrupt.
#pragma once
#include <stdint.h>
#include <IOKit/IOLib.h>
#include "../SwitchCAR/SwitchCAR.h"
#include "../SwitchCAR/SwitchPMC.h"
#include "../SwitchMMIO/SwitchMMIOKernel.h"

struct SwitchEnginePower {
    const char *name;           // the driver's log prefix
    unsigned partition, clock;  // PMC partition; CAR clock, also the module's reset
    uint32_t hotResetCtrl, hotResetStatus, hotResetBit;
    uint32_t lvl2Mask;           // LVL2_CLK_GATE_OVRE bits of the T210 MBIST workaround
    const unsigned *slcgClocks;  // the clocks that pulse needs running
    uint32_t slcgCount;
    void (*hotReset)(uint32_t ctrl, uint32_t bit, bool on);  // tegra_mc_hot_reset
    uint32_t (*mcRead)(uint32_t offset);                     // tegra_smmu_read
    const unsigned *ungateClocks;  // clocks besides its own the partition's ungating runs before its clamps go
    uint32_t ungateCount;
    void (*mbist)();        // the engine's own MBIST pulse in place of the LVL2 one (VIC's needs THI SLCG overridden)
    uint32_t host1xModule;  // its HOST1X_SYNC_MOD_TEARDOWN bit
    void (*moduleTeardown)(uint32_t module);  // tegra_host1x_module_teardown
};

struct SwitchEngineProd {
    uint32_t offset, value;
};

namespace switch_engine_power {
constexpr uint32_t kStepUs = 10;
constexpr uint32_t kLvl2ClockGateOverride = 0x554, kPmcClampStatus = 0x2c, kPmcRemoveClamping = 0x34;
constexpr uint64_t kApbMiscPA = 0x70000000;
constexpr uint32_t kHidrev = 0x804;

// APB_MISC_GP_HIDREV: chip 0x21 is T210, major revision 2 and up its B01 (powergate-t21x.c's tegra210b01), whose
// partitions need no MBIST workaround.
inline bool b01()
{
    static int known = -1;
    if (known < 0) {
        const volatile uint8_t *apb = switch_mmio_map(kApbMiscPA, 0x1000);
        const uint32_t hidrev = apb ? switch_mmio_read32(apb, kHidrev) : 0;
        known = ((hidrev >> 8) & 0xff) == 0x21 && ((hidrev >> 4) & 0xf) >= 2;
    }
    return known == 1;
}

// mc.c tegra_mc_flush: the client's requests stop and those in flight drain; its STATUS bit then holds set. STATUS also
// reads done for a client with no flush requested (TRM 18.11.1.114), so it counts only while the request bit reads set.
inline bool flush(const SwitchEnginePower &p)
{
    p.hotReset(p.hotResetCtrl, p.hotResetBit, true);
    uint32_t settled = 0;
    for (uint32_t i = 0; i < 1000 && settled < 5; i++) {
        if (!(p.mcRead(p.hotResetCtrl) & p.hotResetBit)) {
            IOLog("%s: MC hot reset request bit %#x of MC %#x reads clear while its flush was awaited: the client "
                  "was not flushed\n",
                  p.name, p.hotResetBit, p.hotResetCtrl);
            return false;
        }
        settled = (p.mcRead(p.hotResetStatus) & p.hotResetBit) ? settled + 1 : 0;
        if (settled < 5)
            IODelay(kStepUs);
    }
    return settled >= 5;
}

inline void unflush(const SwitchEnginePower &p)
{
    p.hotReset(p.hotResetCtrl, p.hotResetBit, false);
}

inline void moduleReset(const SwitchEnginePower &p)
{
    switch_car_reset_assert(p.clock);
    IODelay(2);
    switch_car_reset_deassert(p.clock);
    IODelay(kStepUs);
}

// The clocks run while the partition's modules are held in reset, then left as they were found: NVDEC part A sits in
// NVJPG's partition, and an ungated NVJPG partition gets NVDEC's clock edges before its clamps go (TRM 12.2.7.10).
inline void ungatePulse(const SwitchEnginePower &p)
{
    bool wasOn[4] = {};
    for (uint32_t i = 0; i < p.ungateCount && i < 4; i++) {
        wasOn[i] = switch_car_clock_enabled(p.ungateClocks[i]);
        if (!wasOn[i])
            switch_car_clock_enable(p.ungateClocks[i]);
    }
    IODelay(kStepUs);
    for (uint32_t i = 0; i < p.ungateCount && i < 4; i++)
        if (!wasOn[i])
            switch_car_clock_disable(p.ungateClocks[i]);
}

// Only after a real ungate: a partition already on has no clamp left to remove, and what the command does then is
// documented nowhere (TRM 12.2.7: software must not un-gate a unit already un-gated; L4T returns early for one).
inline bool unclamp(const SwitchEnginePower &p)
{
    switch_pmc_remove_clamping(p.partition);
    for (uint32_t i = 0; i < 100; i++) {
        if (!(switch_pmc_read(kPmcRemoveClamping) & (1u << p.partition)))
            return true;
        IODelay(1);
    }
    return false;
}

inline void mbistWorkaround(const SwitchEnginePower &p)
{
    if (b01())
        return;
    if (p.mbist) {
        p.mbist();
        return;
    }
    if (!p.lvl2Mask)
        return;
    bool wasOn[4] = {};
    for (uint32_t i = 0; i < p.slcgCount && i < 4; i++) {
        wasOn[i] = switch_car_clock_enabled(p.slcgClocks[i]);
        if (!wasOn[i])
            switch_car_clock_enable(p.slcgClocks[i]);
    }
    switch_car_rmw(kLvl2ClockGateOverride, 0, p.lvl2Mask);
    IODelay(1);
    switch_car_rmw(kLvl2ClockGateOverride, p.lvl2Mask, 0);
    IODelay(1);
    for (uint32_t i = 0; i < p.slcgCount && i < 4; i++)
        if (!wasOn[i])
            switch_car_clock_disable(p.slcgClocks[i]);
}
}  // namespace switch_engine_power

// The engine off with its partition kept on (another partition depends on it): flushed, in reset, clock off.
inline bool switch_engine_quiesce(const SwitchEnginePower &p)
{
    using namespace switch_engine_power;
    switch_car_clock_enable(p.clock);
    IODelay(kStepUs);
    if (!flush(p)) {
        unflush(p);
        return false;
    }
    IODelay(kStepUs);
    switch_car_reset_assert(p.clock);
    IODelay(kStepUs);
    switch_car_clock_disable(p.clock);
    IODelay(kStepUs);
    return true;
}

// false: the client did not drain (it is released again and the engine left as it was) or the partition did not
// power off.
inline bool switch_engine_power_down(const SwitchEnginePower &p)
{
    return switch_engine_quiesce(p) && switch_pmc_partition_off(p.partition);
}

// false: the partition did not power on, its clamps stayed (it is powered back off, so the next power-up ungates it
// again), or the clock or reset did not take.
inline bool switch_engine_power_up(const SwitchEnginePower &p, bool *wasOn)
{
    using namespace switch_engine_power;
    *wasOn = switch_pmc_partition_is_on(p.partition);
    if (!*wasOn) {
        if (!switch_pmc_partition_on(p.partition))
            return false;
        IODelay(kStepUs);
    }
    switch_car_clock_enable(p.clock);
    switch_car_reset_assert(p.clock);
    IODelay(kStepUs);
    if (!*wasOn) {
        ungatePulse(p);
        if (!unclamp(p)) {
            IOLog("%s: partition %u kept its clamps: REMOVE_CLAMPING_CMD %#x CLAMP_STATUS %#x after 100 us; powered "
                  "back off\n",
                  p.name, p.partition, switch_pmc_read(kPmcRemoveClamping), switch_pmc_read(kPmcClampStatus));
            switch_car_clock_disable(p.clock);
            IODelay(kStepUs);
            (void)switch_pmc_partition_off(p.partition);
            return false;
        }
        IODelay(kStepUs);
    }
    switch_car_reset_deassert(p.clock);
    IODelay(kStepUs);
    unflush(p);
    IODelay(kStepUs);
    mbistWorkaround(p);
    moduleReset(p);
    p.moduleTeardown(p.host1xModule);
    return switch_car_clock_enabled(p.clock) && !switch_car_reset_asserted(p.clock);
}

// t210/cg_regs.c: the module's clock-gating prod values, after its reset and before its falcon boots.
inline void switch_engine_prod(volatile uint8_t *engine, const SwitchEngineProd *prod, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        switch_mmio_write32(engine, prod[i].offset, prod[i].value);
}
