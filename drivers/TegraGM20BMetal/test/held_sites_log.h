// SPDX-License-Identifier: MIT
// The log held_sites.c keeps of a window's allocations and frees (held_sites.h reads it)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kHeldSitesFrames = 24,
    kHeldSitesThreadName = 48
};

// One allocation, free or realloc, in the order the hook ran (a block's free is logged before it can be handed out again)
struct held_sites_event {
    uintptr_t freed;      // the block given back (0: none)
    uintptr_t allocated;  // the block handed out (0: none)
    uint64_t size;
    uint32_t frames;
    uint32_t ready;                     // written last, with __atomic release
    uintptr_t frame[kHeldSitesFrames];  // the allocation's return addresses, innermost first
    bool encoding;                      // the allocation ran on the thread that started the window
    char thread[kHeldSitesThreadName];  // otherwise its thread's name, or the label of the queue it ran
};

void held_sites_start(void);
// The window's events (`count` of them), and whether more came than the log holds
const struct held_sites_event *held_sites_stop(size_t *count, bool *overflow);

#ifdef __cplusplus
}
#endif
