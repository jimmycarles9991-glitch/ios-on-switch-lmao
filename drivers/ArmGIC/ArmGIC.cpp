// SPDX-License-Identifier: BSD-2-Clause-Patent
// GICv2 shared peripheral interrupts (INTID 32..1019) as an IOInterruptController, so Switch drivers use the
// ordinary IOInterruptEventSource / IOFilterInterruptEventSource on their own work loops (SwitchInterrupts.h).
//
// Division of the GIC (Arm IHI 0048B): ArmCpu brings up the distributor and the CPU interfaces, disables every
// SPI at boot, owns SGIs and PPIs, and is the only code that touches GICC: it acknowledges each interrupt
// (GICC_IAR), hands SPIs to switch_gic_dispatch() and ends them (GICC_EOIR, EOImode 0). This file only
// writes per-SPI distributor state: ISENABLER/ICENABLER, ISPENDR/ICPENDR, IPRIORITYR, ITARGETSR and ICFGR.
//
// Vector model (XNU iokit/Kernel/IOInterruptController.cpp): the vector number is the INTID, and a nub's
// interrupt specifier is that INTID. A registered vector starts hard disabled; enableInterrupt() unmasks it at
// the distributor. disableInterrupt() only marks it soft disabled; the line is masked at the distributor (hard
// disabled) when it fires while soft disabled, or here as soon as its handler soft-disabled it. That is how
// IOInterruptEventSource runs a level line: its primary handler disables the interrupt, the work-loop action
// clears the device and re-enables it, and the line cannot fire again between ArmCpu's EOI and that action.
//
// Tegra210 also has the legacy interrupt controller (LIC) in front of the GIC in its device tree; Linux
// (drivers/irqchip/irq-tegra.c) unmasks a line in both. It is most likely only the flow controller's wake path
// (Atmosphere's Horizon kernel never enables it and still gets SDMMC1 on INTID 46), but enabling it as Linux
// does is harmless either way, so this controller does, on Tegra210 only (GICD at 0x50041000).
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitDebug.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <libkern/OSAtomic.h>
#include <string.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include "ArmGIC.h"
#include "SwitchInterrupts.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

// ArmCpu's GIC state (drivers/ArmCpu/ArmCpu.cpp): version, distributor/CPU interface enabled, and each logical
// CPU's GICv2 target mask (its GICD_ITARGETSR0 byte), written once that CPU's interface is up.
extern "C" volatile uint32_t armcpu_gic_version, armcpu_gic_ready, armcpu_gic_targets[8];
extern "C" void armcpu_gic_cpu_describe(char *buffer, size_t size);

