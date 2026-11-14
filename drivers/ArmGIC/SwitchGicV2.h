// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GICv2 sequences ArmCpu and ArmGIC run on every interrupt and every IPI, with their barriers in one place:
// ArmCpu's acknowledge loop (GICC_IAR, the handler, GICC_EOIR), an SGI sent through GICD_SGIR, and ArmGIC's run of an
// SPI's IOKit vector with the line masked or unmasked at the distributor. test/switch_gic_barrier_test.cpp runs these
// same templates against a model that counts the barriers.
#ifndef SWITCH_GIC_V2_H
#define SWITCH_GIC_V2_H

#include <stdint.h>

#ifdef SWITCH_GIC_HOST_MODEL
namespace switchgic {
void hostBarrier(const char *instruction);
}

#define SWITCH_GIC_BARRIER(instruction) ::switchgic::hostBarrier(instruction)
#else
#define SWITCH_GIC_BARRIER(instruction) __asm__ volatile(instruction::: "memory")
#endif

namespace switchgic {

constexpr uint32_t kIpiSgi = 0, kTimerPpi = 27, kSpurious = 1020;

// Every interrupt pending at this core's CPU interface, until GICC_IAR reads spurious. `cpu` provides acknowledge()
// (GICC_IAR), end(acknowledge) (GICC_EOIR, with the source CPU bits of an SGI), timer() for PPI 27, ipi(acknowledge)
// for SGI 0 and other(intid) for everything else (the SPIs). Each class pays for what it needs:
//   * SGI 0: a read barrier after the acknowledge. The sender stored its IPI state (XNU cpu_signal_internal's
//     cpu_signal bits and xcall arguments), then a store barrier, then GICD_SGIR; this pairs with that (Linux
//     gic_handle_irq's smp_rmb), so the handler reads the state the IPI announces. Nothing to order before the EOI.
//   * PPI 27: an ISB before the EOI. XNU's rtclock_intr re-arms the timer with an MSR to CNTV_TVAL_EL0 and no ISB
//     (ml_set_decrementer, both trees); the ISB puts that write, and the timer's level drop with it, in effect before
//     the EOI, so the GIC does not take the level PPI again. A DSB does not synchronize an MSR.
//   * SPIs: a DSB before the EOI, so the driver's status clear, or ArmGIC's mask of the line (runVector), reaches its
//     device before the EOI and a level line is not taken again.
//   * the spurious read that ends the loop: nothing is read on its account and nothing is ended.
// No barrier after the EOI (Linux has none either): the next GICC_IAR read goes to the same Device peripheral, so the EOI
// arrives before it, and the exception return synchronizes.
template <class Cpu> void acknowledgeLoop(Cpu &cpu)
{
    for (;;) {
        const uint32_t acknowledge = cpu.acknowledge();
        const uint32_t intid = acknowledge & 0x3ffU;
        if (intid >= kSpurious)
            return;
        if (intid == kTimerPpi) {
            cpu.timer();
            SWITCH_GIC_BARRIER("isb");
        } else if (intid == kIpiSgi) {
            SWITCH_GIC_BARRIER("dmb ishld");
            cpu.ipi(acknowledge);
        } else {
            cpu.other(intid);
            SWITCH_GIC_BARRIER("dsb sy");
        }
        cpu.end(acknowledge);
    }
}

// One SGI: `write` stores GICD_SGIR. One store barrier before it and none after (Linux gic_ipi_send_mask): a sender
// that then waits for the target's answer (XNU's xcall acknowledge loop, the boot cross-IPI qualification) polls
// normal memory, which the posted SGIR write does not hold up.
template <class Write> void sendSgi(Write write)
{
    SWITCH_GIC_BARRIER("dmb ishst");
    write();
}

// An SPI's IOKit vector (IOInterruptVector), in ArmCpu's acknowledge loop: interruptActive brackets the handler for
// IOInterruptController::disableInterrupt on another core, which stores interruptDisabledSoft, issues a DMB and waits
// for interruptActive to clear. Setting it is ordered before the read of interruptDisabledSoft by a full barrier
// (store against load: neither side may read before its own store is seen); clearing it is a release store, which
// orders the handler's accesses before it without holding back the EOI's. `run` calls the handler; `mask` writes the
// line's ICENABLER bit (and the Tegra LIC's), with no barrier: the acknowledge loop's DSB before the EOI covers it.
template <class Vector, class Run, class Mask> void runVector(Vector &vector, Run run, Mask mask)
{
    __atomic_store_n(&vector.interruptActive, 1, __ATOMIC_RELAXED);
    SWITCH_GIC_BARRIER("dmb ish");
    if (!__atomic_load_n(&vector.interruptDisabledSoft, __ATOMIC_RELAXED))
        run();
    if (__atomic_load_n(&vector.interruptDisabledSoft, __ATOMIC_RELAXED) && !vector.interruptDisabledHard) {
        vector.interruptDisabledHard = 1;
        mask();
    }
    __atomic_store_n(&vector.interruptActive, 0, __ATOMIC_RELEASE);
}

// A line masked from a thread (XNU's registerInterrupt and unregisterInterrupt): masked at the distributor on return.
template <class Mask> void maskFromThread(Mask mask)
{
    mask();
    SWITCH_GIC_BARRIER("dsb sy");
}

// A line unmasked (XNU's enableInterrupt, whose own DSB has already ordered the driver's device clear before it).
// Nothing waits for the unmask to arrive (Linux gic_unmask_irq has no barrier either).
template <class Unmask> void unmask(Unmask write)
{
    write();
}

}  // namespace switchgic

#endif
