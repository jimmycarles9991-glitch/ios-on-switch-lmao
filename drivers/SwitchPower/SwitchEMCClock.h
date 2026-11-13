// SPDX-License-Identifier: BSD-2-Clause-Patent

unsigned SwitchEMC::reconcileRate(uint32_t observed, bool quiesced)
{
    const unsigned row = switch_emc_row(khz, count, observed);
    if (row < count && !quiesced)
        return row;
    memoryDirty = true;
    if (!programMemory(quiesced ? khz[count - 1] : observed, khz[count - 1], true))
        dropDisplayMemory("its plan cannot cover the move to the top rate");
    unsigned restored = observed;
    const int refused = row != count - 1 ? tegra_bpmp_emc_set(khz[count - 1], &restored) : 0;
    const unsigned now = switch_emc_row(khz, count, restored);
    if (refused || now >= count) {
        dropDisplayMemory("the BPMP did not restore a rate in the table");
        return row < count ? row : cur;
    }
    if (!programMemory(restored, restored, true))
        dropDisplayMemory("its plan does not cover the restored rate");
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
        dropDisplayMemory("its plan does not cover the rate the BPMP set");
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