namespace {
// Distributor registers, byte offsets (IHI 0048B table 4-1).
enum : uint32_t {
    GICD_TYPER = 0x004,
    GICD_ISENABLER = 0x100,
    GICD_ICENABLER = 0x180,
    GICD_ISPENDR = 0x200,
    GICD_ICPENDR = 0x280,
    GICD_ISACTIVER = 0x300,
    GICD_IPRIORITYR = 0x400,
    GICD_ITARGETSR = 0x800,
    GICD_ICFGR = 0xc00,
    GICD_PIDR2 = 0xfe8,
};

constexpr uint32_t kFirstSpi = 32, kMaxIntid = 1020;
constexpr uint8_t kSpiPriority = 0xa0;  // below ArmCpu's timer PPI and IPI SGI (0x80)
// Priorities around it (GIC-400 keeps the top five bits; GICC_PMR is 0xff). XNU runs every handler with IRQs masked
// and never nests them, so a priority only decides which of the SPIs pending at one CPU interface ArmCpu takes next
// from GICC_IAR; a tie goes to the lowest INTID.
constexpr uint8_t kFramePriority = 0x90;   // a late one is a late frame: above all other SPIs
constexpr uint8_t kSerialPriority = 0xc0;  // high-rate UARTs behind hardware flow control: below all other SPIs
// The SPIs a frame waits on: display controller A (frame end, vblank; NativeLegacyDisplay, TegraPanel), the GM20B
// stall/nonstall lines (fences and the copy-engine present; TegraGM20B) and host1x's syncpoint line, which ends every
// VIC job (Core Animation's IOSurfaceAccelerator copies, NVDEC -> VIC video frames; TegraHost1x).
constexpr uint32_t kDisplayAIntid = 32 + 73, kGpuStallIntid = 32 + 157, kGpuNonstallIntid = 32 + 158,
                   kHost1xSyncptIntid = 32 + 65;

// SPIs with a secondary core of their own, so CPU0 keeps its timers, I2C, GPIO and UARTs (touch, buttons, Joy-Cons,
// Bluetooth) to itself: storage and Wi-Fi on CPU1, USB on CPU2; the frame lines take the last core (frameTarget).
// Each driver's filter only meets its own threads, which run on any core anyway. Every sleep action runs with the
// secondaries up and every wake action with all cores back (SwitchSuspend.h), so these lines keep their core across
// SC7 as the frame lines do.
struct SteeredSpi {
    uint32_t intid, cpu;
};

constexpr SteeredSpi kSteered[] = {
    {32 + 14, 1},  // SDMMC1, the SD card (NativeSwitchBlock)
    {131, 1},      // the PCIe controller's MSI, Wi-Fi (SwitchWLAN)
    {32 + 44, 2},  // XUDC (SwitchXUDC)
    {32 + 39, 2},  // XUSB host (SwitchXHCI)
    {32 + 40, 2},  // XUSB mailbox (SwitchXHCI)
    // The C7 wake timers (ArmCpuIdle.cpp) on the core each one wakes; CPU0's TMR10 stays on CPU0.
    {32 + 177, 1},  // TMR11
    {32 + 178, 2},  // TMR12
    {32 + 179, 3},  // TMR13
};

// Tegra210 legacy interrupt controller: one bank of 32 SPIs per 0x100, six banks (mainline tegra210.dtsi
// interrupt-controller@60004000, Linux irq-tegra.c register names).
constexpr uint64_t kTegra210Distributor = 0x50041000, kTegra210Lic = 0x60004000;
constexpr uint32_t kLicBanks = 6;

enum : uint32_t {
    ICTLR_CPU_IER = 0x20,
    ICTLR_CPU_IER_SET = 0x24,
    ICTLR_CPU_IER_CLR = 0x28,
    ICTLR_CPU_IEP_CLASS = 0x2c
};

const char kControllerName[] = "SwitchGIC";
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class ArmGIC : public IOInterruptController {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(ArmGIC)

        IOMemoryMap *distMap = nullptr,
                    *licMap = nullptr;
    volatile uint8_t *dist = nullptr, *lic = nullptr;
    uint32_t intids = 0;     // INTIDs the distributor implements: 32 * (GICD_TYPER.ITLinesNumber + 1), < 1020
    uint8_t cpu0Target = 0;  // CPU interface mask of logical CPU 0
    bool steering = false;   // frame and kSteered SPIs go to their secondary core
    uint32_t steerPending[kMaxIntid / 32 + 1] = {};  // steered SPIs made before their core took SPIs
    uint32_t edge[kMaxIntid / 32 + 1] = {};          // trigger of each INTID, fixed when its nub is made
    uint32_t *fired = nullptr;                       // handled interrupts per INTID, for panic messages
    IOService **nubs = nullptr;
    IOLock *nubLock = nullptr;
    const OSSymbol *name = nullptr;
    // System sleep: the SPI state the distributor and the LIC lose in SC7 (ATF re-initializes the GIC on resume).
    uint32_t savedEnable[kMaxIntid / 32] = {}, savedConfig[kMaxIntid / 16] = {};
    uint32_t savedPriority[kMaxIntid / 4] = {}, savedTarget[kMaxIntid / 4] = {};
    uint32_t savedLicEnable[kLicBanks] = {}, savedLicClass[kLicBanks] = {};

    uint32_t read32(uint32_t offset) const
    {
        return switch_mmio_read32(dist, offset);
    }

    uint8_t read8(uint32_t offset) const
    {
        return switch_mmio_read8(dist, offset);
    }

    bool readBit(uint32_t bank, uint32_t intid) const
    {
        return ((read32(bank + (intid / 32) * 4) >> (intid % 32)) & 1) != 0u;
    }

    void write32(uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(dist, offset, value);
    }

    void write8(uint32_t offset, uint8_t value) const
    {
        switch_mmio_write8(dist, offset, value);
    }

    // One bit per INTID in a W1S/W1C register bank (ISENABLER, ICENABLER, ISPENDR, ICPENDR).
    void writeBit(uint32_t bank, uint32_t intid)
    {
        write32(bank + (intid / 32) * 4, 1U << (intid % 32));
    }

    bool licCovers(uint32_t intid) const
    {
        return (lic != nullptr) && licBank(intid) < kLicBanks;
    }

    // The LIC's register `offset` in bank `bank` (32 SPIs from kFirstSpi each).
    uint32_t licRead(uint32_t bank, uint32_t offset) const
    {
        return switch_mmio_read32(lic, bank * 0x100 + offset);
    }

    void licWrite(uint32_t bank, uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(lic, bank * 0x100 + offset, value);
    }

    static uint32_t licBank(uint32_t intid)
    {
        return (intid - kFirstSpi) / 32;
    }

    bool isEdge(uint32_t intid) const
    {
        return ((edge[intid / 32] >> (intid % 32)) & 1) != 0u;
    }

    static bool isFrame(uint32_t intid)
    {
        return intid == kDisplayAIntid || intid == kGpuStallIntid || intid == kGpuNonstallIntid ||
               intid == kHost1xSyncptIntid;
    }

    // GICv2 breaks a priority tie by lowest INTID, so with one priority for every SPI the GPU fence lines (189/190)
    // lose CPU0 to any lower-numbered SPI also pending: during A2DP pairing UART-D (122) kept the GPU fence pending
    // for over a second and backboardd's waitFence panicked.
    static uint8_t priorityFor(uint32_t intid)
    {
        if (isFrame(intid))
            return kFramePriority;
        switch (intid) {
        case 32 + 37:  // UART-B, Joy-Con (R) rail (SwitchJoyCon)
        case 32 + 46:  // UART-C, Joy-Con (L) rail
        case 32 + 90:  // UART-D, Bluetooth (SwitchBluetooth, BCM4356A3)
            return kSerialPriority;
        default:
            return kSpiPriority;
        }
    }

    // The CPU interface of the highest-numbered core whose interface ArmCpu has enabled (it publishes the core's
    // GICD_ITARGETSR0 byte only then), or 0 if no secondary core takes interrupts yet. ArmCpu dispatches an SPI on
    // whichever core takes it, and a secondary never goes offline unless system sleep is on (ArmCpu::haltCPU).
    static uint8_t frameTarget()
    {
        for (uint32_t cpu = 7; cpu > 0; --cpu) {
            const uint32_t target = __atomic_load_n(&armcpu_gic_targets[cpu], __ATOMIC_ACQUIRE);
            if (target && target <= 0xff && !(target & (target - 1)))
                return static_cast<uint8_t>(target);
        }
        return 0;
    }

    static uint32_t steeredCpu(uint32_t intid)
    {
        for (const SteeredSpi &spi : kSteered) {
            if (spi.intid == intid)
                return spi.cpu;
        }
        return 0;
    }

    // The CPU interface of logical CPU `cpu` once ArmCpu has enabled it, else 0.
    static uint8_t cpuTarget(uint32_t cpu)
    {
        const uint32_t target = __atomic_load_n(&armcpu_gic_targets[cpu], __ATOMIC_ACQUIRE);
        return target && target <= 0xff && !(target & (target - 1)) ? static_cast<uint8_t>(target) : 0;
    }

    static uint8_t steerTarget(uint32_t intid)
    {
        if (isFrame(intid))
            return frameTarget();
        const uint32_t cpu = steeredCpu(intid);
        return cpu ? cpuTarget(cpu) : 0;
    }

    // The frame lines and kSteered get a core of their own: there the frame lines never wait behind CPU0's storage,
    // Wi-Fi, Bluetooth, Joy-Con, touch and USB handlers or behind code running on CPU0 with interrupts masked. A
    // steered SPI made before its core is up starts on CPU0 and moves when that core comes up (cpuOnline) or at its
    // next enable (steerPending).
    uint8_t targetFor(uint32_t intid)
    {
        if (!steering || (!isFrame(intid) && !steeredCpu(intid)))
            return cpu0Target;
        if (const uint8_t target = steerTarget(intid))
            return target;
        __atomic_fetch_or(&steerPending[intid / 32], 1U << (intid % 32), __ATOMIC_RELAXED);
        return cpu0Target;
    }

    bool mapDistributor(uint64_t *physical);
    const char *setup();
    IOService *makeNub(uint32_t intid);

public:
    void free() override;
    static LIBKERN_RETURNS_NOT_RETAINED ArmGIC *shared();
    void suspend();
    void resume();
    void cpuOnline(uint32_t cpu);
    IOService *nubFor(uint32_t intid, bool edgeTriggered);
    void describe(uint32_t intid, char *buffer, size_t size) const;

    uint32_t count(uint32_t intid) const
    {
        return intid >= kFirstSpi && intid < intids ? __atomic_load_n(&fired[intid], __ATOMIC_RELAXED) : 0;
    }

    IOReturn handleInterrupt(void *refCon, IOService *nub, int source) override;

    bool vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *) override
    {
        return false;
    }

