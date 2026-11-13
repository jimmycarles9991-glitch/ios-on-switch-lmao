// SPDX-License-Identifier: BSD-2-Clause-Patent
// The EMC (DRAM) clock on demand: ACTMON's watermark interrupts and the frame and input hints decide
// (SwitchEMCGovernor.h), the BPMP firmware changes the clock (TegraBPMP tegra_bpmp_emc_set, as L4T's T210B01 does).
//
// ACTMON (T210 TRM "Activity Monitor"; L4T cactmon_t21x.c and actmon_common.c, the console DT's actmon@6000c800):
// global period control and status, then one block per monitored device; MC_ALL's at 0x1c0 counts all
// memory-controller traffic. Its interrupt is SPI 45 (INTID 77), level high. Its clock is CAR clock 119 on clk_m.
//
// Governing starts at System Boot Complete (the boot runs at the top rate, as the CPU does) and pauses across SC7:
// switch_thermal_suspend stops it before the BPMP drops the EMC for SC7, switch_thermal_resume starts it again after
// the BPMP has restored the rate (ACTMON lost its state with the SoC).
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <os/log.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraBPMP/TegraBPMP.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchMMIO/SwitchMMIOWords.h"
#include "SwitchClocks.h"
#include "SwitchDisplayMemoryState.h"
#include "SwitchEMC.h"
#include "SwitchEMCGovernor.h"
#include "SwitchDisplayBandwidth.h"
#include "SwitchPower.h"

namespace {
uint64_t gDisplayBytes = 78000000ULL * 4;
constexpr uint64_t kActmonPA = 0x6000c800;
constexpr uint32_t kGlbStatus = 0x00, kGlbPeriodCtrl = 0x04, kGlbMcAll = 1u << 26;
constexpr uint32_t kMcAll = 0x1c0;
constexpr uint32_t kCtrl = 0x00, kUpWmark = 0x04, kDownWmark = 0x08, kInitAvg = 0x0c, kCountWeight = 0x18,
                   kIntrStatus = 0x24;
constexpr uint32_t kCtrlEnable = 1u << 31, kCtrlUpWmark = 1u << 30, kCtrlDownWmark = 1u << 29, kCtrlPeriodic = 1u << 18;
constexpr uint32_t kCtrlUpNumShift = 26, kCtrlDownNumShift = 23, kCtrlKShift = 10;
constexpr uint32_t kIntrUp = 1u << 31, kIntrDown = 1u << 30;
// L4T's mc_all values (the console DT): count weight 0x400, average window 2^7 samples.
constexpr uint32_t kMcAllCountWeight = 0x400, kAvgWindowLog2 = 7;
// ACTMON's clock is left on the parent the boot chain gave it, as L4T leaves it: its sample period does not run on it.
constexpr uint32_t kActmonIntid = 32 + 45, kActmonClock = 119;
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchEMC : public OSObject {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchEMC) IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *actmon = nullptr, *signal = nullptr;
    IOTimerEventSource *hold = nullptr;
    volatile uint8_t *regs = nullptr;
    SwitchMMIOWords mcRegs;
    switch_display::MemoryState memoryState;
    bool memoryQualified = false, memoryDirty = false, fixed = false;
    uint32_t memoryAppliedKhz = 0;
    volatile uint8_t *car = nullptr;
    uint32_t khz[SWITCH_EMC_MAX_RATES] = {};
    unsigned count = 0, cur = 0;
    bool governing = false, refusalLogged = false;
    uint64_t hintUntil = 0;
    uint64_t holdTicks = 0, frameTicks = 0;
#if DEVELOPMENT || DEBUG
    uint64_t residency[SWITCH_EMC_MAX_RATES] = {}, since = 0;
    unsigned moves = 0;
    // Hints that found the EMC below its top rate: from the first such hint to the BPMP's answer at the top rate.
    uint64_t raiseAsked = 0, raiseTicks = 0, raiseMax = 0, bpmpMax = 0;
    unsigned raises = 0;
#endif

    static void actmonFired(OSObject *owner, IOInterruptEventSource *, int);
    static void signalFired(OSObject *owner, IOInterruptEventSource *, int);
    static void holdFired(OSObject *owner, IOTimerEventSource *);
    bool held(uint64_t now);
    void armHoldEnd(uint64_t now);
    void program();
    bool moveTo(unsigned asked, switch_emc_reason reason);
    unsigned reconcileRate(uint32_t observed, bool quiesced = false);
    bool useFixedRate(unsigned now);
    IOReturn holdFixedRate();

