// SPDX-License-Identifier: MIT
// The raise/remake guard (SwitchGpuWriterPromise.h switch_gpu_promise_claim), run as the kernel builds the raise
// (gpu_promise_giveup.c's promise_raise, cut out by :promise_raise_source): its steps go through the header's named
// helpers, which this test wraps, so a maker's whole round lands at each boundary of the raise, as another core's would.
// No word ever goes down, and the promise given up still ends released. The same rounds with the guard taken out lower a
// word, so the test sees what the guard is for.
#include <gtest/gtest.h>
#include <functional>
#include <string>
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"
#include "switch_gpu_interrupts_off_stubs.h"

typedef int32_t s32;
#define TGM_FENCE_SLOT (16 * 1024)
#define TGM_FENCE_USER_PAGES 64
#define TGM_FENCE_PROMISE_WORD 512
#define TGM_FENCE_PROMISES 32

struct {
	uint32_t made[TGM_FENCE_USER_PAGES][TGM_FENCE_PROMISES];
	uint32_t raising;
} tgm_fence_pages;

namespace guard
{
inline int at = -1;  // the boundary the maker's round lands at
inline int reached = 0;
inline std::function<void()> round;
inline bool off = false;  // the guard taken out: no mark, and `made` never checked

inline void boundary()
{
	if (reached++ == at && round)
		round();
}

inline void mark(uint32_t *raising, uint32_t offset)
{
	boundary();
	if (!off)
		(switch_gpu_promise_raise_mark)(raising, offset);
	boundary();
}

inline int madeStill(const uint32_t *made, uint32_t value)
{
	const int still = off || (switch_gpu_promise_raise_made_still)(made, value);
	boundary();
	return still;
}

inline int wordBelow(const volatile uint32_t *word, uint32_t value)
{
	const int below = (switch_gpu_promise_raise_word_below)(word, value);
	boundary();
	return below;
}

inline void unmark(uint32_t *raising)
{
	boundary();
	(switch_gpu_promise_raise_unmark)(raising);
	boundary();
}
}  // namespace guard

#define switch_gpu_promise_raise_mark guard::mark
#define switch_gpu_promise_raise_made_still guard::madeStill
#define switch_gpu_promise_raise_word_below guard::wordBelow
#define switch_gpu_promise_raise_unmark guard::unmark
#include "drivers/TegraGM20B/test/promise_raise_source.h"
#undef switch_gpu_promise_raise_mark
#undef switch_gpu_promise_raise_made_still
#undef switch_gpu_promise_raise_word_below
#undef switch_gpu_promise_raise_unmark

namespace
{
constexpr uint32_t kSlots = TGM_FENCE_PROMISES, kFirst = TGM_FENCE_PROMISE_WORD * 4;
constexpr int kBoundaries =
	6;  // before and after the mark, after the made check, after the look, around the unmark

volatile uint32_t page[TGM_FENCE_PROMISE_WORD + TGM_FENCE_PROMISES];

// The connection on fence page 0: its slots and their words, the guard's row, and the highest value it wrote to each
struct Maker {
	switch_gpu_promise_slot slots[kSlots] = {};
	volatile uint32_t *words = &page[TGM_FENCE_PROMISE_WORD];
	switch_gpu_promise_claim claim = {tgm_fence_pages.made[0], &tgm_fence_pages.raising,
					  kFirst};
	uint32_t highest[kSlots] = {};

	Maker()
	{
		for (auto &word : page)
			word = 0;
		for (auto &made : tgm_fence_pages.made[0])
			made = 0;
		tgm_fence_pages.raising = 0;
		for (uint32_t i = 0; i < kSlots; ++i)
			words[i] = 40;
	}

	int make()
	{
		return switch_gpu_promise_make(slots, words, kSlots, &claim);
	}

	// its job's fence push, or its own cancel's store
	void release(int slot, uint32_t value)
	{
		words[slot] = value;
		highest[slot] = std::max(highest[slot], value);
	}

	// the job that keeps the promise runs and releases it, and the slot is made again, kept and released
	void gpuRound(int slot)
	{
		switch_gpu_promise_kept(&slots[slot], 0);
		release(slot, slots[slot].value);
		const int again = make();
		ASSERT_GE(again, 0);
		switch_gpu_promise_kept(&slots[again], 0);
		release(again, slots[again].value);
	}

	// no job: a cancel, a make and a cancel again, on the CPU alone
	void cpuRound(int slot)
	{
		release(slot, slots[slot].value);
		slots[slot].state = SWITCH_GPU_PROMISE_FREE;
		const int again = make();
		ASSERT_GE(again, 0);
		release(again, slots[again].value);
		slots[again].state = SWITCH_GPU_PROMISE_FREE;
	}

	// a word below the highest value written to it went down
	bool lowered() const
	{
		for (uint32_t i = 0; i < kSlots; ++i)
			if (int32_t(words[i] - highest[i]) < 0)
				return true;
		return false;
	}
};

struct Outcome {
	bool lowered, released;
};

// The give-up raises slot 0's promise while the maker's round lands at `at`
Outcome raiseWith(int at, bool gpu, bool off)
{
	Maker maker;
	const int slot = maker.make();
	EXPECT_EQ(slot, 0);
	const uint32_t value = maker.slots[0].value;
	guard::at = at;
	guard::reached = 0;
	guard::off = off;
	guard::round = [&] {
		if (gpu)
			maker.gpuRound(0);
		else
			maker.cpuRound(0);
	};
	promise_raise(kFirst, &maker.words[0], value);
	guard::round = nullptr;
	guard::at = -1;
	guard::off = false;
	EXPECT_EQ(tgm_fence_pages.raising, 0u);
	return {maker.lowered(), int32_t(maker.words[0] - value) >= 0};
}
}  // namespace

// Wherever the maker's round lands in the raise, by its GPU or on the CPU alone, no word goes down, and the promise
// given up ends released.
TEST(PromiseGuard, NoRoundAnywhereInTheRaiseLowersAWord)
{
	for (const bool gpu : {true, false}) {
		for (int at = 0; at < kBoundaries; ++at) {
			const Outcome run = raiseWith(at, gpu, false);
			EXPECT_FALSE(run.lowered)
				<< (gpu ? "GPU" : "CPU-only") << " round at boundary " << at;
			EXPECT_TRUE(run.released)
				<< (gpu ? "GPU" : "CPU-only") << " round at boundary " << at;
		}
	}
}

// With the guard taken out, a round between the raise's look and its store lowers the remade promise's word: the
// window the guard closes, by either route.
TEST(PromiseGuard, WithoutTheGuardARoundBetweenTheLookAndTheStoreLowersAWord)
{
	for (const bool gpu : {true, false}) {
		const Outcome run = raiseWith(3, gpu, true);
		EXPECT_TRUE(run.lowered) << (gpu ? "GPU" : "CPU-only");
	}
}

// A make that finds its slot's word being raised takes another slot, and `made` is back as the raise read it.
TEST(PromiseGuard, AMakeOnTheWordBeingRaisedTakesAnotherSlot)
{
	Maker maker;
	ASSERT_EQ(maker.make(), 0);
	const uint32_t value = maker.slots[0].value;
	maker.release(0, value);
	maker.slots[0].state = SWITCH_GPU_PROMISE_FREE;
	tgm_fence_pages.raising = kFirst;
	EXPECT_EQ(maker.make(), 1);
	EXPECT_EQ(tgm_fence_pages.made[0][0], value);
	tgm_fence_pages.raising = 0;
	EXPECT_EQ(maker.make(), 0);
	EXPECT_EQ(tgm_fence_pages.made[0][0], value + 1);
}