    void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    int getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
    void causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) override;
};

OSDefineMetaClassAndStructors(ArmGIC, IOInterruptController)

    namespace
{
    ArmGIC *volatile gGic = nullptr;
    volatile uint32_t gGicState = 0;  // 0 not made yet, 1 being made, 2 up (a failed setup panics)
}

// Made by the first driver that asks for an interrupt, once ArmCpu has the GICv2 and CPU0's interface up.
ArmGIC *ArmGIC::shared()
{
    for (;;) {
        uint32_t state = __atomic_load_n(&gGicState, __ATOMIC_ACQUIRE);
        if (state == 2)
            return gGic;
        if (state == 1) {
            IOSleep(1);
            continue;
        }
        if (armcpu_gic_version != 2 || !armcpu_gic_ready || !armcpu_gic_targets[0])
            return nullptr;
        if (!__atomic_compare_exchange_n(&gGicState, &state, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        ArmGIC *gic = OSTypeAlloc(ArmGIC);
        const char *failure = gic ? gic->setup() : "out of memory";
        if (failure)
            panic("ArmGIC: controller setup failed: %s", failure);
        __atomic_store_n(&gGic, gic, __ATOMIC_RELEASE);
        __atomic_store_n(&gGicState, 2U, __ATOMIC_RELEASE);
        return gic;
    }
}

// The distributor frame from the ADT, as ArmCpu::mapGic finds it: /arm-io/intc reg = {distributor, size,
// CPU interface, size} in /arm-io bus addresses, /arm-io ranges = {bus base, physical base, size}.
bool ArmGIC::mapDistributor(uint64_t *physical)
{
    const IORegistryEntry *soc = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    const IORegistryEntry *gic = IORegistryEntry::fromPath("/arm-io/intc", gIODTPlane);
    uint64_t bus[3] = {};
    uint64_t regions[4] = {};
    const bool found = switch_dt_value(soc, "ranges", &bus) && switch_dt_value(gic, "reg", &regions);
    OSSafeReleaseNULL(soc);
    OSSafeReleaseNULL(gic);
    if (!found || regions[1] < 0x1000 || regions[0] < bus[0] || bus[2] < 0x1000 ||
        regions[0] - bus[0] > bus[2] - 0x1000) {
        return false;
    }
    *physical = bus[1] + (regions[0] - bus[0]);
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(*physical, 0x1000, kIODirectionInOut);
    distMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!distMap)
        return false;
    dist = reinterpret_cast<volatile uint8_t *>(distMap->getVirtualAddress());
    return ((read32(GICD_PIDR2) >> 4) & 15) == 2;
}

// nullptr when the controller is up, else why not.
const char *ArmGIC::setup()
{
    uint64_t physical = 0;
    if (!init())
        return "init";
    if (!mapDistributor(&physical))
        return "no GICv2 distributor at /arm-io/intc";
    const uint32_t typer = read32(GICD_TYPER);
    intids = ((typer & 31) + 1) * 32;
    if (intids > kMaxIntid)
        intids = kMaxIntid;
    cpu0Target = static_cast<uint8_t>(armcpu_gic_targets[0]);
    // Steered lines stay on their core across system sleep: their drivers suspend in sleep actions, before
    // IOCPUSleepKernel stops any core, and resume in wake actions, after every core is back; a line that pends
    // while its core is off waits in the distributor.
    steering = true;
    vectors = IONewZero(IOInterruptVector, intids);
    nubs = IONewZero(IOService *, intids);
    fired = IONewZero(uint32_t, intids);
    nubLock = IOLockAlloc();
    controllerLock = IOSimpleLockAlloc();
    name = OSSymbol::withCStringNoCopy(kControllerName);
    if (!vectors || !nubs || !fired || !nubLock || !controllerLock || !name)
        return "out of memory";
    // IOInterruptController::registerInterrupt takes the vector lock of every vector a nub names.
    for (uint32_t intid = kFirstSpi; intid < intids; ++intid) {
        vectors[intid].interruptLock = IOLockAlloc();
        if (!vectors[intid].interruptLock)
            return "out of memory";
    }
    if (physical == kTegra210Distributor) {
        IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(kTegra210Lic, 0x1000, kIODirectionInOut);
        licMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!licMap)
            return "cannot map the Tegra legacy interrupt controller";
        lic = reinterpret_cast<volatile uint8_t *>(licMap->getVirtualAddress());
    }
    IOPlatformExpert *platform = getPlatform();
    if (!platform || !attach(platform))
        return "cannot attach to the platform expert";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): registerInterruptController takes a non-const symbol
    platform->registerInterruptController(const_cast<OSSymbol *>(name), this);
    return nullptr;
}