    unsigned carEmcKhz() const
    {
        return car ? switchclk::readEmc(car).kHz : 0;
    }

    uint64_t floorBytes() const;
    unsigned memoryFloor(unsigned first) const;
    void dropDisplayMemory(const char *why);
    bool programMemory(uint32_t before, uint32_t after, bool force = false);
    static IOReturn memoryAction(OSObject *owner, void *request, void *result, void *, void *);
    static IOReturn displayAction(OSObject *owner, void *bytes, void *result, void *, void *);
    bool setup(char *why, size_t size);
    void startActmon();
    void stopActmon();
    void free() override;
    static IOReturn governAction(OSObject *owner, void *, void *, void *, void *);
    static IOReturn pauseAction(OSObject *owner, void *, void *, void *, void *);

public:
    // Thread context, once: the governor ready but not yet governing; nullptr with `why` filled when it cannot run.
    static SwitchEMC *create(char *why, size_t size);
    void govern(bool resume = false);
    void pause();
    void hint();
    void deadline(uint64_t until);
    int reserveDisplay(uint64_t bytes);
    int reserveMemory(const switch_display_memory_head &request);

    bool displayMemoryReady() const
    {
        return __atomic_load_n(&memoryQualified, __ATOMIC_ACQUIRE);
    }
#if DEVELOPMENT || DEBUG
    static IOReturn statsAction(OSObject *owner, void *line, void *size, void *written, void *);
    size_t stats(char *line, size_t size);
    void logMove(unsigned from, unsigned to, unsigned asked, switch_emc_reason reason) const;

    void runStats(char *line, size_t size, size_t *written)
    {
        loop->runAction(&SwitchEMC::statsAction, this, line, &size, written);
    }
#endif
};

namespace {
SwitchEMC *gEMC;
// At the top rate with the down watermark off: a hint then only extends the hold, and a GPU submission costs this one
// load, without waking the work loop.
bool gTopHeld;

void framesStarted()
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->hint();
}
}  // namespace

#include "SwitchDisplayMemoryEMC.h"
#include "SwitchEMCClock.h"
#include "SwitchEMCFixed.h"

SwitchEMC *SwitchEMC::create(char *why, size_t size)
{
    auto *emc = OSTypeAlloc(SwitchEMC);
    if (!emc || !emc->init()) {
        snprintf(why, size, "no memory");
        OSSafeReleaseNULL(emc);
        return nullptr;
    }
    if (!emc->setup(why, size)) {
        emc->release();
        return nullptr;
    }
    return emc;
}

void SwitchEMC::free()
{
    IOEventSource *sources[] = {actmon, signal, hold};
    for (IOEventSource *source : sources) {
        if (!source)
            continue;
        source->disable();
        if (loop)
            loop->removeEventSource(source);
        source->release();
    }
    OSSafeReleaseNULL(loop);
    OSObject::free();
}

