// SPDX-License-Identifier: BSD-2-Clause-Patent
// SOC_THERM's group temperatures for the thermal consumers (SwitchSocTherm.h), moved by its LVL0 threshold interrupt
// (THERMCTL_INTR, SPI 48 = INTID 80, level high; L4T soctherm.c soctherm_thermal_isr). Each group's window sits one degree
// either side of the temperature last reported; when the group leaves it, the interrupt's work-loop action re-centres
// the window on the new reading, as L4T's tegra_thermctl_set_trips does (the group's interrupts off, thresholds written,
// back on), and hands the reading to every listener. Each arming also sets THERMTRIP and the PMC's reset on it, the last
// resort above SwitchThermal's own shutdown (SwitchThermalLimits.h), which SC7 loses with the rest of SOC_THERM.
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"
#include "SwitchSensors.h"
#include "SwitchSocTherm.h"
#include "SwitchSocThermWindow.h"

namespace {
constexpr uint32_t kThermIntid = 32 + 48;
constexpr unsigned kListeners = 4;

int groupMillic(volatile uint8_t *regs, unsigned group)
{
    const switch_soctherm_group_regs &g = switch_soctherm_groups[group];
    return switch_soctherm_millic((switch_mmio_read32(regs, g.temp) >> g.temp_shift) & 0xffffu);
}

// The group's interrupts off, its window around `millic`, then back on (tegra_thermctl_set_trips' order).
void centre(volatile uint8_t *regs, unsigned group, int millic)
{
    const switch_soctherm_group_regs &g = switch_soctherm_groups[group];
    switch_mmio_write32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_DISABLE, switch_soctherm_intr_bits(&g));
    switch_mmio_write32(regs, g.lvl0, switch_soctherm_lvl0_window(switch_mmio_read32(regs, g.lvl0), millic));
    switch_mmio_write32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_EN, switch_soctherm_intr_bits(&g));
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchSocTherm : public OSObject {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchSocTherm) IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *source = nullptr;
    volatile uint8_t *regs = nullptr;

    struct {
        switch_soctherm_moved moved;
        void *ctx;
    } listeners[kListeners] = {};

    unsigned listenerCount = 0;
    bool unexpectedLogged = false, thermtripLogged = false;

    static void fired(OSObject *owner, IOInterruptEventSource *, int);
    void armThermtrip(volatile uint8_t *mapped);
    static IOReturn armAction(OSObject *owner, void *mapped, void *, void *, void *);
    static IOReturn listenAction(OSObject *owner, void *moved, void *ctx, void *added, void *);
    void tell(unsigned group, int millic);

public:
    static SwitchSocTherm *create();
    void arm(volatile uint8_t *mapped);
    bool listen(switch_soctherm_moved moved, void *ctx);

    volatile uint8_t *armedRegs() const
    {
        return __atomic_load_n(&regs, __ATOMIC_ACQUIRE);
    }
};

OSDefineMetaClassAndStructors(SwitchSocTherm, OSObject)

    namespace
{
    SwitchSocTherm *gSocTherm;
}  // namespace

SwitchSocTherm *SwitchSocTherm::create()
{
    auto *self = OSTypeAlloc(SwitchSocTherm);
    if (!self || !self->OSObject::init())
        goto fail;
    self->loop = IOWorkLoop::workLoop();
    if (!self->loop)
        goto fail;
    self->source = IOInterruptEventSource::interruptEventSource(self, &SwitchSocTherm::fired,
                                                                switch_interrupt_nub(kThermIntid, false), 0);
    if (!self->source || self->loop->addEventSource(self->source) != kIOReturnSuccess)
        goto fail;
    self->source->enable();
    return self;
fail:
    IOLog("SwitchSocTherm: no work loop for the SOC_THERM interrupt; its temperature events are off\n");
    if (self) {
        if (self->source && self->loop)
            self->loop->removeEventSource(self->source);
        OSSafeReleaseNULL(self->source);
        OSSafeReleaseNULL(self->loop);
        self->release();
    }
    return nullptr;
}

void SwitchSocTherm::tell(unsigned group, int millic)
{
    for (unsigned i = 0; i < listenerCount; ++i)
        listeners[i].moved(listeners[i].ctx, static_cast<switch_soctherm_group>(group), millic);
}

void SwitchSocTherm::fired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchSocTherm *>(owner);
    volatile uint8_t *regs = self->regs;
    if (!regs)
        return;
    uint32_t status = switch_mmio_read32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_STATUS);
    for (unsigned group = 0; group < SWITCH_SOCTHERM_GROUPS; ++group) {
        const uint32_t bits = switch_soctherm_intr_bits(&switch_soctherm_groups[group]);
        if (!(status & bits))
            continue;
        status &= ~bits;
        switch_mmio_write32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_STATUS, bits);
        const int millic = groupMillic(regs, group);
        centre(regs, group, millic);
        self->tell(group, millic);
    }
    if (status) {
        switch_mmio_write32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_STATUS, status);
        if (!self->unexpectedLogged)
            IOLog("SwitchSocTherm: THERMCTL_INTR_STATUS bits %#x nobody armed, cleared\n", status);
        self->unexpectedLogged = true;
    }
}

