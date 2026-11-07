// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <libkern/kernel_mach_header.h>
#include <arm64/switch_smp.h>
#include "../ArmGIC/ArmGIC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "../SwitchSuspend/SwitchSleepSteps.h"
#include "ArmCpuIdle.h"

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr(unsigned int);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop(vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start(processor_t);
extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
extern "C" thread_t current_thread(void);
extern "C" kern_return_t thread_terminate(thread_t);
extern "C" int switch_smp_publish_cpu_state(void);
// System sleep (drivers/SwitchSuspend): the scheduler's processor exit, XNU's sleep path, the platform quiesce/active
// actions.
extern "C" kern_return_t processor_exit(processor_t);
extern "C" void ml_arm_sleep(void);
extern "C" kern_return_t IOCPURunPlatformQuiesceActions(void);
extern "C" kern_return_t IOCPURunPlatformActiveActions(void);
// Owned here, read by the kernel (osfmk/arm64/cpu.c): arm64_prepare_for_sleep calls switch_cpu_sleep_handler when
// set, and cpu_machine_idle_init sets the flag at boot, so a kernel without the hook (whose arm64_prepare_for_sleep
// only parks in WFI) is recognised and sleep stays off. arm_init_cpu (osfmk/arm/arm_init.c) calls
// switch_cpu_wake_handler on the boot CPU back from SC7, before anything else in the kernel runs there.
extern "C" {
void (*switch_cpu_sleep_handler)(boolean_t) = nullptr;
void (*switch_cpu_wake_handler)(void) = nullptr;
bool switch_cpu_sleep_hook_present = false;
}

extern "C" {
volatile uint32_t armcpu_gic_version = 0;
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_ipi_selftest_sent = 0;
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8],
    armcpu_core_sgi[8];  // per logical core: architectural timer interrupts and SGI 0 taken
// Secondary startup timer probe: bits 1/2 virtual deadline passed / PPI 27 pending, 4/8 the same for the physical
// timer (PPI 30); diag = virtual ticks, physical ticks, (CNTV_CTL << 8 | CNTP_CTL), spins, (IGROUPR0 << 32 | ISENABLER0).
volatile uint32_t armcpu_timer_probe[8];
volatile uint64_t armcpu_timer_diag[8][5];
volatile uint32_t armcpu_quiesce_calls = 0, armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence. Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 local GIC ready,
// 7 initCPU returned, 8 scheduler/timer/self-IPI/cross-IPI qualified.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8],
    armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
volatile uint32_t armcpu_gic_targets[8], armcpu_smp_scheduler_ack[8];
volatile uint32_t armcpu_smp_self_ipi_ack[8], armcpu_smp_cross_out[8], armcpu_smp_cross_back[8];
}

// A CPU's GICv2 interface mask, once its interface is up. ArmGIC hands out SPI nubs only after CPU0's, and a
// driver that asks earlier sleeps on this event (switch_interrupt_nub).
extern "C" void arm_gic_cpu_online(uint32_t cpu);

static void publishGicTarget(uint32_t logicalId, uint32_t target)
{
    __atomic_store_n(&armcpu_gic_targets[logicalId], target, __ATOMIC_RELEASE);
    if (logicalId == 0) {
        thread_wakeup((event_t)&armcpu_gic_targets[0]);
    } else {
        arm_gic_cpu_online(logicalId);
    }
}

#define ARMCPU_SMP_SLOTS 8

class ArmCpu : public IOCPU {
    OSDeclareDefaultStructors(ArmCpu) OSSharedPtr<IOMemoryMap> distributor;
    OSSharedPtr<IOMemoryMap> redistributors;
    IOCPUInterruptController *controller = nullptr;
    const OSSymbol *cpuName = nullptr;
    uintptr_t dist = 0;
    uintptr_t redist = 0;
    uint64_t affinity = 0;
    uintptr_t redistBase = 0;
    uint64_t redistSize = 0;
    bool gicV2 = false;
    uint8_t gicV2Target = 0;
    bool secondary = false;
    uint32_t logicalId = 0;
    bool mapGic(void);
    bool initDistributor(void);
    bool prepareGicV2(void);
    bool prepareGicV2Local(void);
    void configureBankedV2(void);
    void resumeCPU(void);
    bool wakeRedistributor(bool local);
    bool finishSecondary(void);
    static void drainPending(void);
    bool prepareGic(void);
    void startSecondary(void);
    static void startThread(void *argument, wait_result_t);
    static void observeThread(void *argument, wait_result_t);
    void startAllObserver(void);
    static void observeAllThread(void *, wait_result_t);
    void enableGic(void);
    void interrupt(void);
    static void dispatch(void *target, void *, void *, int);

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void initCPU(bool boot) override;
    void quiesceCPU(void) override;
    void haltCPU(void) override;
    kern_return_t startCPU(vm_offset_t, vm_offset_t) override;
    void signalCPU(IOCPU *target) override;

    uint64_t signalTarget(void) const
    {
        return affinity;
    }

    void enableCPUTimeBase(bool enabled) override;
    const OSSymbol *getCPUName(void) override;
};

OSDefineMetaClassAndStructors(ArmCpu, IOCPU)

    // Redistributor ISENABLER0: SGIs 0..15 (only SGI 0 is used by the kernel; the others let the self-SGI probe tell which INTIDs the GIC accepts) and PPI 27.
    static const uint32_t kSgiPpiEnable = 0xffffU | (1U << 27);

