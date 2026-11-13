// SPDX-License-Identifier: BSD-2-Clause-Patent
// T210 runs no BPMP firmware (hekate passes it an sc7entry-fw and ATF powers it off): the boot chain's training owns
// the EMC clock and nothing here may change it. The rate CLK_SOURCE_EMC runs at is the whole table, display memory
// admission plans at it (L4T's tegra21x_la.c is the same for T210 and T210B01; only the DRAM type matters) and refuses
// a demand above it. Read again after SC7, whose warmboot restores the clock.

bool SwitchEMC::useFixedRate(unsigned now)
{
    if (!now)
        return false;
    fixed = true;
    khz[0] = now;
    count = 1;
    return true;
}

IOReturn SwitchEMC::holdFixedRate()
{
    const unsigned now = carEmcKhz();
    if (!now)
        return kIOReturnError;
    if (now != khz[0]) {
        khz[0] = now;
        memoryDirty = true;
    }
    cur = 0;
    __atomic_store_n(&gTopHeld, true, __ATOMIC_RELEASE);
    if (!programMemory(now, now, true))
        dropDisplayMemory("its plan does not cover the rate the boot chain left");
    return kIOReturnSuccess;
}