bool SwitchEMC::setup(char *why, size_t size)
{
    uint32_t table[SWITCH_EMC_MAX_RATES * 2];
    unsigned now = 0;
    const int rows = tegra_bpmp_emc_rates(table, sizeof(table) / sizeof(table[0]));
    const int err = rows < 0 ? rows : tegra_bpmp_emc_get(&now);
    if (rows < 0 || err) {
        car = switch_mmio_map(switchclk::kCarPA, 0x1000);
        now = carEmcKhz();
        if (!useFixedRate(now)) {
            snprintf(why, size, "the BPMP gave no EMC table or rate (%d) and CLK_SOURCE_EMC reads no rate",
                     rows < 0 ? rows : err);
            return false;
        }
    } else {
        count = switch_emc_rates(table, static_cast<unsigned>(rows), now + now / 1000, khz);
        if (count < 2 || switch_emc_row(khz, count, now) != count - 1) {
            snprintf(why, size, "EMC at %u kHz is not the top of %u usable BPMP rates", now, count);
            return false;
        }
    }
    cur = count - 1;
    mcRegs = SwitchMMIOWords(switch_mmio_map(0x70019000, 0x1000));
    const auto emcProfile = switch_mmio_map(0x7001b000, 0x1000);
    const auto chipProfile = switch_mmio_map(0x70000800, 0x100);
    const uint32_t hidrev = chipProfile ? switch_mmio_read32(chipProfile, 4) : 0;
    const uint32_t fbio = emcProfile ? switch_mmio_read32(emcProfile, 0x104) : 0;
    memoryQualified = mcRegs && emcProfile && chipProfile && switch_display::memoryProfile(hidrev, fbio);
    if (!memoryQualified)
        IOLog("SwitchEMC: display memory admission off: HIDREV %#x FBIO_CFG5 %#x is not a T21x with LPDDR4\n", hidrev,
              fbio);
    if (fixed)
        IOLog("SwitchEMC: no BPMP firmware: the EMC stays at %u kHz (%s), where display memory admission plans\n", now,
              switchclk::emcSourceName(switchclk::readEmc(car).source));
    regs = switch_mmio_map(kActmonPA, 0x400);
    loop = IOWorkLoop::workLoop();
    IOService *nub = switch_interrupt_nub(kActmonIntid, false);
    actmon = IOInterruptEventSource::interruptEventSource(this, &SwitchEMC::actmonFired, nub, 0);
    signal = IOInterruptEventSource::interruptEventSource(this, &SwitchEMC::signalFired);
    hold = IOTimerEventSource::timerEventSource(this, &SwitchEMC::holdFired);
    if (!regs || !loop || !actmon || !signal || !hold || loop->addEventSource(actmon) != kIOReturnSuccess ||
        loop->addEventSource(signal) != kIOReturnSuccess || loop->addEventSource(hold) != kIOReturnSuccess) {
        snprintf(why, size, "no ACTMON mapping or work loop");
        return false;
    }
    nanoseconds_to_absolutetime(uint64_t{SWITCH_EMC_HINT_HOLD_US} * NSEC_PER_USEC, &holdTicks);
    nanoseconds_to_absolutetime(uint64_t{100} * NSEC_PER_MSEC, &frameTicks);
    signal->enable();
    return true;
}

bool SwitchEMC::held(uint64_t now)
{
    const uint64_t frame = switch_idle_last_frame();
    return now < __atomic_load_n(&hintUntil, __ATOMIC_ACQUIRE) || ((frame != 0u) && now - frame < frameTicks);
}

void SwitchEMC::armHoldEnd(uint64_t now)
{
    const uint64_t frame = switch_idle_last_frame();
    uint64_t end = __atomic_load_n(&hintUntil, __ATOMIC_ACQUIRE);
    if (frame && frame + frameTicks > end)
        end = frame + frameTicks;
    if (end > now)
        hold->wakeAtTime(end);
}

// ACTMON's watermarks for the rate now; its sample count restarts there.
void SwitchEMC::program()
{
    const uint64_t now = mach_absolute_time();
    const bool keep = held(now);
    switch_emc_wmarks w = switch_emc_watermarks(khz, count, cur, keep);
    if (cur <= memoryFloor(0))
        w.down_on = false;
    const uint32_t base = kMcAll;
    switch_mmio_write32(regs, base + kUpWmark, w.up);
    switch_mmio_write32(regs, base + kDownWmark, w.down);
    switch_mmio_write32(regs, base + kInitAvg, khz[cur] * SWITCH_EMC_SAMPLE_MS);
    switch_mmio_write32(regs, base + kIntrStatus, ~0u);
    switch_mmio_write32(regs, base + kCtrl,
                        kCtrlEnable | kCtrlPeriodic | (kAvgWindowLog2 - 1) << kCtrlKShift |
                            (SWITCH_EMC_DOWN_SAMPLES - 1) << kCtrlDownNumShift | 0u << kCtrlUpNumShift |
                            (w.up_on ? kCtrlUpWmark : 0) | (w.down_on ? kCtrlDownWmark : 0));
    __atomic_store_n(&gTopHeld, cur + 1 == count && !w.down_on, __ATOMIC_RELEASE);
    if (keep)
        armHoldEnd(now);
}

IOReturn SwitchEMC::displayAction(OSObject *owner, void *bytes, void *result, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    const uint64_t demand = *static_cast<uint64_t *>(bytes);
    const unsigned floor = switch_display::row(self->khz, self->count, demand);
    int error = floor >= self->count ? -28 : 0;
    if (!error && floor > self->cur && !self->moveTo(floor, SWITCH_EMC_REASON_DISPLAY))
        error = -5;
    if (!error) {
        __atomic_store_n(&gDisplayBytes, demand, __ATOMIC_RELEASE);
        if (self->governing)
            self->program();
    }
    *static_cast<int *>(result) = error;
    return kIOReturnSuccess;
}

