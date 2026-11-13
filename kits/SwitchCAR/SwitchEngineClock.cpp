// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchEngineClock.h"
#include "SwitchCAR.h"
#include <IOKit/IOLib.h>
#include "../SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kFusePA = 0x7000F800, kApbMiscPA = 0x70000800;
constexpr uint32_t FUSE_SOC_SPEEDO_0 = 0x134, APB_MISC_HIDREV = 0x004;
constexpr uint32_t MISC_CLK_ENB = 0x048, CFG_ALL_VISIBLE = 1u << 28;
constexpr unsigned kFuseClock = 39;  // TEGRA210_CLK_FUSE: CAR bank H bit 7

struct Pll {
    uint32_t base, misc0, misc1, misc2, misc3;
    const char *name, *engines;
};

constexpr Pll kPllC2 = {0x4e8, 0x4ec, 0x4f0, 0x4f4, 0x4f8, "PLLC2", "NVJPG"};
constexpr Pll kPllC3 = {0x4fc, 0x500, 0x504, 0x508, 0x50c, "PLLC3", "NVDEC and NVENC"};

enum : uint32_t {
    PLL_BYPASS = 1u << 31,
    PLL_ENABLE = 1u << 30,
    PLLCX_LOCK = 1u << 26,
    PLLCX_MNP = 0x1f3fcff,
    PLLCX_MISC0_RESET = 1u << 30,
    PLLCX_MISC1_IDDQ = 1u << 27,
    PLLCX_MISC0_DEFAULT = 0x40080000,  // clk-tegra210.c PLLCX_MISC*_DEFAULT_VALUE
    PLLCX_MISC1_DEFAULT = 0x08000000,
    PLLCX_MISC2_DEFAULT = 0x1f720f05,
    PLLCX_MISC3_DEFAULT = 0x000000c4,
};

constexpr uint32_t kLockUs = 600;

IOLock *gLock;
const volatile uint8_t *gFuse, *gApbMisc;

enum Outcome {
    kRate,
    kPllp,
    kBusy,
    kNoLock,
    kOutcomes
};

bool gLogged[4][kOutcomes];

IOLock *lock()
{
    IOLock *held = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (held)
        return held;
    IOLock *made = IOLockAlloc();
    if (!__atomic_compare_exchange_n(&gLock, &held, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        IOLockFree(made);
        return held;
    }
    return made;
}

// FUSE_SOC_SPEEDO_0 on a T210B01 (APB_MISC HIDREV chip 0x21, major 2); -1 elsewhere: the table is T210B01's. The fuse
// cache is made readable each time as SwitchThermal and SwitchSensors make it (the fuse clock and CFG_ALL_VISIBLE,
// which AppleKeyStore's device-key read may put back off).
int32_t socSpeedo()
{
    if (!gFuse) {
        gApbMisc = switch_mmio_map(kApbMiscPA, 0x100);
        gFuse = switch_mmio_map(kFusePA, 0x400);
    }
    if (!gApbMisc || !gFuse)
        return -1;
    const uint32_t hidrev = switch_mmio_read32(gApbMisc, APB_MISC_HIDREV);
    if (((hidrev >> 8) & 0xff) != 0x21 || ((hidrev >> 4) & 0xf) != 2)
        return -1;
    switch_car_clock_enable(kFuseClock);
    switch_car_rmw(MISC_CLK_ENB, 0, CFG_ALL_VISIBLE);
    (void)switch_car_read(MISC_CLK_ENB);
    return static_cast<int32_t>(switch_mmio_read32(gFuse, FUSE_SOC_SPEEDO_0));
}

void flush(uint32_t offset, uint32_t value, uint32_t settleUs)
{
    switch_car_write(offset, value);
    (void)switch_car_read(offset);
    IODelay(settleUs);
}

bool bringUp(const Pll &pll, uint32_t base)
{
    flush(pll.misc0, PLLCX_MISC0_DEFAULT, 0);
    flush(pll.misc1, PLLCX_MISC1_DEFAULT, 0);
    flush(pll.misc2, PLLCX_MISC2_DEFAULT, 0);
    flush(pll.misc3, PLLCX_MISC3_DEFAULT, 1);
    flush(pll.base, base, 0);
    flush(pll.misc1, PLLCX_MISC1_DEFAULT & ~PLLCX_MISC1_IDDQ, 5);
    flush(pll.misc0, PLLCX_MISC0_DEFAULT & ~PLLCX_MISC0_RESET, 1);
    flush(pll.base, base | PLL_ENABLE, 1);
    if (switch_car_wait(pll.base, PLLCX_LOCK, PLLCX_LOCK, kLockUs)) {
        IODelay(2);
        return true;
    }
    flush(pll.base, base, 1);
    flush(pll.misc0, PLLCX_MISC0_DEFAULT, 1);
    flush(pll.misc1, PLLCX_MISC1_DEFAULT, 0);
    return false;
}

// One line per bus and outcome a boot: a PLL the engines could not use is an error line in every build, the rate they
// run at a DEVELOPMENT one.
void logOnce(unsigned bus, Outcome outcome, const Pll &pll, uint32_t found, int32_t speedo, const char *what)
{
    if (gLogged[bus][outcome])
        return;
    gLogged[bus][outcome] = true;
    IOLog("SwitchCAR: %s for %s: %s (BASE %#x as found, now %#x; SoC speedo %d, process %d)\n", pll.name, pll.engines,
          what, found, switch_car_read(pll.base), speedo, switch_engine_soc_process(speedo));
}
}  // namespace

