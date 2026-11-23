// SPDX-License-Identifier: MIT
// One connection's fence page as the kernel writes it (TegraGM20BFencePageABI.h): each power-up's clock anchor into it
// (gpu_anchor.c's tgm_clock_anchor_mirror) and its writer promises, made, kept by their job's release at the address
// keepPromises names and cancelled from the CPU (SwitchGpuWriterPromise.h over TegraGM20BUserClientInternal.h's
// promiseWord). Each writes bytes of its own region alone.
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"
#include "drivers/TegraGM20B/TegraGM20BClockABI.h"
#include "drivers/TegraGM20B/TegraGM20BFencePageABI.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"

namespace
{
constexpr size_t kSlotBytes = 16 * 1024;  // gpu_internal.h TGM_FENCE_SLOT

// A fence slot as tgm_fence_page_get hands it out: zeroed
struct Page {
	std::vector<uint8_t> bytes = std::vector<uint8_t>(kSlotBytes);

	TegraGM20BAbiClockAnchors *anchors()
	{
		return reinterpret_cast<TegraGM20BAbiClockAnchors *>(bytes.data() +
								     TegraGM20BClockAnchorOffset);
	}

	volatile uint32_t *word(uint32_t index)
	{
		return reinterpret_cast<volatile uint32_t *>(bytes.data()) + index;
	}

	std::vector<uint8_t> region(uint32_t first, uint32_t count) const
	{
		return {bytes.begin() + first, bytes.begin() + first + count};
	}

	// Bytes that differ from `before` outside [first, first + count)
	size_t changedOutside(const std::vector<uint8_t> &before, uint32_t first,
			      uint32_t count) const
	{
		size_t changed = 0;
		for (size_t i = 0; i < bytes.size(); ++i)
			changed += (i < first || i >= first + count) && bytes[i] != before[i];
		return changed;
	}
};

// gpu_anchor.c: the kernel's own table, each power-up's anchor published into the page
struct Anchors {
	TegraGM20BAbiClockAnchors kept = {};

	static TegraGM20BAbiClockAnchor of(uint32_t n)
	{
		return {20000000000ull + n * 1000000000ull, 15000000000ull + n * 2000000000ull};
	}

	void publish(Page &page)
	{
		const uint32_t n = kept.published;
		*tgm_clock_anchor_slot(&kept, n) = of(n);
		kept.published = n + 1;
		tgm_clock_anchor_mirror(page.anchors(), &kept, n);
	}
};

// TegraGM20BUserClientPromise.cpp's promises of the page's connection
struct Promises {
	Page &page;
	switch_gpu_promise_slot slots[TegraGM20BMaxPromises] = {};
	uint32_t made[TegraGM20BMaxPromises] = {}, raising = 0;
	switch_gpu_promise_claim claim = {made, &raising, TegraGM20BPromiseWord * 4};

	volatile uint32_t *promiseWord(uint32_t promise)
	{
		return page.word(TegraGM20BPromiseWord + promise);
	}

	int make()
	{
		return switch_gpu_promise_make(slots, promiseWord(0), TegraGM20BMaxPromises,
					       &claim);
	}

	// Its job submitted, then the job's fence push releasing it: a semaphore release of the value at fence_gpu_ +
	// (TegraGM20BPromiseWord + promise) * 4
	void keep(uint32_t promise)
	{
		switch_gpu_promise_kept(&slots[promise], 0);
		*page.word(TegraGM20BPromiseWord + promise) = slots[promise].value;
	}

	void cancel(uint32_t promise)
	{
		switch_gpu_promise_release(&slots[promise], promiseWord(promise));
	}

	std::vector<uint32_t> words()
	{
		std::vector<uint32_t> out;
		for (uint32_t i = 0; i < TegraGM20BMaxPromises; ++i)
			out.push_back(uint32_t(*promiseWord(i)));
		return out;
	}
};

constexpr uint32_t kBootAndWakes = 3;
}  // namespace

// A power-up while promises wait for their jobs (the GPU rail-gates with the display off and comes back for a reader's
// job): its anchor neither releases nor moves any of them, through every slot of the table twice.
TEST(SwitchGpuFencePage, APowerUpsAnchorMovesNoPromiseWord)
{
	Page page;
	Anchors anchors;
	for (uint32_t n = 0; n < kBootAndWakes; ++n)
		anchors.publish(page);
	Promises promises{page};
	for (uint32_t i = 0; i < TegraGM20BMaxPromises; ++i)
		ASSERT_EQ(promises.make(), int(i));
	const std::vector<uint32_t> pending = promises.words();
	for (uint32_t n = kBootAndWakes; n < kBootAndWakes + 2 * TegraGM20BClockAnchorSlots; ++n) {
		const std::vector<uint8_t> before = page.bytes;
		anchors.publish(page);
		EXPECT_EQ(promises.words(), pending) << "anchor " << n;
		EXPECT_EQ(page.changedOutside(before, TegraGM20BFencePageClockAnchorsFirst,
					      TegraGM20BFencePageClockAnchorsBytes),
			  0u)
			<< "anchor " << n;
	}
}

// Every promise kept by its job or cancelled, twice over: the anchor table the client and the GPU read stays as the
// kernel published it, and a sample of the last power-up still takes that power-up's anchor.
TEST(SwitchGpuFencePage, APromiseReleasedLeavesTheClockAnchors)
{
	Page page;
	Anchors anchors;
	for (uint32_t n = 0; n < kBootAndWakes; ++n)
		anchors.publish(page);
	const std::vector<uint8_t> table = page.region(TegraGM20BFencePageClockAnchorsFirst,
						       TegraGM20BFencePageClockAnchorsBytes);
	Promises promises{page};
	for (uint32_t round = 0; round < 2; ++round) {
		for (uint32_t i = 0; i < TegraGM20BMaxPromises; ++i)
			ASSERT_EQ(promises.make(), int(i));
		for (uint32_t i = 0; i < TegraGM20BMaxPromises; ++i) {
			const std::vector<uint8_t> before = page.bytes;
			if (i % 2)
				promises.cancel(i);
			else
				promises.keep(i);
			EXPECT_EQ(page.region(TegraGM20BFencePageClockAnchorsFirst,
					      TegraGM20BFencePageClockAnchorsBytes),
				  table)
				<< "promise " << i;
			EXPECT_EQ(page.changedOutside(before, TegraGM20BFencePagePromisesFirst,
						      TegraGM20BFencePagePromisesBytes),
				  0u)
				<< "promise " << i;
		}
	}
	const TegraGM20BAbiClockAnchor last = Anchors::of(kBootAndWakes - 1);
	TegraGM20BAbiClockAnchor found = {};
	ASSERT_TRUE(tgm_clock_anchor_of(page.anchors(), page.anchors()->published, last.units + 1,
					&found));
	EXPECT_EQ(found.units, last.units);
	EXPECT_EQ(found.ns, last.ns);
}
