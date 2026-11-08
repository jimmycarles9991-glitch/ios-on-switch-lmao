// SPDX-License-Identifier: MIT
// Block-linear IOSurfaces. A surface's layout is decided once, for every process, at its first GPU import that asks
// (SurfaceLayout, TegraGM20BABI.h method 16), by the shared rule (TegraGM20BLayout.h): block-linear when that importer will
// write it, its texels' shape tiles and fits the surface, and no other import of it exists (one that never asked binds it
// pitch); pitch otherwise, for good. A surface that becomes block-linear has what it holds (rows the CPU or a pitch writer
// left there) tiled in place by the copy engine, behind the GPU writers it had, before anyone is told it is tiled. From then
// on the GPU and the display read its own pages tiled (tegra_gm20b_surface_tiled); a CPU lock sees its texels as rows.
// The decision is made under the entry's lock, so concurrent askers wait for it and get the same answer. A move's
// scratch is cleaned and invalidated before the copy engine fills it: a new one was zero-filled through the CPU's cache,
// and a dirty line written back between the move's two copies would land over the rows the second one reads.
#include <IOKit/IOLib.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BRelayout.h"
#include "TegraGM20BTiled.h"
#if TGM_STATS
#include <os/log.h>
extern "C" void proc_selfname(char *buf, int size);
extern "C" int proc_selfpid(void);
#endif

namespace
{
uint32_t gTiledLive;  // atomic: surfaces block-linear now

enum Decision {
	DecidedTiled,
	PitchSampledFirst,
	PitchShared,
	PitchShape,
	PitchMoveFailed,
	Decisions
};

#if TGM_STATS
uint64_t gDecisions[Decisions], gLive, gLiveBytes;  // atomic

void note(Decision d, uint32_t surfaceId, uint32_t width, uint32_t height, uint32_t bpp)
{
	static const char *const why[Decisions] = {
		"block-linear", "pitch: its first GPU import only samples it",
		"pitch: imported before it asked", "pitch: a shape that does not tile",
		"pitch: the tiling move failed"};
	__atomic_fetch_add(&gDecisions[d], 1, __ATOMIC_RELAXED);
	char name[32];
	proc_selfname(name, sizeof(name));
	os_log(OS_LOG_DEFAULT, "tegra-gm20b: IOSurface %u %ux%u x%u bytes %s (decided by %s[%d])",
	       surfaceId, width, height, bpp, why[d], name, proc_selfpid());
}
#else
void note(Decision, uint32_t, uint32_t, uint32_t, uint32_t)
{
}
#endif

Decision decide(tgm_coh *c, tgm_tiled &t, uint32_t width, uint32_t height, uint32_t bpp,
		uint32_t bytesPerRow, bool writes)
{
	if (!writes)
		return PitchSampledFirst;
	if (!tgm_coh_alone(c))
		return PitchShared;
	const tgm_relayout_pages pages = tgm_coh_pages(c);
	const uint64_t bytes = static_cast<uint64_t>(pages.npages) << 12;
	if (!tgm_layout_tiled(width, height, bpp, &t.layout) || t.layout.bytes > bytes ||
	    bytesPerRow < width * bpp || static_cast<uint64_t>(bytesPerRow) * height > bytes)
		return PitchShape;
	t.bytes_per_row = bytesPerRow;
	tgm_relayout_pages rows;
	tgm_scratch *scratch =
		tgm_scratch_get(static_cast<uint64_t>(bytesPerRow) * height, 0, &rows);
	tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS];
	const uint32_t n = tgm_coh_writers(c, writers, TGM_RELAYOUT_MAX_WAITS);
	if (scratch)
		tgm_cpu_clean_pages(rows.pas, rows.npages);
	const int ret = scratch ? tgm_relayout(&pages, &rows, &t.layout, bytesPerRow,
					       TGM_RELAYOUT_TILE_IN_PLACE, writers, n)
				: -12;
	tgm_scratch_put(scratch);
	return ret ? PitchMoveFailed : DecidedTiled;
}
}  // namespace

