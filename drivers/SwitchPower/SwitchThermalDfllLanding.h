// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test cpu.dfll_landing, debug kernels only (SwitchThermal.cpp includes this under DEVELOPMENT || DEBUG): DFLL rows
// asked from a row above and from rows below, the CPU's clock measured where it runs 2, 10, 50 and 200 ms after each
// request, the loop's registers beside each reading (Tegra X1 TRM 6.1.5 DFLL_TUNE1, 6.1.6 DFLL_FREQ_REQ, 6.1.9
// DFLL_OUTPUT_CFG, 6.1.12 DFLL_MONITOR_DATA, 6.1.15 DFLL_I2C_STS). Writing debug.switch_perf_dfll_landing runs it and
// returns once it has ended: each request and each reading is one step on the work loop and the waits between them
// sleep on the writer's thread, so ALERT, SOC_THERM and the thermal timer run meanwhile; the governor's requests wait
// until it has asked the rate it found again. A reading lands up to one work-loop hand-off after its time. Reading
// gives the line: "dfll=ran" and, per case i, "i=<from MHz>><to MHz>" and "i.<ms>=<probe kHz>,<FREQ_REQ>,<TUNE1>,
// <OUTPUT_CFG>,<MONITOR_DATA>,<I2C_STS>" (registers in hex).

#include "../../kits/SwitchSysctl/SwitchSysctl.h"

// The cluster's clock, measured where it runs: a chain of dependent adds (one per cycle on the A57) timed by the
// generic timer with interrupts masked, as XNU's kprof_clock_probe (switch_kprof.c) does, 4x longer (131072
// adds: about 73 us at 1785 MHz, 0.07 % timer resolution). kprof's probe read 995-997 MHz at 998.4, 1664-1668 at
// 1670.4 and 1782-1787 at 1785 on this console: within 0.3 % low, against DFLL steps 1.4 % apart. In kHz.
static uint32_t probeCpuKhz()
{
    constexpr uint64_t kLoops = 16384;
    constexpr uint64_t kAdds = kLoops * 8;
    uint64_t n = 0;
    // NOLINTNEXTLINE(misc-const-correctness): the probe's adds write it ("+r")
    uint64_t x = 0;
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    for (int pass = 0; pass < 2; pass++) {  // the first pass warms the branch predictor and the I-cache
        n = pass ? kLoops : 64;
        t0 = mach_absolute_time();
        __asm__ volatile("1:\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "add %1, %1, #1\n"
                         "subs %0, %0, #1\n"
                         "b.ne 1b\n"
                         : "+r"(n), "+r"(x)
                         :
                         : "cc");
        t1 = mach_absolute_time();
    }
    ml_set_interrupts_enabled(enabled);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    return ns ? static_cast<uint32_t>(kAdds * 1000000 / ns) : 0;
}

namespace {
struct DfllLandingCase {
    unsigned long from, to;
};

// 1581 and 1683 from above and below, 1785 from the row below and from the bottom: 1683 and 1785 are high-tune rows,
// 1581 and below low-tune, so a request from below 1683 crosses from the low tune to the high.
constexpr DfllLandingCase kDfllLandingCases[] = {
    {1683000000UL, 1581000000UL}, {1428000000UL, 1581000000UL}, {1785000000UL, 1683000000UL},
    {1581000000UL, 1683000000UL}, {1683000000UL, 1785000000UL}, {918000000UL, 1785000000UL},
};
constexpr uint32_t kDfllLandingAfterMs[] = {2, 10, 50, 200};
constexpr uint32_t kDfllLandingFromMs = 200;  // the first rate settles: five of clk-dfll.c's 40 ms calibration periods
char gDfllLandingLine[2048] = "dfll=not_run";
struct sysctl_oid gDfllLandingOid;
bool gDfllLandingBusy;

enum class DfllLandingStepKind {
    Begin,
    Ask,
    Read,
    End
};

// The writer's thread sets the step and its rate, the work loop runs it and fills the rest (runAction orders the two).
struct DfllLandingRun {
    DfllLandingStepKind step;
    unsigned long hz, restore;
    unsigned raise;  // perfRaises at Begin: a raise since is another DFLL
    bool on;         // still on the DFLL the run began on
    int err;
    uint32_t khz;
    tegra_dfll_cpu_loop regs;
};

DfllLandingRun gDfllLandingRun;

void dfllLandingAppend(size_t &n, const char *format, ...) __printflike(2, 3);

void dfllLandingAppend(size_t &n, const char *format, ...)
{
    if (n >= sizeof(gDfllLandingLine))
        return;
    va_list args;
    va_start(args, format);
    n += static_cast<size_t>(vsnprintf(gDfllLandingLine + n, sizeof(gDfllLandingLine) - n, format, args));
    va_end(args);
}
}  // namespace

