// SPDX-License-Identifier: BSD-2-Clause-Patent

#if DEVELOPMENT || DEBUG
namespace {
constexpr uint32_t kMcLatencyAllowanceDc = 0x2e8, kMcDisPtsaRate = 0x41c;
}
#endif

unsigned SwitchEMC::reconcileRate(uint32_t observed, bool quiesced)
{
    const unsigned row = switch_emc_row(khz, count, observed);
    if (row < count && !quiesced)
        return row;
    memoryDirty = true;
    if (!programMemory(quiesced ? khz[count - 1] : observed, khz[count - 1], true))
        uncoveredDisplayMemory(observed, "the move to the top rate");
    unsigned restored = observed;
    const int refused = row != count - 1 ? tegra_bpmp_emc_set(khz[count - 1], &restored) : 0;
    const unsigned now = switch_emc_row(khz, count, restored);
    if (refused || now >= count) {
        const uint32_t running = refused ? observed : restored;
        if (!programMemory(running, running, true))
            uncoveredDisplayMemory(running, "the BPMP did not restore a rate in the table");
        return row < count ? row : cur;
    }
    if (!programMemory(restored, restored, true))
        uncoveredDisplayMemory(restored, "the restored rate");
    return now;
}

bool SwitchEMC::moveTo(unsigned asked, switch_emc_reason reason)
{
    unsigned index = asked;
    const unsigned floor = switch_display::row(khz, count, __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE));
    if (floor >= count)
        index = count - 1;
    else if (index < floor)
        index = floor;
    index = memoryFloor(index);
    if (index >= count || !programMemory(khz[cur], khz[index]))
        return false;
    if (index == cur)
        return true;
    unsigned now = 0;
#if DEVELOPMENT || DEBUG
    const bool watch = !bpmpChangeLogged && __atomic_load_n(&memoryQualified, __ATOMIC_ACQUIRE);
    const uint32_t laBefore = watch ? uint32_t(mcRegs[kMcLatencyAllowanceDc / 4]) : 0;
    const uint32_t ptsaBefore = watch ? uint32_t(mcRegs[kMcDisPtsaRate / 4]) : 0;
#endif
    const int err = tegra_bpmp_emc_set(khz[index], &now);
    if (err) {
        if (!refusalLogged) {
            os_log_error(OS_LOG_DEFAULT, "SwitchEMC: the BPMP refused %u kHz (%d); staying at %u kHz", khz[index], err,
                         khz[cur]);
        }
        refusalLogged = true;
        (void)programMemory(khz[cur], khz[cur], true);
        return false;
    }
#if DEVELOPMENT || DEBUG
    if (watch)
        bpmpChangeLogged = logBpmpChange(khz[index], now, laBefore, ptsaBefore);
#endif
    const unsigned actual = reconcileRate(now);
#if DEVELOPMENT || DEBUG
    const uint64_t t = mach_absolute_time();
    residency[cur] += t - since;
    since = t;
    moves++;
    logMove(cur, actual, asked, reason);
#endif
    cur = actual;
    if (!programMemory(khz[cur], khz[cur], true))
        uncoveredDisplayMemory(khz[cur], "the rate the BPMP set");
    return cur >= index;
}

#if DEVELOPMENT || DEBUG
void SwitchEMC::logMove(unsigned from, unsigned to, unsigned asked, switch_emc_reason reason) const
{
    static const char *const kReasons[] = {"ACTMON", "a hint or submit", "display bandwidth", "display memory",
                                           "governor start"};
    const unsigned floor = memoryFloor(0);
    os_log(OS_LOG_DEFAULT, "SwitchEMC: %u -> %u kHz, %s asked %u kHz; display floor %u kHz for %llu MB/s", khz[from],
           khz[to], kReasons[reason], asked < count ? khz[asked] : 0, floor < count ? khz[floor] : 0,
           static_cast<unsigned long long>(floorBytes() / 1000000));
}

// Whether the BPMP firmware's own rate change sets another rate than asked, or rewrites head A's latency allowance
// (TRM 18.11.1.266) or PTSA rate (18.11.1.224) that the display memory plan programmed just before it. The first change
// that does either is logged.
bool SwitchEMC::logBpmpChange(unsigned asked, unsigned set, uint32_t laBefore, uint32_t ptsaBefore)
{
    const uint32_t la = mcRegs[kMcLatencyAllowanceDc / 4], ptsa = mcRegs[kMcDisPtsaRate / 4];
    if (set == asked && la == laBefore && ptsa == ptsaBefore)
        return false;
    os_log(OS_LOG_DEFAULT,
           "SwitchEMC: the BPMP rate change asked %u kHz and set %u kHz; MC LATENCY_ALLOWANCE_DC_0 %#x -> %#x, "
           "DIS_PTSA_RATE %#x -> %#x",
           asked, set, laBefore, la, ptsaBefore, ptsa);
    return true;
}
#endif

IOReturn SwitchEMC::governAction(OSObject *owner, void *resume, void *, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    unsigned now = 0;
    if (self->fixed)
        return self->holdFixedRate();
    if (self->governing)
        return kIOReturnSuccess;
    if (tegra_bpmp_emc_get(&now)) {
        if (!resume)
            return kIOReturnError;
        now = 0;
    }
    self->cur = self->reconcileRate(now, resume != nullptr);
#if DEVELOPMENT || DEBUG
    self->since = mach_absolute_time();
#endif
    self->memoryDirty = true;
    if (!self->moveTo(self->cur, SWITCH_EMC_REASON_GOVERN))
        return kIOReturnError;
    self->governing = true;
    self->startActmon();
    return kIOReturnSuccess;
}