void ArmGIC::free()
{
    if (vectors) {
        for (uint32_t intid = 0; intid < intids; ++intid) {
            if (vectors[intid].interruptLock)
                IOLockFree(vectors[intid].interruptLock);
        }
        IODelete(vectors, IOInterruptVector, intids);
    }
    if (nubs)
        IODelete(nubs, IOService *, intids);
    if (fired)
        IODelete(fired, uint32_t, intids);
    if (nubLock)
        IOLockFree(nubLock);
    if (controllerLock)
        IOSimpleLockFree(controllerLock);
    OSSafeReleaseNULL(name);
    OSSafeReleaseNULL(licMap);
    OSSafeReleaseNULL(distMap);
    IOInterruptController::free();
}

// An IOService under the controller that carries the two properties IOService::resolveInterrupt reads: its
// interrupt 0 is this controller's vector `intid`.
IOService *ArmGIC::makeNub(uint32_t intid)
{
    const IOInterruptVectorNumber number = static_cast<IOInterruptVectorNumber>(intid);
    const OSObject *controllerNames[] = {name};
    OSArray *controllers = OSArray::withObjects(controllerNames, 1);
    const OSData *specifier = OSData::withBytes(&number, sizeof(number));
    const OSObject *specifierList[] = {specifier};
    OSArray *specifiers = specifier ? OSArray::withObjects(specifierList, 1) : nullptr;
    IOService *nub = OSTypeAlloc(IOService);
    bool made = (controllers != nullptr) && (specifiers != nullptr) && (nub != nullptr) && nub->init() &&
                nub->setProperty(gIOInterruptControllersKey, controllers) &&
                nub->setProperty(gIOInterruptSpecifiersKey, specifiers);
    if (made) {
        char label[16];
        snprintf(label, sizeof(label), "irq%u", intid);
        nub->setName(label);
        made = nub->attach(this);
    }
    OSSafeReleaseNULL(controllers);
    OSSafeReleaseNULL(specifier);
    OSSafeReleaseNULL(specifiers);
    if (!made)
        OSSafeReleaseNULL(nub);
    return nub;
}

