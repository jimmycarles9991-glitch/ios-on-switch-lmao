// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>

struct SwitchSdWindow {
    uint64_t first = 0;
    uint64_t count = 0;

    bool contains(uint64_t lba, uint64_t sectors) const
    {
        return sectors && lba >= first && sectors <= count && lba - first <= count - sectors && lba <= UINT32_MAX &&
               sectors - 1 <= UINT32_MAX - lba;
    }
};
