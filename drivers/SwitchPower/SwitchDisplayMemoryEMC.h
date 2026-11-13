// SPDX-License-Identifier: BSD-2-Clause-Patent

// The display's bandwidth demand: the reservation or its first head's plan, and the second head's.
uint64_t SwitchEMC::floorBytes() const
{
    uint64_t bytes = __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE);
    if (memoryState.owned[0])
        bytes = switch_display::memoryMax(bytes, memoryState.heads[0].peak_bytes_per_second);
    if (memoryState.owned[1])
        bytes = switch_display::add(bytes, memoryState.heads[1].peak_bytes_per_second);
    return bytes;
}

unsigned SwitchEMC::memoryFloor(unsigned first) const
{
    const unsigned bandwidth = switch_display::row(khz, count, floorBytes());
    if (first < bandwidth)
        first = bandwidth;
    return memoryState.floor(khz, count, first, khz[cur]);
}

bool SwitchEMC::programMemory(uint32_t before, uint32_t after, bool force)
{
    if (!memoryState.owned[0] && !memoryState.owned[1])
        return true;
    if (!force && !memoryDirty && memoryAppliedKhz == after)
        return true;
    if (!__atomic_load_n(&memoryQualified, __ATOMIC_ACQUIRE) || !memoryState.program(mcRegs, before, after))
        return false;
    memoryAppliedKhz = after;
    memoryDirty = false;
    return true;
}

IOReturn SwitchEMC::memoryAction(OSObject *owner, void *argument, void *result, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    const auto &request = *static_cast<const switch_display_memory_head *>(argument);
    int error = __atomic_load_n(&self->memoryQualified, __ATOMIC_ACQUIRE) ? 0 : -95;
    if (request.head > 1)
        error = -22;
    if (!error && !self->memoryState.same(request)) {
        const auto old = self->memoryState;
        auto next = old;
        next.heads[request.head] = request;
        next.owned[request.head] = true;
        uint64_t bytes =
            next.owned[0] ? next.heads[0].peak_bytes_per_second : __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE);
        if (next.owned[1])
            bytes = switch_display::add(bytes, next.heads[1].peak_bytes_per_second);
        unsigned floor = switch_display::row(self->khz, self->count, bytes);
        while (floor < self->count) {
            switch_display::MemoryPlan plans[2];
            if (next.calculate(self->khz[floor], plans))
                break;
            ++floor;
        }
        if (floor >= self->count || (floor > self->cur && !self->moveTo(floor, SWITCH_EMC_REASON_DISPLAY_MEMORY))) {
            error = -28;
        } else {
            self->memoryState = next;
            self->memoryDirty = true;
            if (!self->programMemory(self->khz[self->cur], self->khz[self->cur], true)) {
                self->memoryState = old;
                self->memoryDirty = true;
                (void)self->programMemory(self->khz[self->cur], self->khz[self->cur], true);
                error = -28;
            } else {
                if (!request.head)
                    __atomic_store_n(&gDisplayBytes, request.peak_bytes_per_second, __ATOMIC_RELEASE);
                if (self->governing)
                    self->program();
            }
        }
    }
    *static_cast<int *>(result) = error;
    return kIOReturnSuccess;
}

int SwitchEMC::reserveMemory(const switch_display_memory_head &request)
{
    int error = 0;
    loop->runAction(memoryAction, this, const_cast<switch_display_memory_head *>(&request), &error);
    return error;
}

// A plan that no longer covers the running rate turns display memory admission off for the boot: the display shows the
// UI alone (window A needs no admission) and its overlays are dropped, never a panic.
void SwitchEMC::dropDisplayMemory(const char *why)
{
    if (__atomic_exchange_n(&memoryQualified, false, __ATOMIC_ACQ_REL))
        os_log_error(OS_LOG_DEFAULT, "SwitchEMC: display memory admission off: %s", why);
    memoryState = {};
    memoryDirty = false;
}

extern "C" int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    if (!request)
        return -22;
    SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    return emc ? emc->reserveMemory(*request) : -19;
}

extern "C" int switch_emc_display_memory_ready(void)
{
    const SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    return emc && emc->displayMemoryReady();
}