IOService *ArmGIC::nubFor(uint32_t intid, bool edgeTriggered)
{
    if (intid < kFirstSpi || intid >= intids)
        panic("ArmGIC: INTID %u is not an SPI (32..%u)", intid, intids - 1);
    IOLockLock(nubLock);
    IOService *nub = nubs[intid];
    if (nub && isEdge(intid) != edgeTriggered) {
        panic("ArmGIC: INTID %u asked for as %s-triggered, already %s-triggered", intid,
              edgeTriggered ? "edge" : "level", edgeTriggered ? "level" : "edge");
    }
    if (!nub) {
        // The trigger is recorded before the nub exists, so getVectorType() already knows it at registration.
        if (edgeTriggered) {
            edge[intid / 32] |= 1U << (intid % 32);
        } else {
            edge[intid / 32] &= ~(1U << (intid % 32));
        }
        nub = makeNub(intid);
        nubs[intid] = nub;  // keeps the creation reference: nubs live as long as the controller
        if (!nub)
            panic("ArmGIC: cannot make the nub for INTID %u", intid);
    }
    IOLockUnlock(nubLock);
    return nub;
}

// Primary interrupt context, any core, from ArmCpu after GICC_IAR; ArmCpu writes GICC_EOIR afterwards.
IOReturn ArmGIC::handleInterrupt(void *, IOService *, int source)
{
    const uint32_t intid = static_cast<uint32_t>(source);
    if (intid < kFirstSpi || intid >= intids)
        return kIOReturnNoInterrupt;
    IOInterruptVector *vector = &vectors[intid];
    if (!vector->interruptRegistered)
        return kIOReturnNoInterrupt;
    // An INTID's handler never runs on two cores at once (the GIC holds it active until the EOI), so a load and a
    // store count it, without an exclusive-monitor loop.
    __atomic_store_n(&fired[intid], __atomic_load_n(&fired[intid], __ATOMIC_RELAXED) + 1U, __ATOMIC_RELAXED);
    if (isFrame(intid))
        switch_idle_note_frame();
    vector->interruptActive = 1;
    OSMemoryBarrier();
    if (!vector->interruptDisabledSoft) {
        const bool trace = (static_cast<uint64_t>(gIOKitTrace) & kIOTraceInterrupts) != 0;
        if (trace)
            timeStampInterruptHandlerStart(static_cast<IOInterruptVectorNumber>(intid), vector);
        vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
        if (trace)
            timeStampInterruptHandlerEnd(static_cast<IOInterruptVectorNumber>(intid), vector);
    }
    // Soft disabled before it fired, or by its own handler (a level IOInterruptEventSource): mask it now, before
    // the EOI, rather than taking a still-asserted line once more to find that out. enableInterrupt() undoes it.
    if (vector->interruptDisabledSoft && !vector->interruptDisabledHard) {
        vector->interruptDisabledHard = 1;
        disableVectorHard(static_cast<IOInterruptVectorNumber>(intid), vector);
    }
    OSMemoryBarrier();
    vector->interruptActive = 0;
    return kIOReturnSuccess;
}

