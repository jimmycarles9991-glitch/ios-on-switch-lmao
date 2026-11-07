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
#include "SwitchEMC.h"
#include "SwitchEMCGovernor.h"
#include "SwitchPower.h"

namespace {
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

class SwitchEMC : public OSObject {
    OSDeclareDefaultStructors(SwitchEMC) IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *actmon = nullptr, *signal = nullptr;
    IOTimerEventSource *hold = nullptr;
    volatile uint8_t *regs = nullptr;
    uint32_t khz[SWITCH_EMC_MAX_RATES] = {};
    unsigned count = 0, cur = 0;
    bool governing = false, refusalLogged = false;
    uint64_t hintUntil = 0;
    uint64_t holdTicks = 0, frameTicks = 0;
#if DEVELOPMENT || DEBUG
    uint64_t residency[SWITCH_EMC_MAX_RATES] = {}, since = 0;
    unsigned moves = 0;
#endif

    static void actmonFired(OSObject *owner, IOInterruptEventSource *, int);
    static void signalFired(OSObject *owner, IOInterruptEventSource *, int);
    static void holdFired(OSObject *owner, IOTimerEventSource *);
    bool held(uint64_t now);
    void armHoldEnd(uint64_t now);
    void program();
    void moveTo(unsigned index);
    bool setup(char *why, size_t size);
    void startActmon();
    void stopActmon();
    void free() override;
    static IOReturn governAction(OSObject *owner, void *, void *, void *, void *);
    static IOReturn pauseAction(OSObject *owner, void *, void *, void *, void *);

public:
    // Thread context, once: the governor ready but not yet governing; nullptr with `why` filled when it cannot run.
    static SwitchEMC *create(char *why, size_t size);
    void govern();
    void pause();
    void hint();
#if DEVELOPMENT || DEBUG
    static IOReturn statsAction(OSObject *owner, void *line, void *size, void *written, void *);
    size_t stats(char *line, size_t size);

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

void framesStarted(void)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->hint();
}
}  // namespace

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
    int err = rows < 0 ? rows : tegra_bpmp_emc_get(&now);
    if (rows < 0 || err) {
        snprintf(why, size, "the BPMP gave no EMC table or rate (%d)", rows < 0 ? rows : err);
        return false;
    }
    count = switch_emc_rates(table, static_cast<unsigned>(rows), now + now / 1000, khz);
    if (count < 2 || switch_emc_row(khz, count, now) != count - 1) {
        snprintf(why, size, "EMC at %u kHz is not the top of %u usable BPMP rates", now, count);
        return false;
    }
    cur = count - 1;
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
    return now < __atomic_load_n(&hintUntil, __ATOMIC_ACQUIRE) || (frame && now - frame < frameTicks);
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
    const switch_emc_wmarks w = switch_emc_watermarks(khz, count, cur, keep);
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

void SwitchEMC::moveTo(unsigned index)
{
    if (index == cur)
        return;
    unsigned now = 0;
    const int err = tegra_bpmp_emc_set(khz[index], &now);
    if (err) {
        if (!refusalLogged)
            os_log_error(OS_LOG_DEFAULT, "SwitchEMC: the BPMP refused %u kHz (%d); staying at %u kHz", khz[index], err,
                         khz[cur]);
        refusalLogged = true;
        return;
    }
#if DEVELOPMENT || DEBUG
    const uint64_t t = mach_absolute_time();
    residency[cur] += t - since;
    since = t;
    moves++;
#endif
    cur = index;
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
    self->moveTo(switch_emc_next(self->count, self->cur, events, self->held(mach_absolute_time())));
    self->program();
}

void SwitchEMC::signalFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (!self->governing)
        return;
    self->moveTo(self->count - 1);
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
    uint64_t until = mach_absolute_time() + holdTicks;
    uint64_t was = __atomic_load_n(&hintUntil, __ATOMIC_RELAXED);
    while (until > was &&
           !__atomic_compare_exchange_n(&hintUntil, &was, until, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
    if (!__atomic_load_n(&gTopHeld, __ATOMIC_ACQUIRE))
        signal->interruptOccurred(nullptr, nullptr, 0);
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

IOReturn SwitchEMC::governAction(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    unsigned now = 0;
    if (self->governing || tegra_bpmp_emc_get(&now))
        return kIOReturnSuccess;
    const unsigned row = switch_emc_row(self->khz, self->count, now);
    if (row < self->count)
        self->cur = row;
#if DEVELOPMENT || DEBUG
    self->since = mach_absolute_time();
#endif
    self->governing = true;
    self->startActmon();
    return kIOReturnSuccess;
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
void SwitchEMC::govern()
{
    loop->runAction(&SwitchEMC::governAction, this);
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
    uint64_t total = 0, at[SWITCH_EMC_MAX_RATES];
    for (unsigned i = 0; i < count; ++i) {
        at[i] = residency[i] + (governing && i == cur ? t - since : 0);
        total += at[i];
    }
    size_t n = static_cast<size_t>(snprintf(line, size, "EMC %u moves", moves));
    for (unsigned i = 0; i < count && n < size; ++i) {
        const unsigned pm = total ? static_cast<unsigned>(at[i] * 1000 / total) : 0;
        n += static_cast<size_t>(snprintf(line + n, size - n, " %u:%u.%u%%", khz[i] / 1000, pm / 10, pm % 10));
    }
    memset(residency, 0, sizeof(residency));
    since = t;
    moves = 0;
    return n;
}
#endif

extern "C" void switch_emc_start(void)
{
    char why[128];
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
    if (hint == SWITCH_PERF_HINT_BOOT_DONE)
        emc->govern();
    else
        emc->hint();
}

// TegraGM20B, before each channel's doorbell: work for the GPU, so the EMC goes to its top rate ahead of that work's
// traffic, as for a hint. One load while it is already there and held; never blocks.
extern "C" void switch_emc_gpu_submit(void)
{
    if (__atomic_load_n(&gTopHeld, __ATOMIC_RELAXED))
        return;
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->hint();
}

extern "C" void switch_emc_suspend(void)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->pause();
}

extern "C" void switch_emc_resume(void)
{
    if (SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE))
        emc->govern();
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
