// SPDX-License-Identifier: MIT
// The kernel's registries for programs run in place from a file's pages (raw_program_pages.h): two processes import
// the same fixed grain of the pack, read-only and without a surface ID, in boImport's order (TegraGM20BUserClientBo.cpp:
// tgm_coherency_add, the pin's switch_gpu_backing_register, tgm_coherency_pin, switch_gpu_backing_cpu_track), and both
// imports get the one coherency entry and the one backing, refused by neither. An import of another page list that
// starts on the same page is refused (SwitchGpuBacking.cpp's page compare), which is why each process imports whole
// grains at fixed file offsets. The read-only retry of the wiring (TegraGM20BUserClientBo.cpp boImport) is not modelled
// here: the page lists stand for what it collects.
#include <cstring>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BTiled.h"

extern "C" {
void tgm_cpu_clean_pages(const uint64_t *, uint32_t)
{
}

int tgm_writer_done(uint32_t, uint32_t)
{
	return 1;
}

void tgm_writer_wait(uint32_t, uint32_t)
{
}

int tgm_writers_all_done(void)
{
	return 1;
}

void proc_selfname(char *buf, int size)
{
	snprintf(buf, size_t(size), "program_pages_test");
}

int proc_selfpid(void)
{
	return 1;
}

void switch_refusal(const char *, const char *, const char *, ...)
{
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

int switch_gpu_backing_prepare_retirement(void)
{
	return 0;
}

void switch_gpu_backing_schedule_retirement(void)
{
}

uint64_t switch_gpu_backing_time_ns(void)
{
	return 100;
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}

int switch_gpu_surface_prepare(uint32_t id, void *, uint32_t, switch_gpu_backing **backing,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*owned = nullptr;
	*descriptor = nullptr;
	*backing = switch_gpu_backing_find_surface(id);
	return 0;
}

void switch_gpu_surface_finish(void *)
{
}

uint32_t switch_gpu_surface_users(void *)
{
	return 0;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}

void switch_gpu_backing_retire(void);
#if SWITCH_GPU_PATH_STATS
void switch_gpu_path_taken(enum switch_gpu_path, uint32_t)
{
}
#endif
void tgm_cpu_view_hold(struct tgm_cpu_view *, int)
{
}

int tgm_cpu_view_live(const struct tgm_cpu_view *)
{
	return 0;
}

int tgm_cpu_view_of(void *, uint32_t, struct tgm_cpu_view *)
{
	return -1;
}

int tgm_cpu_view_remap(const struct tgm_cpu_view *, void *, uint64_t, int)
{
	return -1;
}

void tgm_memory_release(void *)
{
}

void tgm_relayout_by_cpu(const struct tgm_relayout_pages *, const struct tgm_relayout_pages *,
			 const struct tgm_layout *, uint32_t, int,
			 const struct tgm_relayout_writer *, uint32_t)
{
}

void tgm_scratch_discard(struct tgm_scratch *)
{
}

struct tgm_scratch *tgm_scratch_get(uint64_t, int, struct tgm_relayout_pages *)
{
	return nullptr;
}

void *tgm_scratch_map(struct tgm_scratch *, void **)
{
	return nullptr;
}

void *tgm_scratch_memory(struct tgm_scratch *)
{
	return nullptr;
}

void tgm_scratch_pages(const struct tgm_scratch *, struct tgm_relayout_pages *)
{
}

void tgm_scratch_put(struct tgm_scratch *)
{
}

void tgm_scratch_stats(uint64_t *held, uint64_t *peak)
{
	*held = *peak = 0;
}

void tgm_scratch_unmap(void *)
{
}

void *tgm_surface_memory(void *, uint32_t)
{
	return nullptr;
}

uint64_t tgm_timestamp_ns(void)
{
	return 0;
}

void tgm_writers_wait_all(void)
{
}
}

namespace
{

constexpr uint64_t kGrainPages = 0x10000 / 4096;

struct Owner {
	int released = 0;
};

void releaseOwner(void *owner)
{
	static_cast<Owner *>(owner)->released++;
}

// What one process's import of a grain holds: the coherency entry and the backing, as boImport leaves them
struct Import {
	tgm_coh *coh = nullptr;
	switch_gpu_backing *backing = nullptr;
	int covered = 0, pinned = -1, tracked = -1;
};

std::vector<uint64_t> grainPages(uint64_t firstPa)
{
	std::vector<uint64_t> pas;
	for (uint64_t i = 0; i < kGrainPages; i++)
		pas.push_back(firstPa + i * 4096);
	return pas;
}

Import import(const std::vector<uint64_t> &pas, Owner &owner)
{
	Import i;
	const uint32_t n = uint32_t(pas.size());
	i.coh = tgm_coherency_add(pas[0], 0, pas.data(), n, 0, 0, &i.covered);
	i.backing = switch_gpu_backing_register(pas.data(), n, 0, &owner, releaseOwner);
	if (i.backing) {
		i.pinned = tgm_coherency_pin(i.coh, i.backing, nullptr, nullptr);
		if (!i.pinned)
			i.tracked = switch_gpu_backing_cpu_track(i.backing);
	}
	if (i.coh && !i.covered)
		tgm_coherency_cleaned(i.coh);
	return i;
}

void release(Import &i)
{
	tgm_coherency_put(i.coh);
	switch_gpu_backing_release(i.backing);
	i = {};
}

class ProgramPages : public testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
	}

	void TearDown() override
	{
		switch_gpu_backing_retire();
	}
};