// Under the vector lock at registration; the vector is still hard disabled.
void ArmGIC::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    const uint32_t intid = static_cast<uint32_t>(vectorNumber);
    writeBit(GICD_ICENABLER, intid);  // ICFGR may only change while the SPI is disabled (IHI 0048B 4.3.13)
    writeBit(GICD_ICPENDR, intid);    // drop whatever latched while nobody owned it
    write8(GICD_IPRIORITYR + intid, priorityFor(intid));
    const uint8_t target = targetFor(intid);
    write8(GICD_ITARGETSR + intid, target);
    const uint32_t config = GICD_ICFGR + (intid / 16) * 4;
    const uint32_t edgeBit = 2U << ((intid % 16) * 2);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(controllerLock);
    write32(config, isEdge(intid) ? (read32(config) | edgeBit) : (read32(config) & ~edgeBit));
    if (licCovers(intid)) {
        const uint32_t bank = licBank(intid);
        licWrite(bank, ICTLR_CPU_IEP_CLASS,
                 licRead(bank, ICTLR_CPU_IEP_CLASS) & ~(1U << (intid % 32)));  // IRQ, not FIQ
    }
    IOSimpleLockUnlockEnableInterrupt(controllerLock, state);
    OSSynchronizeIO();
}

int ArmGIC::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    return isEdge(static_cast<uint32_t>(vectorNumber)) ? kIOInterruptTypeEdge : kIOInterruptTypeLevel;
}

void ArmGIC::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    const uint32_t intid = static_cast<uint32_t>(vectorNumber);
    writeBit(GICD_ICENABLER, intid);
    if (licCovers(intid))
        licWrite(licBank(intid), ICTLR_CPU_IER_CLR, 1U << (intid % 32));
    OSSynchronizeIO();
}

void ArmGIC::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    const uint32_t intid = static_cast<uint32_t>(vectorNumber);
    // A steered SPI made before its core took interrupts: move it now, while it is still masked.
    const uint32_t bit = 1U << (intid % 32);
    if (__improbable(__atomic_load_n(&steerPending[intid / 32], __ATOMIC_RELAXED) & bit)) {
        if (const uint8_t target = steerTarget(intid)) {
            write8(GICD_ITARGETSR + intid, target);
            __atomic_fetch_and(&steerPending[intid / 32], ~bit, __ATOMIC_RELAXED);
        }
    }
    if (licCovers(intid))
        licWrite(licBank(intid), ICTLR_CPU_IER_SET, 1U << (intid % 32));
    writeBit(GICD_ISENABLER, intid);
    OSSynchronizeIO();
}

