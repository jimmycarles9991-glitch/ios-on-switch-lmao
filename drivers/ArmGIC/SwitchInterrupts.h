// SPDX-License-Identifier: BSD-2-Clause-Patent
// Peripheral interrupts (GICv2 SPIs) for Switch drivers.
//
// Drivers here attach to IOResources and have no device-tree nub carrying
// "interrupts", so they ask for one by GIC interrupt ID and then use the
// ordinary IOKit event sources on their own workloop:
//
//   IOService *nub = switch_interrupt_nub(46, false);   // SDMMC1: SPI 14 = INTID 46, level
//   auto *source = IOInterruptEventSource::interruptEventSource(this, &Driver::interruptOccurred, nub, 0);
//   workLoop->addEventSource(source);
//   source->enable();
//
// IOFilterInterruptEventSource works the same way when the primary-context
// filter must clear a level source. INTID = 32 + SPI number (Linux/DT "GIC_SPI n").
// The controller routes each SPI to one CPU, keeps unclaimed SPIs disabled, and
// masks a level-triggered line while its event source runs (IOKit's usual model).
//
// There is no polling fallback anywhere: a driver whose interrupt cannot be had,
// or never arrives, panics and names it (switch_interrupt_describe below).
#pragma once
#include <stddef.h>
#include <stdint.h>

class IOService;
class IOTimerEventSource;

// True once ArmCpu has the GIC and CPU0's interface up and nubs can be had.
bool switch_interrupts_ready();

// Returns a retained-by-the-registry nub whose interrupt index 0 is `intid`.
// The same nub is returned for repeated calls with the same INTID. Sleeps until
// ArmCpu has the GIC up; panics if that takes over 10 s, if `intid` is not an SPI,
// or if it was asked for with the other trigger. Thread context only.
IOService *switch_interrupt_nub(uint32_t intid, bool edgeTriggered);

// How many times the INTID has reached its handler since boot (ArmCpu dispatched it to a registered vector): proof
// that an unproven driver's interrupt path works before it may panic on a lost one. Any context.
uint32_t switch_interrupt_count(uint32_t intid);

// One line of the INTID's controller state (times handled, distributor enable/
// pending/active, trigger, priority, target, Tegra LIC gate) for a panic message.
void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size);

// Arms `timer` for a lost-interrupt check (or other housekeeping no hardware event can clock) due in `ms` that may
// run up to `ms` later: the timer gets that much leeway, so XNU folds it into a wakeup the CPU takes anyway and an
// idle system takes none of its own for it. The check itself is unchanged; it only fires within [ms, 2 * ms].
int switch_check_after_ms(IOTimerEventSource *timer, uint32_t ms);
