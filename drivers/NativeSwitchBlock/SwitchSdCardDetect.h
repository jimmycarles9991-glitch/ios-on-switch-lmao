// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Active-low mechanical card-detect switch on GPIO PZ1 (0 = card inserted, 1 = removed).
constexpr bool switchSdCardRemoved(int gpioLevel)
{
    return gpioLevel > 0;
}

constexpr bool switchSdShouldPanicOnRemoval(bool armed, bool panicActive, int gpioLevel)
{
    return armed && !panicActive && switchSdCardRemoved(gpioLevel);
}