void SwitchSocTherm::armThermtrip(volatile uint8_t *mapped)
{
    switch_mmio_write32(mapped, SWITCH_SOCTHERM_THERMTRIP_CTL,
                        switch_soctherm_thermtrip_ctl(switch_mmio_read32(mapped, SWITCH_SOCTHERM_THERMTRIP_CTL)));
    switch_pmc_write(SWITCH_PMC_SENSOR_CTRL,
                     switch_pmc_read(SWITCH_PMC_SENSOR_CTRL) | SWITCH_PMC_SENSOR_CTRL_ENABLE_RST);
    if (thermtripLogged)
        return;
    thermtripLogged = true;
    const uint32_t ctl = switch_mmio_read32(mapped, SWITCH_SOCTHERM_THERMTRIP_CTL);
    const uint32_t sensor = switch_pmc_read(SWITCH_PMC_SENSOR_CTRL);
    IOLog("SwitchSocTherm: hardware THERMTRIP backstop armed: PLLX %u.%u C, CPU %u.%u C, GPU %u.%u C, PMC reset "
          "(software shutdown first: SoC diode %d.%d C, PLLX %d.%d C; THERMTRIP_CTL %#x, PMC_SENSOR_CTRL %#x)\n",
          (ctl & 0x1ffu) / 2, (ctl & 1u) * 5, ((ctl >> 9) & 0x1ffu) / 2, ((ctl >> 9) & 1u) * 5,
          ((ctl >> 18) & 0x1ffu) / 2, ((ctl >> 18) & 1u) * 5, SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC / 1000,
          SWITCH_THERMAL_SHUTDOWN_DIODE_MILLIC % 1000 / 100, SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC / 1000,
          SWITCH_THERMAL_SHUTDOWN_PLLX_MILLIC % 1000 / 100, ctl, sensor);
}

IOReturn SwitchSocTherm::armAction(OSObject *owner, void *mapped, void *, void *, void *)
{
    auto *self = static_cast<SwitchSocTherm *>(owner);
    volatile uint8_t *regs = static_cast<volatile uint8_t *>(mapped);
    __atomic_store_n(&self->regs, regs, __ATOMIC_RELEASE);
    if (regs)
        self->armThermtrip(regs);
    for (unsigned group = 0; regs && group < SWITCH_SOCTHERM_GROUPS; ++group) {
        const int millic = groupMillic(regs, group);
        switch_mmio_write32(regs, SWITCH_SOCTHERM_THERMCTL_INTR_STATUS,
                            switch_soctherm_intr_bits(&switch_soctherm_groups[group]));
        centre(regs, group, millic);
        self->tell(group, millic);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchSocTherm::listenAction(OSObject *owner, void *moved, void *ctx, void *added, void *)
{
    auto *self = static_cast<SwitchSocTherm *>(owner);
    *static_cast<bool *>(added) = self->listenerCount < kListeners;
    if (self->listenerCount >= kListeners)
        return kIOReturnSuccess;
    const auto fn = reinterpret_cast<switch_soctherm_moved>(moved);
    self->listeners[self->listenerCount++] = {fn, ctx};
    for (unsigned group = 0; self->regs && group < SWITCH_SOCTHERM_GROUPS; ++group)
        fn(ctx, static_cast<switch_soctherm_group>(group), groupMillic(self->regs, group));
    return kIOReturnSuccess;
}

void SwitchSocTherm::arm(volatile uint8_t *mapped)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): runAction passes void *; armAction restores volatile
    loop->runAction(&SwitchSocTherm::armAction, this, const_cast<uint8_t *>(mapped));
}

bool SwitchSocTherm::listen(switch_soctherm_moved moved, void *ctx)
{
    bool added = false;
    loop->runAction(&SwitchSocTherm::listenAction, this, reinterpret_cast<void *>(moved), ctx, &added);
    return added;
}

namespace {
// SwitchThermal may listen before SwitchSensors arms SOC_THERM: whichever comes first makes it.
SwitchSocTherm *shared()
{
    SwitchSocTherm *have = __atomic_load_n(&gSocTherm, __ATOMIC_ACQUIRE);
    if (have)
        return have;
    SwitchSocTherm *fresh = SwitchSocTherm::create();
    SwitchSocTherm *none = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(&gSocTherm, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    fresh->release();
    return none;
}
}  // namespace

extern "C" void switch_soctherm_armed(volatile unsigned char *regs)
{
    if (SwitchSocTherm *socTherm = shared())
        socTherm->arm(regs);
}

extern "C" int switch_soctherm_read(enum switch_soctherm_group group, int *millic)
{
    const SwitchSocTherm *socTherm = __atomic_load_n(&gSocTherm, __ATOMIC_ACQUIRE);
    volatile uint8_t *regs = socTherm ? socTherm->armedRegs() : nullptr;
    if (!regs || group >= SWITCH_SOCTHERM_GROUPS)
        return -1;
    *millic = groupMillic(regs, group);
    return 0;
}

extern "C" int switch_soctherm_listen(switch_soctherm_moved moved, void *ctx)
{
    SwitchSocTherm *socTherm = shared();
    return socTherm && socTherm->listen(moved, ctx) ? 0 : -1;
}
