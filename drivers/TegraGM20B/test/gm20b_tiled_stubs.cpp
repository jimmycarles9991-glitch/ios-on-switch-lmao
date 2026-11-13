// The block-linear layout's and CPU views' kernel calls, inert, for host tests of the coherency registry that never tile a
// surface (gm20b_fences_test); gm20b_coherency_test has working models of them.
#include <cstdint>

#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"

extern "C" {
void switch_refusal(const char *, const char *, const char *, ...)
{
}

uint64_t tgm_timestamp_ns(void)
{
	return 0;
}

int tgm_relayout_init(void)
{
	return 0;
}

int tgm_relayout(const struct tgm_relayout_pages *, const struct tgm_relayout_pages *,
		 const struct tgm_layout *, uint32_t, int, const struct tgm_relayout_writer *,
		 uint32_t)
{
	return -1;
}

void tgm_relayout_by_cpu(const struct tgm_relayout_pages *, const struct tgm_relayout_pages *,
			 const struct tgm_layout *, uint32_t, int,
			 const struct tgm_relayout_writer *, uint32_t)
{
}

int switch_gpu_backing_wait_visible(struct switch_gpu_backing *)
{
	return 0;
}

struct tgm_scratch *tgm_scratch_get(uint64_t, int, struct tgm_relayout_pages *)
{
	return nullptr;
}

void tgm_scratch_put(struct tgm_scratch *)
{
}

void tgm_scratch_discard(struct tgm_scratch *)
{
}

struct tgm_scratch *tgm_scratch_reclaim(struct tgm_scratch *, uint64_t, int *intact)
{
	*intact = 0;
	return nullptr;
}

uint64_t tgm_scratch_handout(const struct tgm_scratch *)
{
	return 0;
}

void tgm_scratch_stats(uint64_t *held, uint64_t *peak)
{
	*held = *peak = 0;
}

void tgm_scratch_pages(const struct tgm_scratch *, struct tgm_relayout_pages *pages)
{
	pages->pas = nullptr;
	pages->npages = 0;
}

void *tgm_scratch_map(struct tgm_scratch *, void **map)
{
	*map = nullptr;
	return nullptr;
}

void tgm_scratch_unmap(void *)
{
}

void *tgm_scratch_memory(struct tgm_scratch *)
{
	return nullptr;
}

int tgm_cpu_view_of(void *, uint32_t, struct tgm_cpu_view *)
{
	return 0;
}

int tgm_cpu_view_remap(const struct tgm_cpu_view *, void *, uint64_t, int)
{
	return -1;
}

int tgm_cpu_view_live(const struct tgm_cpu_view *)
{
	return 1;
}

void tgm_cpu_view_hold(struct tgm_cpu_view *, int)
{
}

void *tgm_surface_memory(void *, uint32_t)
{
	return nullptr;
}

void tgm_memory_release(void *)
{
}
}