TEST_F(ProgramPages, TwoProcessesImportingOneGrainShareItsEntryAndBacking)
{
	const std::vector<uint64_t> grain = grainPages(0x90000000);
	Owner first, second;
	Import a = import(grain, first);
	Import b = import(grain, second);
	ASSERT_NE(a.coh, nullptr);
	ASSERT_NE(a.backing, nullptr);
	EXPECT_EQ(a.pinned, 0);
	EXPECT_EQ(a.tracked, 0);
	EXPECT_EQ(b.coh, a.coh) << "one coherency entry for the grain";
	EXPECT_EQ(b.backing, a.backing) << "one backing for the grain";
	EXPECT_EQ(b.pinned, 0) << "the second import's pin is not refused";
	EXPECT_EQ(b.tracked, 0);
	EXPECT_EQ(tgm_coh_find_pa(grain[0]), a.coh);
	tgm_coherency_put(a.coh);
	EXPECT_EQ(second.released, 1)
		<< "the second pin's wiring goes at once: the first one's holds the pages";
	EXPECT_EQ(first.released, 0);

	release(a);
	EXPECT_EQ(first.released, 0) << "the second process still runs from the pages";
	switch_gpu_backing *still = switch_gpu_backing_find_pa(grain[0]);
	EXPECT_EQ(still, b.backing);
	switch_gpu_backing_release(still);
	release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(first.released, 1) << "the last import releases the wiring";
	EXPECT_EQ(switch_gpu_backing_find_pa(grain[0]), nullptr);
}

TEST_F(ProgramPages, AnotherPageListFromTheSameFirstPageIsRefused)
{
	const std::vector<uint64_t> grain = grainPages(0xa0000000);
	std::vector<uint64_t> shorter(grain.begin(), grain.begin() + 2), other = grain;
	other[1] = 0xa1000000;
	Owner first, second, third;
	Import a = import(grain, first);
	ASSERT_EQ(a.pinned, 0);
	Import b = import(other, second);
	EXPECT_EQ(b.backing, nullptr)
		<< "a different page after the same first page: refused, the slot is copied";
	if (b.coh)
		tgm_coherency_put(b.coh);
	Import c = import(shorter, third);
	EXPECT_EQ(c.backing, a.backing) << "a prefix of the grain's pages is the same backing";
	release(c);
	release(a);
	switch_gpu_backing_retire();
	EXPECT_EQ(first.released, 1)
		<< "the backing goes, and releases its owner, while the owner is still there";
}

}  // namespace
