// SPDX-License-Identifier: MIT
// The GPU clock's anchors (TegraGM20BClockABI.h): one reading of PTIMER against the CPU's clock per power-up, mirrored
// into every connection's fence page.
#include "gpu_internal.h"
#include "TegraGM20BClockABI.h"

static DEFINE_MUTEX(anchors_lock);
static struct TegraGM20BAbiClockAnchors anchors;

static struct TegraGM20BAbiClockAnchor anchor_read(struct nvkm_device *device)
{
	struct TegraGM20BAbiClockAnchor best = {0, 0};
	u64 narrowest = U64_MAX;
	int i = 0;

	for (i = 0; i < 3; i++) {
		u64 before = nvxnu_nanotime();
		u64 units = nvkm_timer_read(device->timer);
		u64 after = nvxnu_nanotime();

		if (after - before < narrowest) {
			narrowest = after - before;
			best.units = units;
			best.ns = before + (after - before) / 2;
		}
	}
	return best;
}

static struct TegraGM20BAbiClockAnchors *page_anchors(u32 page)
{
	return (struct TegraGM20BAbiClockAnchors *)((u8 *)tgm_fence_pages.va +
						    (size_t)page * TGM_FENCE_SLOT +
						    TegraGM20BClockAnchorOffset);
}

/* anchors_lock held: anchor n of the table into `to`, then its count */
static void mirror(struct TegraGM20BAbiClockAnchors *to, u32 n)
{
	*tgm_clock_anchor_slot(to, n) = *tgm_clock_anchor_slot(&anchors, n);
	__atomic_store_n(&to->published, n + 1, __ATOMIC_RELEASE);
}

/* Right after a power-up's timer init (and its reseed), before any channel runs: every GPU time of the power-up is at or
 * above this anchor */
void tgm_clock_anchor_publish(struct nvkm_device *device)
{
	struct TegraGM20BAbiClockAnchor a = anchor_read(device);
	u32 n = 0;
	u32 page = 0;

	mutex_lock(&anchors_lock);
	n = anchors.published;
	*tgm_clock_anchor_slot(&anchors, n) = a;
	anchors.published = n + 1;
	for (page = 0; tgm_fence_pages.va && page < TGM_FENCE_USER_PAGES; page++)
		mirror(page_anchors(page), n);
	mutex_unlock(&anchors_lock);
}

/* A fence page just handed out and zeroed (tgm_fence_page_get): every anchor kept */
void tgm_clock_anchors_fill(u32 page)
{
	struct TegraGM20BAbiClockAnchors *to = page_anchors(page);
	u32 n = 0;

	mutex_lock(&anchors_lock);
	for (n = anchors.published > TegraGM20BClockAnchorSlots
			 ? anchors.published - TegraGM20BClockAnchorSlots
			 : 0;
	     n < anchors.published; n++)
		mirror(to, n);
	mutex_unlock(&anchors_lock);
}
