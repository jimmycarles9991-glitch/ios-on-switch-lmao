// SPDX-License-Identifier: MIT
// The stats tick's "unlocked waits" line (debug kernels): each wait that used to hold a lock, by where it now runs
// without it (enum tgm_unlocked).
#include "gpu_internal.h"

#if TGM_STATS
static u32 unlocked[TGM_UNLOCKED_KINDS];
static u64 imported_ns;

void tgm_stats_unlocked(enum tgm_unlocked kind, uint64_t ns)
{
	__atomic_fetch_add(&unlocked[kind], 1, __ATOMIC_RELAXED);
	if (kind == TGM_UNLOCKED_IMPORT)
		__atomic_fetch_add(&imported_ns, ns, __ATOMIC_RELAXED);
}

void tgm_stats_unlocked_window(void)
{
	u32 n[TGM_UNLOCKED_KINDS];
	u32 any = 0;
	u32 i = 0;

	for (i = 0; i < TGM_UNLOCKED_KINDS; i++) {
		n[i] = __atomic_exchange_n(&unlocked[i], 0, __ATOMIC_RELAXED);
		any |= n[i];
	}
	if (!any)
		return;
	nvxnu_stats("tegra-gm20b: unlocked waits: %u CPU syncs beside another; ring room waited "
		    "for without the queue "
		    "lock by %u copies, %u bridge jobs; %u relayout GPU waits without the lock, %u "
		    "moves kicked "
		    "while another waited; %u imports wired before the connection lock (%llu us); "
		    "%u copy "
		    "presents found no push room",
		    n[TGM_UNLOCKED_SYNC_BESIDE], n[TGM_UNLOCKED_COPY_ROOM],
		    n[TGM_UNLOCKED_BRIDGE_ROOM], n[TGM_UNLOCKED_RELAYOUT],
		    n[TGM_UNLOCKED_RELAYOUT_BESIDE], n[TGM_UNLOCKED_IMPORT],
		    __atomic_exchange_n(&imported_ns, 0, __ATOMIC_RELAXED) / 1000,
		    n[TGM_UNLOCKED_PRESENT_ROOM]);
}
#endif