// Work loop: one step of the run. No step sleeps.
void SwitchThermal::dfllLandingStep()
{
    DfllLandingRun &run = gDfllLandingRun;
    if (run.step == DfllLandingStepKind::Begin) {
        run.raise = perfRaises;
        run.restore = static_cast<unsigned long>(switch_perf_cpu_khz()) * 1000;
    }
    run.on = perfRaised && dfllMode && perfRaises == run.raise;
    switch (run.step) {
    case DfllLandingStepKind::Begin:
        dfllLandingRuns = run.on;
        break;
    case DfllLandingStepKind::Ask:
        run.err = run.on ? tegra_dfll_cpu_set_rate(run.hz) : 0;
        break;
    case DfllLandingStepKind::Read:
        if (run.on) {
            run.khz = probeCpuKhz();
            tegra_dfll_cpu_loop_registers(&run.regs);
        }
        break;
    case DfllLandingStepKind::End: {
        if (!run.on) {
            run.err = -ENODEV;
        } else {
            run.err = run.restore ? tegra_dfll_cpu_set_rate(run.restore) : -EINVAL;
        }
        dfllLandingRuns = false;
        const bool pending = governorPending;
        governorPending = false;
        if (pending && switchperf::running())
            applyGovernor();
        break;
    }
    }
}

// The writer's thread: the steps on the work loop, the waits here.
void SwitchThermal::dfllLanding()
{
    DfllLandingRun &run = gDfllLandingRun;
    const auto step = [this](DfllLandingStepKind kind) {
        run.step = kind;
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                static_cast<SwitchThermal *>(owner)->dfllLandingStep();
                return kIOReturnSuccess;
            },
            this);
        return run.on;
    };
    size_t n = 0;
    if (!step(DfllLandingStepKind::Begin)) {
        dfllLandingAppend(n, "dfll=not_run reason=not_on_dfll");
        return;
    }
    dfllLandingAppend(n, "dfll=ran");
    for (unsigned c = 0; c < sizeof(kDfllLandingCases) / sizeof(kDfllLandingCases[0]) && run.on; ++c) {
        const DfllLandingCase &k = kDfllLandingCases[c];
        dfllLandingAppend(n, " %u=%lu>%lu", c, k.from / 1000000, k.to / 1000000);
        run.hz = k.from;
        if (step(DfllLandingStepKind::Ask) && !run.err) {
            IOSleep(kDfllLandingFromMs);
            run.hz = k.to;
            (void)step(DfllLandingStepKind::Ask);
        }
        if (!run.on)
            break;
        if (run.err) {
            dfllLandingAppend(n, " %u.err=%d", c, run.err);
            continue;
        }
        uint32_t slept = 0;
        for (const uint32_t ms : kDfllLandingAfterMs) {
            IOSleep(ms - slept);
            slept = ms;
            if (!step(DfllLandingStepKind::Read))
                break;
            dfllLandingAppend(n, " %u.%u=%u,%x,%x,%x,%x,%x", c, ms, run.khz, run.regs.freq_req, run.regs.tune1,
                              run.regs.output_cfg, run.regs.monitor, run.regs.i2c_sts);
        }
    }
    if (!run.on)
        dfllLandingAppend(n, " reason=not_on_dfll");
    (void)step(DfllLandingStepKind::End);
    dfllLandingAppend(n, " restore=%lu,%d", run.restore / 1000000, run.err);
    IOLog("SwitchPerf: DFLL landing: %s\n", gDfllLandingLine);
}

int SwitchThermal::dfllLandingSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) {
        int value = 0;
        if (const int error = SYSCTL_IN(req, &value, sizeof(value)))
            return error;
        SwitchThermal *self = gThermal;
        if (!self || !self->loop)
            return ENODEV;
        if (__atomic_exchange_n(&gDfllLandingBusy, true, __ATOMIC_ACQUIRE))
            return EBUSY;
        self->dfllLanding();
        __atomic_store_n(&gDfllLandingBusy, false, __ATOMIC_RELEASE);
    }
    return SYSCTL_OUT(req, gDfllLandingLine, strlen(gDfllLandingLine) + 1);
}

void SwitchThermal::dfllLandingRegister()
{
    if (gDfllLandingOid.oid_name)
        return;
    switch_sysctl_oid(&gDfllLandingOid, &sysctl__debug_children, "switch_perf_dfll_landing",
                      static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED),
                      &SwitchThermal::dfllLandingSysctl, "A",
                      "write: ask DFLL rows from above and below and measure them; read: the readings");
    sysctl_register_oid(&gDfllLandingOid);
}
