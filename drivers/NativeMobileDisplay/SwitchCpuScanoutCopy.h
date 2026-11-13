// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU scanout copy is a fallback the display drives to zero: every copy is counted (DEVELOPMENT), and the stats
// line prints the boot total next to the window's copies.
uint64_t copyFrames = 0, copyTicksSum = 0, copyTicksMax = 0;

// Copy time of the last swap, with the running maximum and mean, at the 60th CPU copy and every 600th after
// (cntpct_el0, the architected counter at the ADT's timebase frequency).
void noteCopy(uint64_t ticks)
{
    if (!stats)
        return;
    copyFrames = copyFrames + 1;
    copyTicksSum = copyTicksSum + ticks;
    if (ticks > copyTicksMax)
        copyTicksMax = ticks;
    if (copyFrames != 60 && copyFrames % 600)
        return;
    const uint64_t hz = gPEClockFrequencyInfo.timebase_frequency_hz;
    if (!hz)
        return;
    IOLog("SwitchDisplay: CPU scanout copy %llu us, max %llu us, mean %llu us over %llu swaps (%s, %llu waited for a "
          "flip)\n",
          ticks * 1000000 / hz, copyTicksMax * 1000000 / hz, copyTicksSum / copyFrames * 1000000 / hz, copyFrames,
          doubleBuffer ? "double-buffered"
          : hwRotate   ? "block-linear"
                       : "rotating",
          flipWaits);
}
