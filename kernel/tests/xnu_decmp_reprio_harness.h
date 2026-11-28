// SPDX-License-Identifier: GPL-2.0-only
// Runs each XNU tree's vm_decmp_upl_reprioritize (osfmk/vm/vm_object.c, extracted by genrule) against stub allocators.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xnu_decmp_reprio_run {
    unsigned fail_kalloc;  // kalloc_data returns NULL
    unsigned fail_req_at;  // the 1-based zalloc_noblock call that returns NULL (0: none)
    unsigned blocking_allocs;
    unsigned nowait_allocs;
    unsigned enqueued;
    uint64_t enqueued_blknos[16];
};

// tree: 10063 or 12377. info: the real I/O UPL's per-page reprio words (blkno | len << 32), pages of them.
void xnu_decmp_reprio(int tree, int nowait, const uint64_t *info, unsigned pages, struct xnu_decmp_reprio_run *run);

#ifdef __cplusplus
}
#endif