extern "C" int tgm_coherency_layout(struct tgm_coh *c, uint32_t width, uint32_t height,
				    uint32_t bpp, uint32_t bytes_per_row, int writes)
{
	if (!c)
		return 0;
	tgm_coh_lock(c);
	// NOLINTNEXTLINE(misc-const-correctness): the release store below publishes through it
	tgm_tiled **slot = tgm_coh_tiled(c);
	tgm_tiled *t = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	const bool fresh = t == nullptr;
	if (fresh)
		t = IONew(tgm_tiled, 1);
	if (fresh && t) {
		bzero(t, sizeof(*t));
		const Decision d = decide(c, *t, width, height, bpp, bytes_per_row, writes != 0);
		t->tiled = static_cast<int>(d == DecidedTiled);
		if (t->tiled)
			__atomic_add_fetch(&gTiledLive, 1, __ATOMIC_ACQ_REL);
		note(d, tgm_coh_surface_id(c), width, height, bpp);
#if TGM_STATS
		if (t->tiled) {
			__atomic_fetch_add(&gLive, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&gLiveBytes, t->layout.bytes, __ATOMIC_RELAXED);
		}
#endif
		__atomic_store_n(
			slot, t,
			__ATOMIC_RELEASE);  // complete before anyone outside the entry lock reads it
	}
	int layout = 0;
	if (t && t->tiled) {
		const bool same = t->layout.width == width && t->layout.height == height &&
				  t->layout.bpp == bpp;
		layout = same ? 1 : -1;
	}
	tgm_coh_unlock(c);
	return layout;
}

extern "C" void tgm_tiled_untile(struct tgm_coh *c)
{
	tgm_coh_lock(c);
	tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE);
	if (t && t->tiled) {
		const tgm_relayout_pages pages = tgm_coh_pages(c);
		tgm_relayout_pages rows;
		tgm_scratch *scratch =
			tgm_scratch_get(uint64_t(t->bytes_per_row) * t->layout.height, 0, &rows);
		tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS];
		const uint32_t n = tgm_coh_writers(c, writers, TGM_RELAYOUT_MAX_WAITS);
		if (scratch)
			tgm_cpu_clean_pages(rows.pas, rows.npages);
		const int ret = scratch ? tgm_relayout(&pages, &rows, &t->layout, t->bytes_per_row,
						       TGM_RELAYOUT_UNTILE_IN_PLACE, writers, n)
					: -12;
		tgm_scratch_put(scratch);
		if (ret) {
			IOLog("tegra-gm20b: IOSurface %u stays block-linear with no GPU client: "
			      "its texels were not "
			      "moved back to rows (%d)\n",
			      tgm_coh_surface_id(c), ret);
		}
#if TGM_STATS
		__atomic_fetch_sub(&gLive, 1, __ATOMIC_RELAXED);
		__atomic_fetch_sub(&gLiveBytes, t->layout.bytes, __ATOMIC_RELAXED);
#endif
		__atomic_store_n(&t->tiled, 0, __ATOMIC_RELEASE);
		__atomic_sub_fetch(&gTiledLive, 1, __ATOMIC_ACQ_REL);
	}
	tgm_coh_unlock(c);
}

extern "C" int tgm_tiled_any(void)
{
	return static_cast<int>(__atomic_load_n(&gTiledLive, __ATOMIC_ACQUIRE) != 0);
}

extern "C" void tgm_tiled_free(struct tgm_tiled *t)
{
	if (!t)
		return;
	if (t->tiled)
		__atomic_sub_fetch(&gTiledLive, 1, __ATOMIC_ACQ_REL);
#if TGM_STATS
	if (t->tiled) {
		__atomic_fetch_sub(&gLive, 1, __ATOMIC_RELAXED);
		__atomic_fetch_sub(&gLiveBytes, t->layout.bytes, __ATOMIC_RELAXED);
	}
#endif
	IODelete(t, tgm_tiled, 1);
}

extern "C" void tgm_tiled_take_stats(uint64_t out[9])
{
	bzero(out, 9 * sizeof(uint64_t));
#if TGM_STATS
	for (uint32_t i = 0; i < Decisions; i++)
		out[i] = __atomic_exchange_n(&gDecisions[i], 0, __ATOMIC_RELAXED);
	out[5] = __atomic_load_n(&gLive, __ATOMIC_RELAXED);
	out[6] = __atomic_load_n(&gLiveBytes, __ATOMIC_RELAXED);
	tgm_scratch_stats(&out[7], &out[8]);
#endif
}