int SwitchEMC::reserveDisplay(uint64_t bytes)
{
    int error = 0;
    loop->runAction(&SwitchEMC::displayAction, this, &bytes, &error);
    return error;
}

void SwitchEMC::actmonFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (!self->governing)
        return;
    const uint32_t status = switch_mmio_read32(self->regs, kGlbStatus) & kGlbMcAll
                                ? switch_mmio_read32(self->regs, kMcAll + kIntrStatus)
                                : 0;
    switch_mmio_write32(self->regs, kMcAll + kIntrStatus, status);
    const uint32_t events =
        (status & kIntrUp ? SWITCH_EMC_EVENT_UP : 0) | (status & kIntrDown ? SWITCH_EMC_EVENT_DOWN : 0);
    self->moveTo(switch_emc_next(self->count, self->cur, events, self->held(mach_absolute_time())),
                 SWITCH_EMC_REASON_ACTMON);
    self->program();
}

void SwitchEMC::signalFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (!self->governing)
        return;
#if DEVELOPMENT || DEBUG
    const unsigned before = self->cur;
    const uint64_t bpmp = mach_absolute_time();
#endif
    self->moveTo(self->count - 1, SWITCH_EMC_REASON_HINT);
#if DEVELOPMENT || DEBUG
    const uint64_t asked = __atomic_exchange_n(&self->raiseAsked, 0, __ATOMIC_RELAXED);
    if (before != self->cur && asked) {
        const uint64_t now = mach_absolute_time();
        self->raises++;
        self->raiseTicks += now - asked;
        self->raiseMax = now - asked > self->raiseMax ? now - asked : self->raiseMax;
        self->bpmpMax = now - bpmp > self->bpmpMax ? now - bpmp : self->bpmpMax;
    }
#endif
    self->program();
}

void SwitchEMC::holdFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (self->governing)
        self->program();
}

// Any context (the scheduler's callout, an interrupt filter, a thread): the top rate at once, kept for the hold.
void SwitchEMC::hint()
{
    deadline(mach_absolute_time() + holdTicks);
}