static uint32_t read32(uintptr_t base, unsigned offset)
{
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32(uintptr_t base, unsigned offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static bool waitClear(uintptr_t base, unsigned offset, uint32_t mask)
{
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    uint64_t started = counter();
    while ((read32(base, offset) & mask) != 0) {
        if (counter() - started > frequency / 10) {
            return false;
        }
    }
    return true;
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uintptr_t gV2CpuIf = 0;
static uint8_t gV2BootTarget = 0;
static uint64_t gFramesSize = 0;
static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0;  // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gNativeSmp = false;
static bool gSharedGicV2 = false;
static uint32_t gV2ClaimedTargets = 0;
static uint32_t gCrossRequested[ARMCPU_SMP_SLOTS], gCrossAwaiting[ARMCPU_SMP_SLOTS];
static uint32_t gCrossReply[ARMCPU_SMP_SLOTS];
static uint32_t gStartDone[ARMCPU_SMP_SLOTS];
static int gStartStatus[ARMCPU_SMP_SLOTS];
static uint32_t gCpuOnAttempted[ARMCPU_SMP_SLOTS];
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
// System sleep is on (armcpu_system_sleep_enable): haltCPU/quiesceCPU/initCPU(false) take the PSCI sleep path.
static bool gSystemSleep = false;
static uint64_t gSleepEntryPA = 0, gSleepRecordPA = 0;
// The SoC watchdog across the steps no kernel watchdog sees (debug kernels): SC7 entry after the boot CPU's halt, and
// SC7 exit until IOPMrootDomain's own watchdog runs again. Either takes milliseconds.
static constexpr unsigned kSleepWatchSeconds = 10;

// Shared peripheral interrupts (INTID 32..1019) belong to the ArmGIC controller (drivers/ArmGIC), which drivers reach
// through IOInterruptEventSource. An SPI no driver owns is counted and disabled instead of being acknowledged forever.
static uint64_t gBootAffinity = 0;
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
}

extern "C" void switch_smp_scheduler_online(unsigned int cpu)
{
    if (cpu < ARMCPU_SMP_SLOTS) {
        __atomic_store_n(&armcpu_smp_scheduler_ack[cpu], 1U, __ATOMIC_RELEASE);
    }
}

static uint8_t gicV2TargetMask(uint32_t bankedTargets)
{
    bankedTargets |= bankedTargets >> 16;
    bankedTargets |= bankedTargets >> 8;
    return static_cast<uint8_t>(bankedTargets);
}

static void gicV2Send(uint32_t target)
{
    __asm__ volatile("dsb ishst" ::: "memory");
    write32(gDist, 0xf00, target << 16);  // TargetListFilter=0, SGI 0.
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

// Set once every secondary core passed its startup self- and cross-IPI qualification (observeThread): from then on
// SGIs skip gicV2IpiObserved, which only that qualification reads.
static bool gIpiQualified = false;

// A per-core counter only its own core writes, from interrupt context: a load and a store, no atomic read-modify-write.
static inline void countOwn(volatile uint32_t &counter)
{
    __atomic_store_n(&counter, __atomic_load_n(&counter, __ATOMIC_RELAXED) + 1U, __ATOMIC_RELEASE);
}

static void gicV2IpiObserved(uint32_t cpu, uint32_t acknowledge)
{
    if (!gNativeSmp || cpu >= SWITCH_SMP_CORE_COUNT) {
        return;
    }
    const uint32_t source = 1U << ((acknowledge >> 10) & 7);
    if (source == __atomic_load_n(&armcpu_gic_targets[cpu], __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&armcpu_smp_self_ipi_ack[cpu], 1U, __ATOMIC_RELEASE);
    }
    if (cpu == 0) {
        for (uint32_t peer = 1; peer < SWITCH_SMP_CORE_COUNT; ++peer) {
            const uint32_t target = __atomic_load_n(&armcpu_gic_targets[peer], __ATOMIC_ACQUIRE);
            if (source == target && __atomic_exchange_n(&gCrossReply[peer], 0U, __ATOMIC_ACQ_REL)) {
                __atomic_store_n(&armcpu_smp_cross_back[peer], 1U, __ATOMIC_RELEASE);
            }
            if (target && __atomic_exchange_n(&gCrossRequested[peer], 0U, __ATOMIC_ACQ_REL)) {
                __atomic_store_n(&gCrossAwaiting[peer], 1U, __ATOMIC_RELEASE);
                gicV2Send(target);
            }
        }
    } else if (source == gV2BootTarget && __atomic_exchange_n(&gCrossAwaiting[cpu], 0U, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&armcpu_smp_cross_out[cpu], 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&gCrossReply[cpu], 1U, __ATOMIC_RELEASE);
        gicV2Send(gV2BootTarget);
    }
}

IOService *ArmCpu::probe(IOService *provider, SInt32 *score)
{
    OSData *type = OSDynamicCast(OSData, provider->getProperty("device_type"));
    OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
    uint32_t id;
    if ((type == nullptr) || (type->getLength() != 4) || (memcmp(type->getBytesNoCopy(), "cpu", 4) != 0) ||
        (reg == nullptr) || (reg->getLength() < sizeof(id))) {
        return nullptr;
    }
    memcpy(&id, reg->getBytesNoCopy(), sizeof(id));
    if (ml_get_cpu_number(id) != ml_get_boot_cpu_number() &&
        (ml_get_cpu_number(id) < 0 || ml_get_cpu_number(id) >= ARMCPU_SMP_SLOTS)) {
        return nullptr;
    }
    return IOCPU::probe(provider, score);
}

bool ArmCpu::mapGic(void)
{
    IORegistryEntry *soc = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    IORegistryEntry *gic = IORegistryEntry::fromPath("/arm-io/intc", gIODTPlane);
    if ((soc == nullptr) || (gic == nullptr)) {
        if (soc != nullptr) {
            soc->release();
        }
        if (gic != nullptr) {
            gic->release();
        }
        return false;
    }
    OSData *ranges = OSDynamicCast(OSData, soc->getProperty("ranges"));
    OSData *reg = OSDynamicCast(OSData, gic->getProperty("reg"));
    OSData *compatible = OSDynamicCast(OSData, gic->getProperty("compatible"));
    uint64_t bus[3];
    uint64_t regions[4];
    // A GICv2 ADT supplies distributor and CPU-interface MMIO, not a redistributor.
    gicV2 = compatible != nullptr &&
            ((compatible->getLength() == sizeof("arm,cortex-a15-gic") &&
              memcmp(compatible->getBytesNoCopy(), "arm,cortex-a15-gic", sizeof("arm,cortex-a15-gic")) == 0) ||
             (compatible->getLength() == sizeof("arm,gic-400") &&
              memcmp(compatible->getBytesNoCopy(), "arm,gic-400", sizeof("arm,gic-400")) == 0));
    bool gicV3 = compatible != nullptr && compatible->getLength() == sizeof("arm,gic-v3") &&
                 memcmp(compatible->getBytesNoCopy(), "arm,gic-v3", sizeof("arm,gic-v3")) == 0;
    bool valid = (ranges != nullptr) && (ranges->getLength() == sizeof(bus)) && (reg != nullptr) &&
                 (reg->getLength() == sizeof(regions)) && (gicV2 || gicV3);
    if (valid) {
        memcpy(bus, ranges->getBytesNoCopy(), sizeof(bus));
        memcpy(regions, reg->getBytesNoCopy(), sizeof(regions));
    }
    OSData *strideProperty = OSDynamicCast(OSData, gic->getProperty("redistributor-stride"));
    if (strideProperty != nullptr && strideProperty->getLength() == sizeof(uint64_t)) {
        memcpy(&gRedistStride, strideProperty->getBytesNoCopy(), sizeof(uint64_t));
        if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) {
            gRedistStride = 0;
        }
    }
    armcpu_gic_stride = static_cast<uint32_t>(gRedistStride);
    soc->release();
    gic->release();
    if (!valid || (regions[1] < (gicV2 ? 0x1000U : 0x10000U)) || (regions[3] < (gicV2 ? 0x100U : 0x20000U))) {
        return false;
    }
    for (unsigned index = 0; index < 4; index += 2) {
        if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
            (regions[index + 1] > bus[2] - (regions[index] - bus[0])) || (bus[1] + regions[index] - bus[0] < bus[1])) {
            return false;
        }
    }
    OSSharedPtr<IOMemoryDescriptor> distMemory(
        IOMemoryDescriptor::withPhysicalAddress(bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut),
        OSNoRetain);
    OSSharedPtr<IOMemoryDescriptor> rdMemory(
        IOMemoryDescriptor::withPhysicalAddress(bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut),
        OSNoRetain);
    if (!distMemory || !rdMemory) {
        return false;
    }
    distributor = OSSharedPtr<IOMemoryMap>(distMemory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    redistributors = OSSharedPtr<IOMemoryMap>(rdMemory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    if (!distributor || !redistributors) {
        return false;
    }
    dist = static_cast<uintptr_t>(distributor->getVirtualAddress());
    if (((read32(dist, gicV2 ? 0xfe8 : 0xffe8) >> 4) & 15) != (gicV2 ? 2U : 3U)) {
        return false;
    }
    redistBase = static_cast<uintptr_t>(redistributors->getVirtualAddress());
    redistSize = regions[3];
    gDistMap = distributor;
    gRedistMap = redistributors;
    gDist = dist;
    gFrames = redistBase;
    gFramesSize = redistSize;
    gV2CpuIf = gicV2 ? redistBase : 0;
    armcpu_gic_version = gicV2 ? 2 : 3;
    return true;
}

bool ArmCpu::initDistributor(void)
{
    uint32_t ctl = read32(dist, 0);
    write32(dist, 0, ctl & ~3U);
    if (!waitClear(dist, 0, 1U << 31)) {
        return false;
    }
    write32(dist, 0, (ctl & ~3U) | 16U);
    if (!waitClear(dist, 0, 1U << 31)) {
        return false;
    }
    unsigned interrupts = ((read32(dist, 4) & 31) + 1) * 32;
    for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
        write32(dist, 0x180 + id / 8, 0xffffffff);
    }
    return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor(bool local)
{
    uintptr_t frames = redistBase;
    // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
    // from the DT affinity, so the new core never needs GIC MMIO of its own.
    if (local) {
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(affinity));
    }
    uint32_t target = static_cast<uint32_t>((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
    redist = 0;
    for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
        armcpu_gic_typer_probe[0] = static_cast<uint32_t>(offset);
        uint64_t typer = *reinterpret_cast<volatile uint64_t *>(frames + offset + 8);
        armcpu_gic_typer_probe[1] = static_cast<uint32_t>(typer);
        if (static_cast<uint32_t>(typer >> 32) == target) {
            redist = frames + offset;
            break;
        }
        if ((typer & 16) != 0) {
            break;
        }
        offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
    }
    if (redist == 0) {
        return false;
    }
    write32(redist, 0x14, read32(redist, 0x14) & ~2U);
    if (!waitClear(redist, 0x14, 4)) {
        return false;
    }
    uintptr_t sgi = redist + 0x10000;
    write32(sgi, 0x180, 0xffffffff);
    write32(sgi, 0x80, 0xffffffff);
    write32(sgi, 0xd00, 0);
    write32(sgi, 0x280, 0xffffffff);
    for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
        write32(sgi, offset, 0x80808080);
    }
    if (!local) {
        write32(sgi, 0x100, kSgiPpiEnable);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return waitClear(redist, 0, 8);
}

// Arm IHI 0048B.b: GICv2 uses banked SGI/PPI registers in GICD and a
// memory-mapped GICC. The NS GICC view controls Group 1; firmware must already
// have assigned those interrupts to the non-secure OS. Do not attempt to
// change security ownership from EL1. With Security Extensions absent, the
// same enable bit controls the ordinary interrupt group.
bool ArmCpu::prepareGicV2(void)
{
    const unsigned cpus = static_cast<unsigned>(ml_get_max_cpu_number()) + 1;
    if ((cpus != 1 && (!gNativeSmp || cpus != SWITCH_SMP_CORE_COUNT)) || cpus > (((read32(dist, 4) >> 5) & 7) + 1)) {
        return false;
    }
    // Only the boot CPU touches global SPI state. Secondaries must never reset
    // the distributor while peripheral drivers are using it.
    write32(dist, 0, read32(dist, 0) & ~1U);
    const unsigned interrupts = ((read32(dist, 4) & 31) + 1) * 32;
    for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
        write32(dist, 0x180 + id / 8, 0xffffffff);
    }
    return prepareGicV2Local();
}

bool ArmCpu::prepareGicV2Local(void)
{
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(affinity));
    if (logicalId >= ARMCPU_SMP_SLOTS || (secondary && (affinity & 0xff00ffffffULL) != armcpu_smp_mpidr[logicalId]) ||
        (gNativeSmp && (affinity & 0xff00ffffffULL) != logicalId)) {
        return false;
    }
    if (!secondary) {
        gBootAffinity = affinity & 0xff00ffffffULL;
    }
    // The GIC target mask is hardware-defined; it need not equal MPIDR Aff0.
    const uint8_t target = gicV2TargetMask(read32(dist, 0x800));
    // GICv2 uniprocessor implementations make ITARGETSR RAZ/WI (IHI 0048B, 4.3.12).
    // Only a multi-interface controller requires a nonzero discovered target.
    const bool uniprocessor = ((read32(dist, 4) >> 5) & 7) == 0;
    if ((!uniprocessor && !target) || (target & (target - 1)) != 0) {
        return false;
    }
    uint32_t claimed = __atomic_load_n(&gV2ClaimedTargets, __ATOMIC_ACQUIRE);
    do {
        if (target && (claimed & target)) {
            return false;
        }
    } while (!__atomic_compare_exchange_n(&gV2ClaimedTargets, &claimed, claimed | target, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    gicV2Target = target;
    if (!secondary) {
        gV2BootTarget = target;
    }
    if (secondary && !gNativeSmp) {
        return false;
    }
    configureBankedV2();
    return true;
}

// This core's banked GICD view: SGI 0 and the virtual timer PPI 27. At first start and after system sleep.
void ArmCpu::configureBankedV2(void)
{
    write32(dist, 0x180, 0xffff0000);                         // Disable PPIs while configuring.
    write32(dist, 0x280, 1U << 27);                           // Virtual timer PPI pending clear.
    write32(dist, 0xf10, 0xff);                               // SGI 0: clear all source-CPU pending bits.
    write32(dist, 0x380, (1U << 27) | 1U);                    // Initial timer/SGI active clear.
    write32(dist, 0xc04, read32(dist, 0xc04) & ~(2U << 22));  // PPI 27 is level-sensitive.
    *reinterpret_cast<volatile uint8_t *>(dist + 0x400) = 0x80;
    *reinterpret_cast<volatile uint8_t *>(dist + 0x400 + 27) = 0x80;
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

// This core's banked GIC state back from a C7 power-down (ArmCpuIdle.cpp): what configureBankedV2 and enableGic set
// and ATF's warm boot (tegra_gic_pcpu_init) reset, with nothing pending cleared: the IPI that woke the core may be.
void armcpu::restoreBankedGic(void)
{
    *reinterpret_cast<volatile uint8_t *>(gDist + 0x400) = 0x80;
    *reinterpret_cast<volatile uint8_t *>(gDist + 0x400 + 27) = 0x80;
    write32(gV2CpuIf, 4, 0xff);
    write32(gV2CpuIf, 8, 0);
    write32(gV2CpuIf, 0, (read32(gV2CpuIf, 0) & 0x1e0U) | 1U);
    write32(gDist, 0x100, (1U << 27) | 1U);
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

bool ArmCpu::prepareGic(void)
{
    if (!mapGic()) {
        return false;
    }
    if (gicV2) {
        if (!prepareGicV2()) {
            return false;
        }
    } else if (!initDistributor() || !wakeRedistributor(true)) {
        return false;
    }
    gSharedGicV2 = gicV2;
    __atomic_store_n(&gGicShared, 1U, __ATOMIC_RELEASE);
    return true;
}

void ArmCpu::enableGic(void)
{
    if (gicV2) {
        write32(gV2CpuIf, 4, 0xff);  // GICC_PMR
        write32(gV2CpuIf, 8, 0);     // GICC_BPR
        // Keep bypass disable bits, select EOImode 0, and enable the NS interface.
        // Acknowledgement and deactivation then pair through IAR/EOIR, not DIR.
        write32(gV2CpuIf, 0, (read32(gV2CpuIf, 0) & 0x1e0U) | 1U);
        write32(dist, 0x100, (1U << 27) | 1U);
        if (!secondary) {
            write32(dist, 0, read32(dist, 0) | 1U);
        }
        __asm__ volatile("dsb sy\nisb" ::: "memory");
        if ((read32(gV2CpuIf, 0) & 1) == 0 || (read32(dist, 0) & 1) == 0 ||
            (read32(dist, 0x100) & ((1U << 27) | 1U)) != ((1U << 27) | 1U)) {
            panic("ArmCpu: GICv2 timer/interface not accessible; check firmware security handoff");
        }
        if (!secondary) {
            armcpu_gic_boot[0] = read32(dist, 0x80);
            armcpu_gic_boot[1] = read32(dist, 0x100);
            armcpu_gic_boot[2] = read32(dist, 0);
            armcpu_gic_boot[3] = read32(dist, 4);
            armcpu_gic_boot[4] = gV2BootTarget;
            armcpu_gic_boot[5] = read32(gV2CpuIf, 4);
            armcpu_gic_boot[6] = read32(gV2CpuIf, 0);
            armcpu_gic_boot[7] = read32(gV2CpuIf, 0xfc);
        }
        __atomic_store_n(&armcpu_gic_ready, 1U, __ATOMIC_RELEASE);
        return;
    }
    uint64_t value;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(value));
    value |= 1;
    __asm__ volatile("msr icc_sre_el1, %0\nisb" ::"r"(value) : "memory");
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(value));
    if ((value & 1) == 0) {
        panic("ArmCpu: GICv3 system register interface unavailable");
    }
    __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(value));
    value &= ~2ULL;  // EOImode 0: EOIR also deactivates the interrupt.
    __asm__ volatile("msr icc_ctlr_el1, %0" ::"r"(value) : "memory");
    value = 0xff;
    __asm__ volatile("msr icc_pmr_el1, %0" ::"r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr icc_bpr1_el1, %0" ::"r"(value) : "memory");
    if (!secondary) {
        gBootAffinity = affinity & 0xff00ffffffULL;
    }
    {
        write32(redist + 0x10000, 0x100, kSgiPpiEnable);
        write32(dist, 0, read32(dist, 0) | 0x12U);
    }
    value = 1;
    __asm__ volatile("msr icc_igrpen1_el1, %0\ndsb sy\nisb" ::"r"(value) : "memory");
    if (!secondary) {
        uint64_t pmr, ig, sre;
        __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(pmr));
        __asm__ volatile("mrs %0, icc_igrpen1_el1" : "=r"(ig));
        __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
        armcpu_gic_boot[0] = read32(redist + 0x10000, 0x80);
        armcpu_gic_boot[1] = read32(redist + 0x10000, 0x100);
        armcpu_gic_boot[2] = read32(dist, 0);
        armcpu_gic_boot[3] = read32(dist, 4);
        armcpu_gic_boot[4] = read32(redist + 0x10000, 0xd00);
        armcpu_gic_boot[5] = static_cast<uint32_t>(pmr);
        armcpu_gic_boot[6] = static_cast<uint32_t>(ig);
        armcpu_gic_boot[7] = static_cast<uint32_t>(sre);
    }
    armcpu_gic_ready = 1;
}

bool ArmCpu::start(IOService *provider)
{
    uint32_t physical;
    OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
    if (!reg || reg->getLength() < sizeof(physical)) {
        return false;
    }
    memcpy(&physical, reg->getBytesNoCopy(), sizeof(physical));
    logicalId = static_cast<uint32_t>(ml_get_cpu_number(physical));
    if (logicalId >= ARMCPU_SMP_SLOTS) {
        return false;
    }
    secondary = logicalId != static_cast<uint32_t>(ml_get_boot_cpu_number());
    if (secondary) {
        if (!IOCPU::start(provider)) {
            return false;
        }
        armcpu_smp_step[logicalId] = 1;
        affinity = physical;
        armcpu_smp_mpidr[logicalId] = physical;
        cpuName = OSSymbol::withCStringNoCopy("ARM architectural secondary CPU");
        if (cpuName == nullptr) {
            return false;
        }
        retain();
        __atomic_store_n(&gPending[logicalId], this, __ATOMIC_RELEASE);
        if (gBootRegistered != 0) {
            drainPending();
        }
        return true;
    } else {
        gNativeSmp = ml_get_max_cpu_number() != 0;
        uint64_t bootMpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(bootMpidr));
        armcpu_boot_mpidr = static_cast<uint32_t>(bootMpidr);
        if (!prepareGic() || !IOCPU::start(provider)) {
            return false;
        }
    }
    cpuName = OSSymbol::withCStringNoCopy("ARM architectural boot CPU");
    if (cpuName == nullptr) {
        return false;
    }
    ml_processor_info_t info = {};
    info.cpu_id = this;
    info.phys_id = physical;
    info.log_id = logicalId;
    info.cluster_id = static_cast<uint32_t>(ml_get_cluster_number(physical));
    info.cluster_type = ml_get_boot_cluster_type();
    perfmon_interrupt_handler_func pmi = nullptr;
    kern_return_t status = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi);
    if (status != KERN_SUCCESS) {
        panic("ArmCpu: CPU %u registration failed: %d", logicalId, status);
    }
    setCPUNumber(info.log_id);
    armcpu_registered = 1;
    armcpu_smp_requested = 1;
    unsigned cpuCount = static_cast<unsigned>(ml_get_max_cpu_number()) + 1;
    controller = new IOCPUInterruptController;
    if ((controller == nullptr) || (controller->initCPUInterruptController(cpuCount, 1) != kIOReturnSuccess)) {
        panic("ArmCpu: CPU interrupt controller allocation failed");
    }
    controller->registerCPUInterruptController();
    controller->enableCPUInterrupt(this);
    if (controller->registerInterrupt(provider, logicalId, this, &ArmCpu::dispatch, nullptr) != kIOReturnSuccess) {
        panic("ArmCpu: CPU IRQ action registration failed");
    }
    gController = controller;
    initCPU(true);
    registerService();
    __atomic_store_n(&gBootRegistered, 1U, __ATOMIC_RELEASE);
    startAllObserver();
    drainPending();
    return true;
}

