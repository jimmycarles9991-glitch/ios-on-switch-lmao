/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "clock.h"

extern void switch_a3_tsf_read(void *wlc, uint32_t *low, uint32_t *high);

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (!wlc || !tsf)
        return -2;
    uint32_t low, high, previous;
    switch_a3_tsf_read(wlc, &low, &previous);
    switch_a3_tsf_read(wlc, &low, &high);
    if (high != previous) {
        previous = high;
        switch_a3_tsf_read(wlc, &low, &high);
        if (high != previous)
            return -35;
    }
    *tsf = ((uint64_t)high << 32) | low;
    return 0;
}
