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