void ArmCpu::drainPending(void)
{
    for (unsigned id = 0; id < ARMCPU_SMP_SLOTS; id++) {
        ArmCpu *cpu = __atomic_exchange_n(&gPending[id], static_cast<ArmCpu *>(nullptr), __ATOMIC_ACQ_REL);
        if (cpu != nullptr) {
            cpu->finishSecondary();
        }
    }
}

bool ArmCpu::finishSecondary(void)
{
    if (!__atomic_load_n(&gGicShared, __ATOMIC_ACQUIRE)) {
        return false;
    }
    gicV2 = gSharedGicV2;
    dist = gDist;
    redistBase = gFrames;
    redistSize = gFramesSize;
    if (gicV2 && (!gNativeSmp || logicalId >= SWITCH_SMP_CORE_COUNT || affinity != logicalId)) {
        armcpu_smp_fail[logicalId] = 3;
        return false;
    }
    ml_processor_info_t info = {};
    info.cpu_id = this;
    info.phys_id = static_cast<uint32_t>(affinity);
    info.log_id = logicalId;
    info.cluster_id = static_cast<uint32_t>(ml_get_cluster_number(static_cast<uint32_t>(affinity)));
    info.cluster_type = ml_get_boot_cluster_type();
    perfmon_interrupt_handler_func pmi = nullptr;
    kern_return_t status = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi);
    if (status != KERN_SUCCESS) {
        armcpu_smp_fail[logicalId] = 4;
        return false;
    }
    setCPUNumber(info.log_id);
    armcpu_smp_step[logicalId] = 2;
    setCPUState(kIOCPUStateUninitalized);
    registerService();
    startSecondary();
    return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit(bool *hvc)
{
    IORegistryEntry *psci = IORegistryEntry::fromPath("/psci", gIODTPlane);
    if (psci == nullptr) {
        return false;
    }
    OSData *method = OSDynamicCast(OSData, psci->getProperty("method"));
    bool ok = false;
    if (method != nullptr && method->getLength() == 4 && memcmp(method->getBytesNoCopy(), "smc", 4) == 0) {
        *hvc = false;
        ok = true;
    } else if (method != nullptr && method->getLength() == 4 && memcmp(method->getBytesNoCopy(), "hvc", 4) == 0) {
        *hvc = true;
        ok = true;
    }
    if (armcpu_gic_version == 2) {
        OSData *compatible = OSDynamicCast(OSData, psci->getProperty("compatible"));
        const bool known =
            compatible && ((compatible->getLength() == sizeof("arm,psci-0.2") &&
                            !memcmp(compatible->getBytesNoCopy(), "arm,psci-0.2", sizeof("arm,psci-0.2"))) ||
                           (compatible->getLength() == sizeof("arm,psci-1.0") &&
                            !memcmp(compatible->getBytesNoCopy(), "arm,psci-1.0", sizeof("arm,psci-1.0"))));
        ok = ok && !*hvc && known && gNativeSmp;
    }
    psci->release();
    return ok;
}

