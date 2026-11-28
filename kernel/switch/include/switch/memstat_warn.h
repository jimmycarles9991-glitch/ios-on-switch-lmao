// SPDX-License-Identifier: GPL-2.0-only
/*
 * When a daemon that goes clean before its idle exit is warned (bsd/kern/kern_memorystatus.c memorystatus_dirty_set, both
 * trees): its footprint against the inactive limit its demotion is about to apply. Free of kernel types so kernel/tests
 * can drive it.
 *
 * memorystatus_warn_process keeps no one-shot state, and posts to every pressure knote of the process, Apple's too, so a
 * warning at every clean transition runs every release a daemon has once per transaction. As the ledger warns once per
 * crossing of its running limit, a process is warned again only once its footprint has grown a step past the footprint it
 * was last warned at: one that gave back and grew again within one deferral window is warned before the demotion, one
 * that stays high is warned once. Applying the inactive limit clears the mark (the ledger warns from then on).
 */
#ifndef SWITCH_MEMSTAT_WARN_H
#define SWITCH_MEMSTAT_WARN_H

#include <stdbool.h>
#include <stdint.h>

/* the share of the limit warned at: the ledger's PHYS_FOOTPRINT_WARNING_LEVEL (osfmk/kern/task.c) */
#define SWITCH_MEMSTAT_WARNING_LEVEL 80
/* the growth past the last warning that warns again, in hundredths of the limit */
#define SWITCH_MEMSTAT_WARNING_STEP 5

static inline bool switch_memstat_inactive_warn(uint64_t footprint, uint64_t limit_bytes, uint64_t *warned)
{
    if (limit_bytes == 0 || footprint < limit_bytes / 100 * SWITCH_MEMSTAT_WARNING_LEVEL) {
        return false;
    }
    if (*warned != 0 && footprint < *warned + limit_bytes / 100 * SWITCH_MEMSTAT_WARNING_STEP) {
        return false;
    }
    *warned = footprint;
    return true;
}

#endif /* SWITCH_MEMSTAT_WARN_H */