void ArmGIC::causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
    writeBit(GICD_ISPENDR, static_cast<uint32_t>(vectorNumber));
    OSSynchronizeIO();
}

// Distributor state of one SPI, the Tegra LIC gate in front of it, the lowest SPI active anywhere and the calling
// core's CPU interface, for a driver's panic message.
void ArmGIC::describe(uint32_t intid, char *buffer, size_t size) const
{
    if (intid < kFirstSpi || intid >= intids) {
        snprintf(buffer, size, "INTID %u invalid", intid);
        return;
    }
    const IOInterruptVector &vector = vectors[intid];
    snprintf(buffer, size,
             "INTID %u: fired %u, registered %u, soft-off %u, gicd en %u pend %u act %u, %s, prio %#x, "
             "target %#x, lic ier %d",
             intid, fired[intid], vector.interruptRegistered, vector.interruptDisabledSoft,
             static_cast<int>(readBit(GICD_ISENABLER, intid)), static_cast<int>(readBit(GICD_ISPENDR, intid)),
             static_cast<int>(readBit(GICD_ISACTIVER, intid)), isEdge(intid) ? "edge" : "level",
             read8(GICD_IPRIORITYR + intid), read8(GICD_ITARGETSR + intid),
             licCovers(intid) ? static_cast<int>((licRead(licBank(intid), ICTLR_CPU_IER) >> (intid % 32)) & 1) : -1);
    int active = -1;
    for (uint32_t i = kFirstSpi; i < intids && active < 0; ++i) {
        if (readBit(GICD_ISACTIVER, i)) {
            active = static_cast<int>(i);
        }
    }
    const size_t used = strnlen(buffer, size);
    if (used + 1 >= size) {
        return;
    }
    char cpu[96];
    armcpu_gic_cpu_describe(cpu, sizeof(cpu));
    snprintf(buffer + used, size - used, ", active %d, %s", active, cpu);
}

// System sleep (drivers/SwitchSuspend quiesce/active actions): boot CPU, interrupts off, the other cores off. The
// SPIs' enables, priorities, targets and triggers, and the LIC's gates, are saved before SC7 and written back after
// it, all lines disabled while they are reprogrammed (ICFGR may change only then). Interrupts that went pending while
// the SoC was off are gone; a level line still asserted (the PMIC's wake interrupt) fires again once enabled.
void ArmGIC::suspend()
{
    for (uint32_t i = kFirstSpi / 32; i < intids / 32; ++i)
        savedEnable[i] = read32(GICD_ISENABLER + i * 4);
    for (uint32_t i = kFirstSpi / 16; i < intids / 16; ++i)
        savedConfig[i] = read32(GICD_ICFGR + i * 4);
    for (uint32_t i = kFirstSpi / 4; i < intids / 4; ++i) {
        savedPriority[i] = read32(GICD_IPRIORITYR + i * 4);
        savedTarget[i] = read32(GICD_ITARGETSR + i * 4);
    }
    for (uint32_t bank = 0; lic != nullptr && bank < kLicBanks; ++bank) {
        savedLicEnable[bank] = licRead(bank, ICTLR_CPU_IER);
        savedLicClass[bank] = licRead(bank, ICTLR_CPU_IEP_CLASS);
    }
}