static int64_t psciCall(bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = b;
    register uint64_t x3 __asm__("x3") = c;
    if (hvc) {
        __asm__ volatile("hvc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                           "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    } else {
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                           "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    }
    return static_cast<int64_t>(x0);
}

static bool qualifyNativePsci(uint64_t target)
{
    const uint64_t version = static_cast<uint64_t>(psciCall(false, 0x84000000, 0, 0, 0));
    const unsigned major = static_cast<unsigned>(version >> 16);
    if (version > UINT32_MAX || major > 1 || (!major && version < 2)) {
        return false;
    }
    if (major &&
        (psciCall(false, 0x8400000a, 0xc4000003, 0, 0) != 0 || psciCall(false, 0x8400000a, 0xc4000004, 0, 0) != 0)) {
        return false;
    }
    // PSCI 0.2 mandates CPU_ON and AFFINITY_INFO. Never start an already-on or
    // transitioning core, and verify the boot affinity is online on this conduit.
    return psciCall(false, 0xc4000004, gBootAffinity, 0, 0) == 0 && psciCall(false, 0xc4000004, target, 0, 0) == 1;
}

static void invalidateSecondaryStatus(switch_smp_record *record)
{
    const uintptr_t status = reinterpret_cast<uintptr_t>(record) + SWITCH_SMP_STATUS_OFFSET;
    // This cache line belongs exclusively to the secondary after CPU_ON. The
    // boot CPU never dirties it while polling cache-off entry/fault evidence.
    __asm__ volatile("dsb sy\ndc ivac, %0\ndsb sy" ::"r"(status) : "memory");
}

static bool nativeSmpReady(uint32_t cpu, uint64_t stage)
{
    return cpu < SWITCH_SMP_CORE_COUNT && stage == SWITCH_SMP_STAGE_C &&
           __atomic_load_n(&armcpu_smp_scheduler_ack[cpu], __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&armcpu_gic_targets[cpu], __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&armcpu_core_timer[cpu], __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&armcpu_smp_self_ipi_ack[cpu], __ATOMIC_ACQUIRE);
}

void ArmCpu::startAllObserver(void)
{
    if (!gicV2 || !gNativeSmp) {
        return;
    }
    thread_t observer;
    if (kernel_thread_start(&ArmCpu::observeAllThread, nullptr, &observer) != KERN_SUCCESS) {
        panic("ArmCpu: all-four startup observer allocation failed");
    }
    thread_deallocate(observer);
}

void ArmCpu::observeAllThread(void *, wait_result_t)
{
    // This observer starts before draining nubs, so missing nubs or failed
    // registration/start threads cannot evade the per-core qualification checks.
    const uint64_t started = counter();
    for (;;) {
        bool ready = __atomic_load_n(&armcpu_smp_running, __ATOMIC_ACQUIRE) == SWITCH_SMP_CORE_COUNT - 1;
        for (uint32_t cpu = 1; cpu < SWITCH_SMP_CORE_COUNT; ++cpu) {
            ready = ready && __atomic_load_n(&armcpu_smp_step[cpu], __ATOMIC_ACQUIRE) == 8;
        }
        if (ready) {
            IOLog("ArmCpu: %u of %u secondary cores running\n", SWITCH_SMP_CORE_COUNT - 1, SWITCH_SMP_CORE_COUNT - 1);
            armcpu_idle_start();
            thread_terminate(current_thread());
            return;
        }
        // Allow the three per-core five-second startup budgets and late nub matching.
        if (counter() - started >= UINT64_C(19200000) * 20) {
            break;
        }
        IOSleep(10);
    }
    // Cores that never reached the scheduler are simply absent; the system runs on the rest.
    IOLog("ArmCpu: %u of %u secondary cores running (CPU1=%u/%u/%d CPU2=%u/%u/%d CPU3=%u/%u/%d step/fail/rc)\n",
          armcpu_smp_running, SWITCH_SMP_CORE_COUNT - 1, armcpu_smp_step[1], armcpu_smp_fail[1], armcpu_smp_psci_rc[1],
          armcpu_smp_step[2], armcpu_smp_fail[2], armcpu_smp_psci_rc[2], armcpu_smp_step[3], armcpu_smp_fail[3],
          armcpu_smp_psci_rc[3]);
    thread_terminate(current_thread());
}

void ArmCpu::startThread(void *argument, wait_result_t)
{
    ArmCpu *self = static_cast<ArmCpu *>(argument);
    __atomic_store_n(&armcpu_smp_step[self->logicalId], 3U, __ATOMIC_RELEASE);
    if (self->gicV2) {
        thread_t observer;
        if (kernel_thread_start(&ArmCpu::observeThread, self, &observer) != KERN_SUCCESS) {
            panic("ArmCpu: CPU%u startup observer allocation failed", self->logicalId);
        }
        thread_deallocate(observer);
    }
    const kern_return_t result = processor_start(self->machProcessor);
    gStartStatus[self->logicalId] = result;
    __atomic_store_n(&gStartDone[self->logicalId], 1U, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}

void ArmCpu::observeThread(void *argument, wait_result_t)
{
    ArmCpu *self = static_cast<ArmCpu *>(argument);
    const uint64_t started = counter();
    bool requested = false;
    for (;;) {
        switch_smp_record *record = &switch_secondary_records[self->logicalId];
        const uint32_t cpu = self->logicalId;
        const bool issued = __atomic_load_n(&armcpu_smp_step[cpu], __ATOMIC_ACQUIRE) >= 4;
        if (issued) {
            invalidateSecondaryStatus(record);
        }
        const bool returned = __atomic_load_n(&gStartDone[cpu], __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&armcpu_smp_fail[cpu], __ATOMIC_ACQUIRE) || (issued && record->error) ||
            (returned && (gStartStatus[cpu] != KERN_SUCCESS || armcpu_smp_psci_rc[cpu] != 0))) {
            break;
        }
        const bool ready = issued && returned && nativeSmpReady(cpu, record->stage);
        if (ready && !requested) {
            __atomic_store_n(&gCrossRequested[cpu], 1U, __ATOMIC_RELEASE);
            gicV2Send(gV2BootTarget);
            requested = true;
        }
        if (ready && __atomic_load_n(&armcpu_smp_cross_back[cpu], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&armcpu_smp_step[cpu], 8U, __ATOMIC_RELEASE);
            if (__atomic_add_fetch(&armcpu_smp_running, 1U, __ATOMIC_RELAXED) == SWITCH_SMP_CORE_COUNT - 1)
                __atomic_store_n(&gIpiQualified, true, __ATOMIC_RELEASE);
            thread_terminate(current_thread());
            return;
        }
        if (counter() - started >= UINT64_C(19200000) * 5) {
            break;
        }
        IOSleep(10);
    }
    switch_smp_record *record = &switch_secondary_records[self->logicalId];
    // A core that never acknowledged the scheduler gets no work: leave it out and continue. One that joined
    // the scheduler but cannot take interrupts would hang the system, so that still stops the boot.
    if (!__atomic_load_n(&armcpu_smp_scheduler_ack[self->logicalId], __ATOMIC_ACQUIRE)) {
        IOLog("ArmCpu: CPU%u did not start (rc=%d step=%u fail=%u entry=%llu error=%llu ESR=%llx ELR=%llx); continuing "
              "without it\n",
              self->logicalId, armcpu_smp_psci_rc[self->logicalId], armcpu_smp_step[self->logicalId],
              armcpu_smp_fail[self->logicalId], record->stage, record->error, record->esr, record->elr);
        thread_terminate(current_thread());
    }
    panic("ArmCpu: CPU%u startup failed rc=%d step=%u fail=%u entry=%llu error=%llu ESR=%llx ELR=%llx FAR=%llx "
          "sched=%u timer=%u/%x "
          "dv=%llu dp=%llu ctl=%llx spin=%llu gic=%llx self=%u cross=%u/%u",
          self->logicalId, armcpu_smp_psci_rc[self->logicalId], armcpu_smp_step[self->logicalId],
          armcpu_smp_fail[self->logicalId], record->stage, record->error, record->esr, record->elr, record->far,
          armcpu_smp_scheduler_ack[self->logicalId], armcpu_core_timer[self->logicalId],
          armcpu_timer_probe[self->logicalId], armcpu_timer_diag[self->logicalId][0],
          armcpu_timer_diag[self->logicalId][1], armcpu_timer_diag[self->logicalId][2],
          armcpu_timer_diag[self->logicalId][3], armcpu_timer_diag[self->logicalId][4],
          armcpu_smp_self_ipi_ack[self->logicalId], armcpu_smp_cross_out[self->logicalId],
          armcpu_smp_cross_back[self->logicalId]);
    thread_terminate(current_thread());
}

void ArmCpu::startSecondary(void)
{
    thread_t thread;
    if (kernel_thread_start(&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
        armcpu_smp_psci_rc[logicalId] = -1000;
        return;
    }
    thread_deallocate(thread);
}

void ArmCpu::initCPU(bool boot)
{
    if (!boot) {
        resumeCPU();
        return;
    }
    if (secondary) {
        __atomic_store_n(&armcpu_smp_step[logicalId], 5U, __ATOMIC_RELEASE);
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t>(mpidr);
        // A failure is recorded and the core parked (rather than a panic that hides the other cores' state).
        if ((mpidr & 0xff00ffffffULL) != (affinity & 0xff00ffffffULL)) {
            __atomic_store_n(&armcpu_smp_fail[logicalId], 1U, __ATOMIC_RELEASE);
            for (;;) {
                __asm__ volatile("wfe");
            }
        }
        if (gicV2 ? !prepareGicV2Local() : !wakeRedistributor(true)) {
            __atomic_store_n(&armcpu_smp_fail[logicalId], 2U, __ATOMIC_RELEASE);
            for (;;) {
                __asm__ volatile("wfe");
            }
        }
        __atomic_store_n(&armcpu_smp_step[logicalId], 6U, __ATOMIC_RELEASE);
        enableGic();
        // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
        // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
        ml_install_interrupt_handler(this, static_cast<int>(logicalId), this,
                                     reinterpret_cast<IOInterruptHandler>(&ArmCpu::dispatch), nullptr);
        // An idle core gets no deadline of its own for a long time (XNU's idle path pushes the timer out), so the
        // startup observer cannot wait for a real tick. Prove the path here instead: with IRQs masked, a 10 us
        // deadline must leave the timer PPI pending at this core (GICD_ISPENDR0 is banked); then push it out.
        // The physical timer (PPI 30) runs alongside as a comparison, and both counters' progress is recorded.
        if (gicV2) {
            uint64_t daif, vctl = 0, pctl = 0, v0, p0, v1, p1;
            uint32_t seen = 0, spin = 0;
            __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif)::"memory");
            __asm__ volatile(
                "msr cntv_tval_el0, %0\nmsr cntv_ctl_el0, %1\nmsr cntp_tval_el0, %0\nmsr cntp_ctl_el0, %1\nisb" ::"r"(
                    UINT64_C(192)),
                "r"(UINT64_C(1))
                : "memory");
            __asm__ volatile("isb\nmrs %0, cntvct_el0\nmrs %1, cntpct_el0" : "=r"(v0), "=r"(p0));
            for (; spin < 1000000 && (seen & 0xa) != 0xa; spin++) {
                __asm__ volatile("isb\nmrs %0, cntv_ctl_el0\nmrs %1, cntp_ctl_el0\nmrs %2, cntpct_el0"
                                 : "=r"(vctl), "=r"(pctl), "=r"(p1));
                const uint32_t pending = read32(dist, 0x200);
                if (vctl & 4) {
                    seen |= 1;
                }  // virtual deadline passed
                if (pending & (1U << 27)) {
                    seen |= 2;
                }  // virtual timer PPI pending at this core
                if (pctl & 4) {
                    seen |= 4;
                }  // physical deadline passed
                if (pending & (1U << 30)) {
                    seen |= 8;
                }  // physical timer PPI pending at this core
                if (p1 - p0 > UINT64_C(19200)) {
                    break;
                }  // 1 ms of physical time
            }
            __asm__ volatile("isb\nmrs %0, cntvct_el0\nmrs %1, cntpct_el0" : "=r"(v1), "=r"(p1));
            __asm__ volatile("msr cntv_tval_el0, %0\nmsr cntp_ctl_el0, xzr\nisb" ::"r"(UINT64_C(0x7fffffff))
                             : "memory");
            write32(dist, 0x280, 1U << 30);  // drop any physical timer pending state
            armcpu_timer_probe[logicalId] = seen;
            armcpu_timer_diag[logicalId][0] = v1 - v0;
            armcpu_timer_diag[logicalId][1] = p1 - p0;
            armcpu_timer_diag[logicalId][2] = (vctl << 8) | pctl;
            armcpu_timer_diag[logicalId][3] = spin;
            armcpu_timer_diag[logicalId][4] = (uint64_t)read32(dist, 0x80) << 32 | read32(dist, 0x100);
            if (seen & 2) {
                __atomic_fetch_add(&armcpu_core_timer[logicalId], 1U, __ATOMIC_RELEASE);
            }
            __asm__ volatile("msr daif, %0\nisb" ::"r"(daif) : "memory");
        }
        enableCPUTimeBase(true);
        setCPUState(kIOCPUStateRunning);
        if (gicV2) {
            publishGicTarget(logicalId, gicV2Target);
        }
        __atomic_store_n(&armcpu_smp_step[logicalId], 7U, __ATOMIC_RELEASE);
        if (!gicV2) {
            __atomic_fetch_add(&armcpu_smp_running, 1U, __ATOMIC_RELAXED);
        }
        // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
        // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
        signalCPU(this);
        return;
    }
    enableGic();
    enableCPUTimeBase(true);
    setCPUState(kIOCPUStateRunning);
    if (gicV2) {
        publishGicTarget(logicalId, gicV2Target);
    }
    signalCPU(this);
}

void ArmCpu::dispatch(void *target, void *, void *, int)
{
    static_cast<ArmCpu *>(target)->interrupt();
}

// Every interrupt on every core comes through here: each core's own counters are plain stores (only that core writes
// its slot, with interrupts masked), not exclusive-monitor read-modify-writes.
void ArmCpu::interrupt(void)
{
    for (;;) {
        uint64_t id;
        uint32_t v2Acknowledge = 0;
        if (gicV2) {
            v2Acknowledge = read32(gV2CpuIf, 0xc);
            id = v2Acknowledge & 0x3ffU;
            // Pair with the sender's store barrier before consuming IPI state.
            __asm__ volatile("dmb ish" ::: "memory");
        } else {
            __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(id));
        }
        if ((id & 0xffffff) >= 1020) {
            break;
        }
        if ((id & 0xffffff) == 27) {
            rtclock_intr(0);
            countOwn(armcpu_core_timer[logicalId]);
        } else if ((id & 0xffffff) == 0) {
            if (ipi_handler != nullptr) {
                ipi_handler();
            }
            countOwn(armcpu_core_sgi[logicalId]);
            if (gicV2 && !__atomic_load_n(&gIpiQualified, __ATOMIC_RELAXED)) {
                gicV2IpiObserved(logicalId, v2Acknowledge);
            }
        } else {
            const uint32_t intid = static_cast<uint32_t>(id & 0xffffff);
            if (intid >= 32 && switch_gic_dispatch(intid)) {
                // A driver's vector ran (or masked its soft-disabled line); the EOI below is still the only one.
            } else if (intid >= 32 && gDist != 0) {
                armcpu_spi_unhandled = armcpu_spi_unhandled + 1;
                armcpu_spi_unhandled_id = intid;
                write32(gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
            } else {
                panic("ArmCpu: unexpected architectural IRQ %llu", id);
            }
        }
        if (gicV2) {
            // Preserve source-CPU bits [12:10] for SGIs, not only the 10-bit INTID.
            __asm__ volatile("dsb sy" ::: "memory");
            write32(gV2CpuIf, 0x10, v2Acknowledge);
            __asm__ volatile("dsb sy\nisb" ::: "memory");
        } else {
            __asm__ volatile("msr icc_eoir1_el1, %0\nisb" ::"r"(id) : "memory");
        }
    }
}

void ArmCpu::signalCPU(IOCPU *target)
{
    ArmCpu *peer = OSDynamicCast(ArmCpu, target);
    if (peer == nullptr || (peer->signalTarget() & 0xff) >= 16) {
        panic("ArmCpu: unsupported CPU signal target");
    }
    uint64_t aff = peer->signalTarget();
    if (gicV2) {
        if (!peer->gicV2) {
            panic("ArmCpu: mixed GIC CPU signal target");
        }
        if (peer == this) {
            __asm__ volatile("dsb ishst" ::: "memory");
            write32(dist, 0xf00, 2U << 24);  // Requesting CPU, SGI 0.
            __asm__ volatile("dsb sy\nisb" ::: "memory");
        } else {
            const uint32_t targetMask = __atomic_load_n(&armcpu_gic_targets[peer->logicalId], __ATOMIC_ACQUIRE);
            if (!gNativeSmp || !targetMask || (targetMask & (targetMask - 1))) {
                panic("ArmCpu: GICv2 CPU%u target unavailable", peer->logicalId);
            }
            gicV2Send(targetMask);
        }
        return;
    }
    uint64_t sgi =
        ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
    __asm__ volatile("dsb ishst\nmsr icc_sgi1r_el1, %0\nisb" ::"r"(sgi) : "memory");
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_core_sgi before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;

extern "C" void armcpu_gic_diag(uint64_t aff, uint32_t *out)
{
    for (unsigned i = 0; i < 8; i++) {
        out[i] = 0xdeadbeef;
    }
    if (gDist == 0 || gFrames == 0) {
        armcpu_gic_diag_stage = 0xe;
        return;
    }
    if (armcpu_gic_version == 2) {
        if ((aff & 0xff00ffffffULL) != gBootAffinity) {
            armcpu_gic_diag_stage = 0xe;
            return;
        }
        out[0] = read32(gDist, 0x80);
        out[1] = read32(gDist, 0x100);
        out[2] = read32(gDist, 0x200);
        out[3] = read32(gV2CpuIf, 0);
        out[4] = read32(gV2CpuIf, 0x18);
        out[5] = read32(gDist, 0);
        out[6] = read32(gDist, 0x400);
        out[7] = 1;
        armcpu_gic_diag_stage = 9;
        return;
    }
    const uint32_t target = static_cast<uint32_t>((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
    for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
        armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t>(offset >> 16);
        const uint64_t typer = *reinterpret_cast<volatile uint64_t *>(gFrames + offset + 8);
        if (static_cast<uint32_t>(typer >> 32) == target) {
            const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
            armcpu_gic_diag_stage = 2;
            out[0] = read32(sgi, 0x80);
            armcpu_gic_diag_stage = 3;
            out[1] = read32(sgi, 0x100);
            armcpu_gic_diag_stage = 4;
            out[2] = read32(sgi, 0x200);
            armcpu_gic_diag_stage = 5;
            out[3] = read32(sgi, 0xd00);
            armcpu_gic_diag_stage = 6;
            out[4] = read32(redist, 0x14);
            armcpu_gic_diag_stage = 7;
            out[5] = read32(gDist, 0);
            armcpu_gic_diag_stage = 8;
            out[6] = read32(sgi, 0x400);
            out[7] = 1;
            armcpu_gic_diag_stage = 9;
            return;
        }
        if ((typer & 16) != 0) {
            break;
        }
        offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
    }
    armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe(uint64_t *out)
{
    // This diagnostic probes the GICv3 system-register path only.
    if (armcpu_gic_version == 2) {
        for (unsigned i = 0; i < 10; i++) {
            out[i] = ~0ULL;
        }
        return;
    }
    uint64_t mpidr, daif, v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2\nisb" : "=r"(daif)::"memory");
    for (unsigned i = 0; i < 10; i++) {
        out[i] = 0;
    }
    out[0] = mpidr;
    __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(v));
    out[3] = v;
    __asm__ volatile("mrs %0, icc_igrpen1_el1" : "=r"(v));
    out[4] = v;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(v));
    out[5] = v;
    __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(v));
    out[6] = v;
    __asm__ volatile("mrs %0, icc_rpr_el1" : "=r"(v));
    out[7] = v;
    if ((mpidr & 0xff) < 16) {
        const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) |
                              (1ULL << (mpidr & 0xff));
        for (uint64_t intid = 0; intid < 16; intid++) {
            const uint64_t sgi = base | (intid << 24);
            uint64_t h = 1023, ta, tb;
            __asm__ volatile("dsb ishst\nisb\nmrs %0, cntvct_el0\nisb" : "=r"(ta)::"memory");
            __asm__ volatile("msr icc_sgi1r_el1, %0\nisb\nmrs %1, cntvct_el0" : "=r"(tb) : "r"(sgi) : "memory");
            if (intid == 0) {
                out[8] = tb - ta;
            }
            for (unsigned polls = 0; polls < 400; polls++) {
                __asm__ volatile("mrs %0, icc_hppir1_el1" : "=r"(h));
                if ((h & 0xffffff) == intid) {
                    break;
                }
                __asm__ volatile("isb" ::: "memory");
            }
            if ((h & 0xffffff) == intid) {
                out[1] |= 1ULL << intid;
                __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(v));
                __asm__ volatile("msr icc_eoir1_el1, %0\nisb" ::"r"(v) : "memory");
                continue;
            }
            __asm__ volatile("dsb ishst\nmsr icc_sgi0r_el1, %0\nisb" ::"r"(sgi) : "memory");
            for (unsigned polls = 0; polls < 400; polls++) {
                __asm__ volatile("mrs %0, icc_hppir0_el1" : "=r"(h));
                if ((h & 0xffffff) == intid) {
                    break;
                }
                __asm__ volatile("isb" ::: "memory");
            }
            if ((h & 0xffffff) == intid) {
                out[2] |= 1ULL << intid;
                __asm__ volatile("mrs %0, icc_iar0_el1" : "=r"(v));
                __asm__ volatile("msr icc_eoir0_el1, %0\nisb" ::"r"(v) : "memory");
            }
        }
    }
    {
        uint64_t ta, tb;
        __asm__ volatile("isb\nmrs %0, cntvct_el0\nisb\nmrs %2, icc_pmr_el1\nisb\nmrs %1, cntvct_el0"
                         : "=r"(ta), "=r"(tb), "=r"(v)::"memory");
        out[9] = tb - ta;
    }
    __asm__ volatile("msr daif, %0\nisb" ::"r"(daif) : "memory");
}

