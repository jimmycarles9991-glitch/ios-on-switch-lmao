// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEMC's part of the power stats line (debug kernels; included by SwitchEMC.cpp): rate changes, the share of time
// at each rate, hints' raises, the CPUs' busy time at each rate, the windows of work that found the EMC not held at its
// top rate, and the moves the BPMP or the display memory plan refused.
#if DEVELOPMENT || DEBUG
// The tick, one CPU at a time: its own counters, stored as they grow for the loop to read.
void SwitchEMC::noteWindow(uint64_t busy, bool topHeld)
{
    __atomic_store_n(&busyTotal, busyTotal + busy, __ATOMIC_RELAXED);
    const bool unheldNow = busy >= holdState.busy_floor && !topHeld;
    if (unheldNow) {
        __atomic_store_n(&unheld, unheld + 1, __ATOMIC_RELAXED);
        if (unheldBefore)
            __atomic_store_n(&late, late + 1, __ATOMIC_RELAXED);
    }
    unheldBefore = unheldNow;
}

// The loop, before the rate changes or a line is taken: the time and the CPUs' busy time since the last call, at `cur`.
void SwitchEMC::account(uint64_t now)
{
    residency[cur] += now - since;
    since = now;
    const uint64_t total = __atomic_load_n(&busyTotal, __ATOMIC_RELAXED);
    busyAt[cur] += total - busyMark;
    busyMark = total;
}

IOReturn SwitchEMC::statsAction(OSObject *owner, void *line, void *size, void *written, void *)
{
    *static_cast<size_t *>(written) =
        static_cast<SwitchEMC *>(owner)->stats(static_cast<char *>(line), *static_cast<size_t *>(size));
    return kIOReturnSuccess;
}

size_t SwitchEMC::stats(char *line, size_t size)
{
    const uint64_t t = mach_absolute_time();
    if (governing)
        account(t);
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i)
        total += residency[i];
    size_t n = static_cast<size_t>(snprintf(line, size, "EMC %u moves", moves));
    for (unsigned i = 0; i < count && n < size; ++i) {
        const unsigned pm = total ? static_cast<unsigned>(residency[i] * 1000 / total) : 0;
        n += static_cast<size_t>(snprintf(line + n, size - n, " %u:%u.%u%%", khz[i] / 1000, pm / 10, pm % 10));
    }
    if (n < size) {
        n += static_cast<size_t>(
            snprintf(line + n, size - n, ", %u raises: hint to top %llu/%llu us (BPMP max %llu us)", raises,
                     raises ? raiseTicks / raises * 5 / 96 : 0, raiseMax * 5 / 96, bpmpMax * 5 / 96));
    }
    if (n < size)
        n += static_cast<size_t>(snprintf(line + n, size - n, ", cpu ms"));
    for (unsigned i = 0; i < count && n < size; ++i)
        n += static_cast<size_t>(snprintf(line + n, size - n, " %u:%llu", khz[i] / 1000, busyAt[i] / 19200));
    const uint32_t unheldNow = __atomic_load_n(&unheld, __ATOMIC_RELAXED);
    const uint32_t lateNow = __atomic_load_n(&late, __ATOMIC_RELAXED);
    if (n < size) {
        n += static_cast<size_t>(snprintf(line + n, size - n, ", %u work windows unheld (%u late), %u refused",
                                          unheldNow - unheldSeen, lateNow - lateSeen, refusals));
    }
    unheldSeen = unheldNow;
    lateSeen = lateNow;
    memset(residency, 0, sizeof(residency));
    memset(busyAt, 0, sizeof(busyAt));
    since = t;
    moves = raises = refusals = 0;
    raiseTicks = raiseMax = bpmpMax = 0;
    return n;
}
#endif