void ArmGIC::resume()
{
    for (uint32_t i = kFirstSpi / 32; i < intids / 32; ++i)
        write32(GICD_ICENABLER + i * 4, 0xffffffffu);
    for (uint32_t i = kFirstSpi / 32; i < intids / 32; ++i)
        write32(GICD_ICPENDR + i * 4, 0xffffffffu);
    for (uint32_t i = kFirstSpi / 16; i < intids / 16; ++i)
        write32(GICD_ICFGR + i * 4, savedConfig[i]);
    for (uint32_t i = kFirstSpi / 4; i < intids / 4; ++i) {
        write32(GICD_IPRIORITYR + i * 4, savedPriority[i]);
        write32(GICD_ITARGETSR + i * 4, savedTarget[i]);
    }
    for (uint32_t bank = 0; lic != nullptr && bank < kLicBanks; ++bank) {
        licWrite(bank, ICTLR_CPU_IEP_CLASS, savedLicClass[bank]);
        licWrite(bank, ICTLR_CPU_IER_CLR, ~savedLicEnable[bank]);
        licWrite(bank, ICTLR_CPU_IER_SET, savedLicEnable[bank]);
    }
    for (uint32_t i = kFirstSpi / 32; i < intids / 32; ++i)
        write32(GICD_ISENABLER + i * 4, savedEnable[i]);
    OSSynchronizeIO();
}

// ArmCpu, when logical CPU `cpu` first takes interrupts: the kSteered lines made before it was up move to it.
void ArmGIC::cpuOnline(uint32_t cpu)
{
    const uint8_t target = cpuTarget(cpu);
    if (!target)
        return;
    for (const SteeredSpi &spi : kSteered) {
        const uint32_t bit = 1U << (spi.intid % 32);
        if (spi.cpu != cpu || spi.intid >= intids ||
            !(__atomic_load_n(&steerPending[spi.intid / 32], __ATOMIC_RELAXED) & bit))
            continue;
        write8(GICD_ITARGETSR + spi.intid, target);
        __atomic_fetch_and(&steerPending[spi.intid / 32], ~bit, __ATOMIC_RELAXED);
    }
    OSSynchronizeIO();
}

extern "C" void arm_gic_cpu_online(uint32_t cpu)
{
    ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    if (gic)
        gic->cpuOnline(cpu);
}

extern "C" void arm_gic_suspend(void)
{
    ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    if (gic)
        gic->suspend();
}

extern "C" void arm_gic_resume(void)
{
    ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    if (gic)
        gic->resume();
}

extern "C" bool switch_gic_dispatch(uint32_t intid)
{
    ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    return (gic != nullptr) && gic->handleInterrupt(nullptr, nullptr, static_cast<int>(intid)) == kIOReturnSuccess;
}

int switch_check_after_ms(IOTimerEventSource *timer, uint32_t ms)
{
    uint64_t interval = 0;
    clock_interval_to_absolutetime_interval(ms, kMillisecondScale, &interval);
    return timer->setTimeout(kIOTimeOptionsWithLeeway, interval, interval);
}

bool switch_interrupts_ready()
{
    return ArmGIC::shared() != nullptr;
}

// A driver may start before ArmCpu has CPU0's interface up: it sleeps until ArmCpu publishes CPU0's target
// (publishGicTarget wakes this event), and panics if that never happens.
IOService *switch_interrupt_nub(uint32_t intid, bool edgeTriggered)
{
    ArmGIC *gic = ArmGIC::shared();
    if (!gic) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): shared() lends the singleton, it is never released
        while (!(gic = ArmGIC::shared())) {
            if (armcpu_gic_ready && armcpu_gic_version != 2) {
                panic("ArmGIC: INTID %u asked for, but the interrupt controller is GICv%u", intid, armcpu_gic_version);
            }
            assert_wait_deadline(reinterpret_cast<event_t>(reinterpret_cast<uintptr_t>(&armcpu_gic_targets[0])),
                                 THREAD_UNINT, deadline);
            gic = ArmGIC::shared();
            if (gic) {
                clear_wait(current_thread(), THREAD_AWAKENED);
                break;
            }
            if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && !ArmGIC::shared()) {
                panic("ArmGIC: INTID %u asked for, GIC not up after 10 s (version %u, ready %u, cpu0 target %#x)",
                      intid, armcpu_gic_version, armcpu_gic_ready, armcpu_gic_targets[0]);
            }
        }
    }
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): shared() lends the singleton, it is never released
    return gic->nubFor(intid, edgeTriggered);
}

uint32_t switch_interrupt_count(uint32_t intid)
{
    const ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    return gic ? gic->count(intid) : 0;
}

void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size)
{
    const ArmGIC *gic = __atomic_load_n(&gGic, __ATOMIC_ACQUIRE);
    if (gic) {
        gic->describe(intid, buffer, size);
    } else {
        snprintf(buffer, size, "INTID %u: no GIC controller", intid);
    }
}
