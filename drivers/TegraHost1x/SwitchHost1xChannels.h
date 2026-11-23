// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp's command DMA channels (SwitchHost1xChannels.cpp): each channel's push buffer of GATHERs and its
// command arena. `lock` is host1x's submit lock, which program and stop are called under.
#pragma once
#include <stdint.h>
#include <IOKit/IOLocks.h>

bool switch_host1x_channels_alloc(volatile uint8_t *host, volatile uint8_t *sync, IOLock *lock);
// Boot and resume: every channel stopped, emptied and started again on its push buffer; submits are taken from here on.
void switch_host1x_channels_program();
// The channels (bit n: channel n) whose gather filter stayed on when program cleared it: their streams' host1x-class
// waits would not run.
uint32_t switch_host1x_channels_filter_stuck();
// Suspend: every channel stopped; submits fail until program.
void switch_host1x_channels_stop();

// Under the submit lock, host1x up, once a failed producer's cancellation is stored: the display stream that waits on
// it is cancelled, then the producer's syncpoint is released to `threshold`. An increment that does not take panics.
void switch_host1x_channels_release_failed(uint32_t id, uint32_t threshold);