extern "C" uint32_t switch_engine_clock_source(unsigned bus, uint32_t *khz)
{
    const Pll &pll = bus == SWITCH_ENGINE_BUS_C3 ? kPllC3 : kPllC2;
    bus = bus == SWITCH_ENGINE_BUS_C3 ? SWITCH_ENGINE_BUS_C3 : SWITCH_ENGINE_BUS_C2;
    IOLockLock(lock());
    const int32_t speedo = socSpeedo();
    const switch_engine_clock_plan plan = switch_engine_clock_plan_for(bus, speedo);
    const uint32_t found = switch_car_read(pll.base);
    const uint32_t ours = bus == SWITCH_ENGINE_BUS_C3 ? SWITCH_ENGINE_SOURCE_PLLC3 : SWITCH_ENGINE_SOURCE_PLLC2;
    uint32_t source = SWITCH_ENGINE_SOURCE_PLLP;
    uint32_t rate = SWITCH_ENGINE_PLLP_KHZ;
    if (!plan.pll_base) {
#if DEVELOPMENT || DEBUG
        logOnce(bus, kPllp, pll, found, speedo, "408000 kHz from PLLP, no faster rate at 637 mV");
#endif
    } else if (found & PLL_ENABLE) {
        if ((found & (PLLCX_MNP | PLL_BYPASS | PLLCX_LOCK)) == (plan.pll_base | PLLCX_LOCK)) {
            source = ours;
            rate = plan.khz;
        } else {
            logOnce(bus, kBusy, pll, found, speedo, "running with dividers that are not ours, 408000 kHz from PLLP");
        }
    } else if (bringUp(pll, plan.pll_base)) {
        source = ours;
        rate = plan.khz;
#if DEVELOPMENT || DEBUG
        char what[64];
        snprintf(what, sizeof(what), "%u kHz, L4T's 637 mV point", plan.khz);
        logOnce(bus, kRate, pll, found, speedo, what);
#endif
    } else {
        logOnce(bus, kNoLock, pll, found, speedo, "no lock in 600 us, put back off, 408000 kHz from PLLP");
    }
    IOLockUnlock(lock());
    if (khz)
        *khz = rate;
    return source;
}