extern "C" void armcpu_ipi_selftest(uint64_t aff)
{
    if (armcpu_gic_version == 2) {
        if ((aff & 0xff00ffffffULL) != gBootAffinity) {
            return;
        }
        __atomic_fetch_add(&armcpu_ipi_selftest_sent, UINT64_C(1), __ATOMIC_RELAXED);
        __asm__ volatile("dsb ishst" ::: "memory");
        write32(gDist, 0xf00, 2U << 24);
        __asm__ volatile("dsb sy\nisb" ::: "memory");
        return;
    }
    uint64_t sgi =
        ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
    __atomic_fetch_add(&armcpu_ipi_selftest_sent, UINT64_C(1), __ATOMIC_RELAXED);
    __asm__ volatile("dsb ishst\nmsr icc_sgi1r_el1, %0\nisb" ::"r"(sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase(bool enabled)
{
    uint64_t value = enabled ? 1 : 2;
    __asm__ volatile("msr cntv_ctl_el0, %0\nisb" ::"r"(value) : "memory");
}

// On the core going down, from cpu_sleep() (PE_cpu_machine_quiesce), interrupts off, on its interrupt stack. The
// boot CPU runs the platform quiesce actions first (SwitchSuspend: GPIO, pinmux and GIC distributor saved): the
// secondaries are already off by then. ml_arm_sleep() publishes the sleep token, stops the timebase and calls
// arm64_prepare_for_sleep(), which hands over to psciSleep() below; it does not return.
void ArmCpu::quiesceCPU(void)
{
    __atomic_fetch_add(&armcpu_quiesce_calls, 1U, __ATOMIC_RELAXED);
    if (!gSystemSleep || !gicV2) {
        panic("ArmCpu: CPU%u quiesce for system sleep, but SwitchSuspend did not enable SC7", logicalId);
    }
    if (!secondary) {
        switch_sleep_trail(SWITCH_STEP_QUIESCE);
        const kern_return_t result = IOCPURunPlatformQuiesceActions();
        if (result != KERN_SUCCESS) {
            panic("ArmCpu: platform quiesce actions failed (%d)", result);
        }
    }
    enableCPUTimeBase(false);
    write32(gV2CpuIf, 0, read32(gV2CpuIf, 0) & ~1U);
    __asm__ volatile("dsb sy\nisb" ::: "memory");
    if (secondary) {
        setCPUState(kIOCPUStateStopped);
    }
    ml_arm_sleep();
}

// IOCPUSleepKernel() (iokit/Kernel/IOCPU.cpp) calls this on the boot CPU's thread for each running secondary and
// then for the boot CPU itself. processor_exit() takes the processor out of the scheduler and runs cpu_sleep() on
// it (osfmk/kern/machine.c processor_offline_intstack). For a secondary it returns once that core is on its sleep
// path; for the boot CPU it returns after wake, as AppleARMSMP's IOCPUSleepKernel does.
void ArmCpu::haltCPU(void)
{
    __atomic_fetch_add(&armcpu_halt_calls, 1U, __ATOMIC_RELAXED);
    if (!gSystemSleep) {
        panic("ArmCpu: CPU%u halt requested, but SwitchSuspend did not enable SC7", logicalId);
    }
    if (secondary) {
        switch_sleep_trail(SWITCH_STEP_HALT_CPU1 + logicalId - 1);
    } else {
        // IOCPUSleepKernel has just stopped IOPMrootDomain's sleep watchdog: from here to the wake, a hang that does not
        // end in SC7 warm-resets instead. SC7 powers the timer down with the rest of the core.
        switch_sleep_trail(SWITCH_STEP_HALT_BOOT_CPU);
        switch_power_trail_watch(kSleepWatchSeconds, "SC7 entry");
    }
    const kern_return_t result = processor_exit(machProcessor);
    if (result != KERN_SUCCESS) {
        panic("ArmCpu: processor_exit for CPU%u failed (%d)", logicalId, result);
    }
}

// initCPU(false): the core is back from system sleep. The boot CPU comes through ATF's SYSTEM_SUSPEND return and
// XNU's reset path; a secondary through PSCI CPU_ON (startCPU on processor_start). The GIC distributor lost its
// state in SC7 (ATF re-initialized it): the boot CPU's platform active actions restore the SPIs (SwitchSuspend ->
// ArmGIC) before its interface is enabled. Each core re-sends itself the IPI that clears XNU's SIGPdisabled
// (osfmk/kern/machine.c processor_offline: "PE_cpu_machine_init() ... sends a self-IPI").
void ArmCpu::resumeCPU(void)
{
    if (!secondary)
        switch_sleep_trail(SWITCH_STEP_RESUME_CPU);
    if (!gSystemSleep || !gicV2) {
        panic("ArmCpu: CPU%u resumed, but system sleep is not enabled", logicalId);
    }
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    if ((mpidr & 0xff00ffffffULL) != (affinity & 0xff00ffffffULL)) {
        panic("ArmCpu: CPU%u resumed on MPIDR %#llx, expected %#llx", logicalId, mpidr, affinity);
    }
    configureBankedV2();
    if (!secondary) {
        switch_sleep_trail(SWITCH_STEP_RESUME_ACTIVE);
        const kern_return_t result = IOCPURunPlatformActiveActions();
        if (result != KERN_SUCCESS) {
            panic("ArmCpu: platform active actions failed (%d)", result);
        }
    } else {
        ml_install_interrupt_handler(this, static_cast<int>(logicalId), this,
                                     reinterpret_cast<IOInterruptHandler>(&ArmCpu::dispatch), nullptr);
    }
    enableGic();
    enableCPUTimeBase(true);
    setCPUState(kIOCPUStateRunning);
    signalCPU(this);
}

// arm64_prepare_for_sleep()'s handler (XNU diff), on the core going down, after ml_arm_sleep(). PSCI 1.0 (Arm DEN
// 0022D): CPU_OFF 0x84000002, AFFINITY_INFO 0xC4000004 (1 = OFF), SYSTEM_SUSPEND 0xC400000E. SYSTEM_SUSPEND is
// refused while another core is still on at the firmware, and ml_arm_sleep() only waits for their sleep tokens,
// which come just before their CPU_OFF: the boot CPU waits (bounded) for AFFINITY_INFO to report each one off.
// The boot CPU resumes at switch_secondary_entry (EL2 -> EL1, then XNU's reset vector, start.s) with record 0.
static void cleanToMemory(const void *start, size_t length);

static void psciSleep(boolean_t)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    const uint32_t cpu = static_cast<uint32_t>(mpidr & 0xff);
    if (cpu != 0) {
        const int64_t rc = psciCall(false, 0x84000002, 0, 0, 0);
        panic("ArmCpu: PSCI CPU_OFF on CPU%u returned %lld", cpu, rc);
    }
    switch_sleep_trail(SWITCH_STEP_PSCI_PEERS_OFF);
    for (uint64_t peer = 1; gNativeSmp && peer < SWITCH_SMP_CORE_COUNT; ++peer) {
        const uint64_t started = counter();
        int64_t state;
        while ((state = psciCall(false, 0xc4000004, peer, 0, 0)) != 1) {
            if (counter() - started > UINT64_C(19200000) / 10) {
                panic("ArmCpu: CPU%llu still on at the firmware 100 ms after its sleep token (AFFINITY_INFO %lld)",
                      peer, state);
            }
        }
    }
    armcpu::fillBootRecord();
#if DEVELOPMENT || DEBUG
    // ATF's SYSTEM_SUSPEND returns at once, without suspending, while an interrupt is pending here (psci_suspend.c).
    uint64_t pending;
    __asm__ volatile("mrs %0, isr_el1" : "=r"(pending));
    switch_power_trail("isr_el1 %#llx", pending);
#endif
    switch_power_trail_mark_now(SWITCH_STEP_SYSTEM_SUSPEND);
    const int64_t rc = psciCall(false, 0xc400000e, gSleepEntryPA, gSleepRecordPA, 0);
    panic("ArmCpu: PSCI SYSTEM_SUSPEND returned %lld", rc);
}

#if DEVELOPMENT || DEBUG
// The boot CPU back from SC7, the first C the kernel runs there (arm_init_cpu): interrupts off, the timebase not yet
// rebased.
static void sc7Exit(void)
{
    // The mark first: I2C5's raw write re-clocks its controller, while the timer block's state after the warm boot is
    // unknown, and a stall in the watchdog's write must not leave the mark at SYSTEM_SUSPEND.
    switch_power_trail_mark_now(SWITCH_STEP_RESUME);
    switch_power_trail_watch(kSleepWatchSeconds, "SC7 exit");
}
#endif

// The boot CPU's PSCI entry record, which only system sleep and C7 use (secondaries get theirs at CPU_ON).
void armcpu::fillBootRecord(void)
{
    switch_smp_record *record = &switch_secondary_records[0];
    memset(record, 0, sizeof(*record));
    record->magic = SWITCH_SMP_MAGIC;
    record->mpidr = 0;
    record->logical = 0;
    record->cntfrq = UINT64_C(19200000);
    record->cntvoff = 0;
    record->reset_pa = ml_static_vtop(reinterpret_cast<vm_offset_t>(LowResetVectorBase));
    cleanToMemory(record, sizeof(*record));
}

int64_t armcpu::psciSmc(uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
    return psciCall(false, function, a, b, c);
}

bool armcpu::psciSmcConduit(void)
{
    bool hvc = false;
    return psciConduit(&hvc) && !hvc;
}

bool armcpu::nativeGicV2Smp(void)
{
    return gSharedGicV2 && gNativeSmp && armcpu_gic_ready &&
           __atomic_load_n(&armcpu_smp_running, __ATOMIC_ACQUIRE) == SWITCH_SMP_CORE_COUNT - 1;
}

uintptr_t armcpu::gicDistributor(void)
{
    return gDist;
}

// SwitchSuspend's start: system sleep is possible when native GICv2 SMP (or the boot CPU alone) runs on switchroot
// ATF's PSCI with CPU_OFF, AFFINITY_INFO and SYSTEM_SUSPEND, and the kernel has the sleep hook. Returns 0 and turns
// sleep on, or -1 with the reason in `why`.
extern "C" int armcpu_system_sleep_enable(char *why, size_t size)
{
    const char *reason = nullptr;
    bool hvc = false;
    int64_t rc = 0;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!switch_cpu_sleep_hook_present) {
        reason = "the kernel does not call switch_cpu_sleep_handler (XNU sleep diff)";
    } else if (armcpu_gic_version != 2 || !armcpu_gic_ready) {
        reason = "no GICv2 up";
    } else if (ml_get_max_cpu_number() != 0 && !gNativeSmp) {
        reason = "secondary cores without native SMP";
    }
    // A core parked after a failed start stays on at the firmware, and SYSTEM_SUSPEND would wait for it forever.
    else if (gNativeSmp && __atomic_load_n(&armcpu_smp_running, __ATOMIC_ACQUIRE) != SWITCH_SMP_CORE_COUNT - 1) {
        reason = "not every secondary core is running";
    } else if (!psciConduit(&hvc) || hvc) {
        reason = "no PSCI SMC conduit in /psci";
    } else if ((static_cast<uint64_t>(psciCall(false, 0x84000000, 0, 0, 0)) >> 16) < 1) {
        reason = "PSCI older than 1.0";
    } else if ((rc = psciCall(false, 0x8400000a, 0xc400000e, 0, 0)) != 0) {
        reason = "PSCI_FEATURES: no SYSTEM_SUSPEND";
    } else if ((rc = psciCall(false, 0x8400000a, 0x84000002, 0, 0)) != 0) {
        reason = "PSCI_FEATURES: no CPU_OFF";
    } else if ((rc = psciCall(false, 0x8400000a, 0xc4000004, 0, 0)) != 0) {
        reason = "PSCI_FEATURES: no AFFINITY_INFO";
    } else if (frequency != UINT64_C(19200000)) {
        reason = "CNTFRQ is not 19.2 MHz";
    }
    if (!reason) {
        gSleepEntryPA = ml_static_vtop(reinterpret_cast<vm_offset_t>(switch_secondary_entry));
        gSleepRecordPA = ml_static_vtop(reinterpret_cast<vm_offset_t>(&switch_secondary_records[0]));
        if (!gSleepEntryPA || gSleepEntryPA >= UINT64_C(0x100000000) || !gSleepRecordPA || (gSleepRecordPA & 63) ||
            ml_vtophys(reinterpret_cast<vm_offset_t>(&switch_secondary_records[0])) != gSleepRecordPA) {
            reason = "the PSCI entry or record 0 is not a usable physical address";
        }
    }
    if (reason) {
        snprintf(why, size, "%s (PSCI rc %lld)", reason, rc);
        return -1;
    }
    gSystemSleep = true;
    switch_cpu_sleep_handler = psciSleep;
#if DEVELOPMENT || DEBUG
    switch_cpu_wake_handler = sc7Exit;
#endif
    return 0;
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory(const void *start, size_t length)
{
    switch_dcache_clean(start, length);
    __asm__ volatile("isb" ::: "memory");
}

static bool cleanKernelSegments(void)
{
    const kernel_mach_header_t *header = &_mh_execute_header;
    if (header->magic != MH_MAGIC_KERNEL || header->ncmds > 128 || header->sizeofcmds > 65536) {
        return false;
    }
    const uint8_t *command = reinterpret_cast<const uint8_t *>(header + 1);
    const uint8_t *end = command + header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (static_cast<size_t>(end - command) < sizeof(load_command)) {
            return false;
        }
        const load_command *load = reinterpret_cast<const load_command *>(command);
        if (load->cmdsize < sizeof(*load) || load->cmdsize > static_cast<size_t>(end - command)) {
            return false;
        }
        if (load->cmd == LC_SEGMENT_KERNEL) {
            if (load->cmdsize < sizeof(kernel_segment_command_t)) {
                return false;
            }
            const kernel_segment_command_t *seg = reinterpret_cast<const kernel_segment_command_t *>(command);
            if (seg->vmsize > UINTPTR_MAX - seg->vmaddr || seg->vmsize > (UINT64_C(1) << 30) ||
                seg->vmaddr + seg->vmsize > UINTPTR_MAX - SWITCH_HW_PAGE_MASK) {
                return false;
            }
            if (seg->initprot & VM_PROT_READ) {
                // Discarded KLD/LINKEDIT pages can be unmapped by this point. Clean
                // only currently mapped RAM pages, never an MMIO alias or a hole.
                for (uintptr_t va = seg->vmaddr & ~SWITCH_HW_PAGE_MASK; va < seg->vmaddr + seg->vmsize;
                     va += SWITCH_HW_PAGE_SIZE) {
                    const uint64_t pa = ml_vtophys(va);
                    if (pa && switch_dma_pa_managed(pa)) {
                        switch_dcache_clean_lines(reinterpret_cast<const void *>(va), SWITCH_HW_PAGE_SIZE);
                    }
                }
            }
        }
        command += load->cmdsize;
    }
    // One barrier completes every page's clean (and anything written before), ahead of the core that reads them.
    switch_dcache_sync();
    __asm__ volatile("isb" ::: "memory");
    return command == end;
}

kern_return_t ArmCpu::startCPU(vm_offset_t, vm_offset_t)
{
    if (!secondary) {
        return KERN_NOT_SUPPORTED;
    }
    bool hvc = false;
    if (!psciConduit(&hvc)) {
        armcpu_smp_psci_rc[logicalId] = -1001;
        return KERN_NOT_SUPPORTED;
    }
    __atomic_store_n(&armcpu_smp_conduit, hvc ? 2U : 1U, __ATOMIC_RELAXED);
    // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
    // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
    uint64_t entry = static_cast<uint64_t>(ml_static_vtop(reinterpret_cast<vm_offset_t>(LowResetVectorBase)));
    uint64_t context = 0;
    // After system sleep the same CPU_ON brings the core back (IOCPUSleepKernel -> processor_start); only a core
    // that sleep stopped may be started a second time.
    const bool resume = gSystemSleep && getCPUState() == kIOCPUStateStopped;
    if (gicV2) {
        if (!gNativeSmp || logicalId >= SWITCH_SMP_CORE_COUNT || affinity != logicalId ||
            (__atomic_exchange_n(&gCpuOnAttempted[logicalId], 1U, __ATOMIC_ACQ_REL) && !resume) ||
            !qualifyNativePsci(affinity)) {
            armcpu_smp_psci_rc[logicalId] = -1002;
            return KERN_NOT_SUPPORTED;
        }
        uint64_t frequency;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        switch_smp_record *record = &switch_secondary_records[logicalId];
        context = ml_static_vtop(reinterpret_cast<vm_offset_t>(record));
        const uint64_t reset = entry;
        entry = ml_static_vtop(reinterpret_cast<vm_offset_t>(switch_secondary_entry));
        if (frequency != UINT64_C(19200000) || !entry || entry >= UINT64_C(0x100000000) || !reset ||
            reset >= UINT64_C(0x100000000) || !context || (context & 63) ||
            context > UINT64_C(0x100000000) - sizeof(*record) ||
            ml_vtophys(reinterpret_cast<vm_offset_t>(record)) != context ||
            ml_vtophys(reinterpret_cast<vm_offset_t>(record) + sizeof(*record) - 1) != context + sizeof(*record) - 1) {
            armcpu_smp_psci_rc[logicalId] = -1003;
            return KERN_FAILURE;
        }
        memset(record, 0, sizeof(*record));
        record->magic = SWITCH_SMP_MAGIC;
        record->mpidr = affinity;
        record->logical = logicalId;
        record->cntfrq = frequency;
        record->cntvoff = 0;
        record->reset_pa = reset;
        if (!cleanKernelSegments() || switch_smp_publish_cpu_state()) {
            armcpu_smp_psci_rc[logicalId] = -1004;
            return KERN_FAILURE;
        }
        cleanToMemory(record, sizeof(*record));
    } else {
        // The generic GICv3 path. Native GICv2 publishes the actual Mach segments and kernel-owned CPU/table closure
        // above.
        cleanToMemory(LowResetVectorBase, 0x10000);
        cleanToMemory(ResetHandlerData, 0x1000);
        cleanToMemory(const_boot_args, 0x1000);
    }
    __atomic_store_n(&armcpu_smp_step[logicalId], 4U, __ATOMIC_RELEASE);
    int64_t result = psciCall(hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, context);
    armcpu_smp_psci_rc[logicalId] = static_cast<int32_t>(result);
    return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName(void)
{
    return cpuName;
}