void SwitchEMC::deadline(uint64_t until)
{
    uint64_t was = __atomic_load_n(&hintUntil, __ATOMIC_RELAXED);
    while (until > was &&
           !__atomic_compare_exchange_n(&hintUntil, &was, until, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
    if (!__atomic_load_n(&gTopHeld, __ATOMIC_ACQUIRE)) {
#if DEVELOPMENT || DEBUG
        uint64_t none = 0;
        __atomic_compare_exchange_n(&raiseAsked, &none, mach_absolute_time(), false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED);
#endif
        signal->interruptOccurred(nullptr, nullptr, 0);
    }
}

void SwitchEMC::startActmon()
{
    switch_car_clock_disable(kActmonClock);
    switch_car_reset_assert(kActmonClock);
    switch_car_clock_enable(kActmonClock);
    IODelay(10);
    switch_car_reset_deassert(kActmonClock);
    switch_mmio_write32(regs, kGlbPeriodCtrl, SWITCH_EMC_SAMPLE_MS - 1);
    switch_mmio_write32(regs, kMcAll + kCountWeight, kMcAllCountWeight);
    program();
    actmon->enable();
}

void SwitchEMC::stopActmon()
{
    __atomic_store_n(&gTopHeld, false, __ATOMIC_RELEASE);
    actmon->disable();
    switch_mmio_write32(regs, kMcAll + kCtrl, 0);
    switch_mmio_write32(regs, kMcAll + kIntrStatus, ~0u);
    hold->cancelTimeout();
}

IOReturn SwitchEMC::pauseAction(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (!self->governing)
        return kIOReturnSuccess;
    self->stopActmon();
#if DEVELOPMENT || DEBUG
    const uint64_t t = mach_absolute_time();
    self->residency[self->cur] += t - self->since;
    self->since = t;
#endif
    self->governing = false;
    return kIOReturnSuccess;
}

// Thread context: the work loop runs it.
void SwitchEMC::govern(bool resume)
{
    const IOReturn error = loop->runAction(&SwitchEMC::governAction, this, resume ? this : nullptr);
    if (resume && error != kIOReturnSuccess)
        os_log_error(OS_LOG_DEFAULT, "SwitchEMC: not governing after SC7 (%#x): the EMC stays at %u kHz", error,
                     khz[cur]);
}

void SwitchEMC::pause()
{
    loop->runAction(&SwitchEMC::pauseAction, this);
}

#if DEVELOPMENT || DEBUG
IOReturn SwitchEMC::statsAction(OSObject *owner, void *line, void *size, void *written, void *)
{
    *static_cast<size_t *>(written) =
        static_cast<SwitchEMC *>(owner)->stats(static_cast<char *>(line), *static_cast<size_t *>(size));
    return kIOReturnSuccess;
}

size_t SwitchEMC::stats(char *line, size_t size)
{
    const uint64_t t = mach_absolute_time();
    uint64_t total = 0;
    uint64_t at[SWITCH_EMC_MAX_RATES];
    for (unsigned i = 0; i < count; ++i) {
        at[i] = residency[i] + (governing && i == cur ? t - since : 0);
        total += at[i];
    }
    size_t n = static_cast<size_t>(snprintf(line, size, "EMC %u moves", moves));
    for (unsigned i = 0; i < count && n < size; ++i) {
        const unsigned pm = total ? static_cast<unsigned>(at[i] * 1000 / total) : 0;
        n += static_cast<size_t>(snprintf(line + n, size - n, " %u:%u.%u%%", khz[i] / 1000, pm / 10, pm % 10));
    }
    if (n < size) {
        n += static_cast<size_t>(
            snprintf(line + n, size - n, ", %u raises: hint to top %llu/%llu us (BPMP max %llu us)", raises,
                     raises ? raiseTicks / raises * 5 / 96 : 0, raiseMax * 5 / 96, bpmpMax * 5 / 96));
    }
    memset(residency, 0, sizeof(residency));
    since = t;
    moves = raises = 0;
    raiseTicks = raiseMax = bpmpMax = 0;
    return n;
}
#endif

extern "C" void switch_emc_start(void)
{
    char why[128];
    // NOLINTNEXTLINE(misc-const-correctness): __atomic_store_n publishes it as gEMC, which is not const
    SwitchEMC *emc = SwitchEMC::create(why, sizeof(why));
    if (!emc) {
        IOLog("SwitchEMC: on-demand EMC off: %s\n", why);
        return;
    }
    __atomic_store_n(&gEMC, emc, __ATOMIC_RELEASE);
    switch_idle_set_frames_started(framesStarted);
    IOLog("SwitchEMC: on demand from System Boot Complete (ACTMON MC_ALL, INTID %u)\n", kActmonIntid);
}

extern "C" void switch_emc_hint(unsigned int hint)
{
    SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    if (!emc)
        return;
    if (hint == SWITCH_PERF_HINT_BOOT_DONE) {
        emc->govern();
    } else {
        emc->hint();
    }
}

// TegraGM20B and TegraHost1x, before each channel's doorbell: work for the GPU or a host1x engine, so the EMC goes to
// its top rate ahead of that work's traffic, as for a hint. One load while it is already there and held; never blocks.
extern "C" void switch_emc_gpu_submit(void)
{
    if (__atomic_load_n(&gTopHeld, __ATOMIC_RELAXED))
        return;
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->hint();
}

extern "C" int switch_emc_display_reserve(uint64_t bytes_per_second)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE)) {
        if (bytes_per_second == __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE))
            return 0;
        return emc->reserveDisplay(bytes_per_second);
    }
    uint64_t old = __atomic_load_n(&gDisplayBytes, __ATOMIC_RELAXED);
    while (old < bytes_per_second && !__atomic_compare_exchange_n(&gDisplayBytes, &old, bytes_per_second, true,
                                                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        return emc->reserveDisplay(bytes_per_second);
    unsigned khz = 0;
    const int error = tegra_bpmp_emc_get(&khz);
    return error ? error : khz >= switch_display::floorKhz(bytes_per_second) ? 0 : -28;
}

extern "C" void switch_emc_suspend(void)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->pause();
}

extern "C" void switch_emc_resume(void)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->govern(true);
}

#if DEVELOPMENT || DEBUG
extern "C" size_t switch_emc_stats(char *line, size_t size)
{
    SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    size_t written = 0;
    if (emc)
        emc->runStats(line, size, &written);
    return written;
}
#endif

OSDefineMetaClassAndStructors(SwitchEMC, OSObject)
