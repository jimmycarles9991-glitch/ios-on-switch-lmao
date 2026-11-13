// SPDX-License-Identifier: MIT
// The give-up's one call and its table (SwitchGpuPromiseGiveUp.h), under the same kernel as
// switch_gpu_promise_giveup_test: what it costs at idle and at a pass, where it runs, and that every promise slot that can
// exist, and every GPU reader of one, is given up at its second.
#include "switch_gpu_promise_giveup_fixture.h"

namespace
{
using namespace promise_giveup;

unsigned readerSetsTaken()
{
	unsigned taken = 0;
	for (const uint64_t word : giveups.reader_sets_taken)
		taken += unsigned(__builtin_popcountll(word));
	return taken;
}

class Call : public GiveUp
{
};

// Idle: no reader parked, nothing armed, and the interrupt pass costs a load.
TEST_P(Call, NothingIsArmedAtIdle)
{
	for (int i = 0; i < 100; ++i)
		pass();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(fake::call.armed + fake::call.immediate + fake::call.cancelled + fake::call.ran,
		  0u);
}

// After the keeping job's pass cancelled the call, an interrupt pass costs one load: no simple lock, no tgm_live_lock.
// A keep takes the ring's simple lock alone, never tgm_live_lock, until a reader parks again.
TEST_P(Call, ACancelledCallCostsALoadAtEachPassAndNoLiveLockAtAKeep)
{
	readerSubmits();
	makerKeeps(0);
	makerReleases();
	ASSERT_EQ(fake::call.cancelled, 1u);
	const unsigned saves = interrupts_off::saves, takes = tgm_live_takes;
	for (int i = 0; i < 50; ++i)
		pass();
	EXPECT_EQ(interrupts_off::saves - saves, 50u)
		<< "the test's own interrupts-off pass, nothing inside it";
	for (int i = 0; i < 5; ++i) {
		const switch_gpu_backing_token job = {maker.submitted, 1, SWITCH_GPU_ENGINE_GM20B,
						      uint32_t(maker.id)};
		tgm_promise_kept(kOffset + 4, kValue + uint32_t(i), &maker, &job);
	}
	EXPECT_EQ(interrupts_off::saves - saves, 55u) << "each keep's ring under the simple lock";
	EXPECT_EQ(tgm_live_takes, takes);
	EXPECT_EQ(fake::call.ran, 0u);
}

// A direct present's promise whose word moved unheard is handed to it by whichever drop finds it, here the sweep of the
// next park.
TEST_P(Call, EveryDropOfAMovedWordTheDirectPresentWaitsOnHandsItOver)
{
	ASSERT_EQ(tgm_promise_park(kOffset, kValue, "the display", "a direct present", nullptr, 1),
		  1);
	fake::pool[kOffset / 4] = kValue;
	fake::pool[(kOffset + 4) / 4] = kValue - 1;
	tgm_fence_pages.made[0][(kOffset + 4) / 4 - TGM_FENCE_PROMISE_WORD] = kValue;
	const switch_gpu_backing_token other = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset + 4};
	tgm_chan_park_promises(&reader, &other, 1);
	EXPECT_EQ(presentReleased, 1u);
}

// Every promise slot that can exist, each waited on at once (a GPU reader on each): every one has its own record, so
// each is given up a second after its park, the 33rd and the 64th and the 2048th as the first, with its own line.
TEST_P(Call, EveryPromiseSlotWaitedOnAtOnceIsGivenUpAtOneSecond)
{
	std::vector<uint32_t> offsets;
	for (uint32_t page = 0; page < TGM_FENCE_USER_PAGES; ++page)
		for (uint32_t slot = 0; slot < TegraGM20BMaxPromises; ++slot)
			offsets.push_back(page * TGM_FENCE_SLOT +
					  (TGM_FENCE_PROMISE_WORD + slot) * 4);
	ASSERT_EQ(offsets.size(), size_t(SWITCH_GPU_GIVEUPS));
	for (const uint32_t offset : offsets) {
		fake::pool[offset / 4] = kValue - 1;
		tgm_fence_pages.made[offset / TGM_FENCE_SLOT]
				    [offset % TGM_FENCE_SLOT / 4 - TGM_FENCE_PROMISE_WORD] = kValue;
		const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE,
							  offset};
		tgm_chan_park_promises(&reader, &promise, 1);
	}
	reader.submitted += 1;
	EXPECT_EQ(readerSetsTaken(), unsigned(SWITCH_GPU_GIVEUP_READER_SETS))
		<< "the promises past the last reader set count their readers as parked";
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	EXPECT_EQ(lines.size(), size_t(SWITCH_GPU_GIVEUPS));
	EXPECT_EQ(readerSetsTaken(), 0u);
	for (const uint32_t offset : offsets)
		EXPECT_EQ(fake::pool[offset / 4], kValue) << offset;
	ASSERT_GE(lines.size(), 64u);
	EXPECT_NE(lines[32].find("waited 1000 ms"), std::string::npos) << lines[32];
	EXPECT_NE(lines[63].find("waited 1000 ms"), std::string::npos) << lines[63];
	EXPECT_EQ(fake::call.ran, 1u);
}

// More GPU reader channels than a record tells apart, one of them with two jobs parked: the promise is still given up a
// second after the first park, with one line, and every reader's acquire passes.
TEST_P(Call, SixGpuReaderChannelsAreAllLetGoAtOneSecond)
{
	fake_tgm_chan more[5];
	const char *const names[] = {"Photos", "Camera", "backboardd", "present", "host1x-bridge"};
	const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
	readerSubmits();
	for (int i = 0; i < 5; ++i) {
		setChan(more[i], 0x40 + i, names[i], 10);
		fake::chans.push_back(&more[i]);
	}
	for (auto &chan : more) {
		tgm_chan_park_promises(&chan, &promise, 1);
		chan.submitted += 1;
	}
	tgm_chan_park_promises(&more[0], &promise, 1);
	more[0].submitted += 1;
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("Camera waited 1000 ms"), std::string::npos) << lines[0];
	EXPECT_EQ(fake::pool[kOffset / 4], kValue) << "every reader's acquire passes";
	EXPECT_EQ(fake::call.ran, 1u);
	EXPECT_FALSE(fake::call.pending);
}

// The channels the record tells apart die; the readers it could not tell apart are still parked, and the promise is given
// up at its second all the same: nothing pins it.
TEST_P(Call, ReadersBeyondTheRecordNeverPinThePromise)
{
	fake_tgm_chan more[5];
	const char *const names[] = {"Photos", "Camera", "backboardd", "present", "host1x-bridge"};
	const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
	readerSubmits();
	for (int i = 0; i < 5; ++i) {
		setChan(more[i], 0x40 + i, names[i], 10);
		fake::chans.push_back(&more[i]);
		tgm_chan_park_promises(&more[i], &promise, 1);
		more[i].submitted += 1;
	}
	reader.error = 1;
	for (int i = 0; i < 3; ++i)
		more[i].error = 1;
	until(fake::now + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_FALSE(fake::call.pending);
}

// A fifth channel parks once a reader the record told apart has passed its acquire: that one is dropped to make room,
// so the record still knows when every reader left, and nothing is given up.
TEST_P(Call, AFifthChannelTakesTheRoomOfAReaderThatWentOn)
{
	fake_tgm_chan more[4];
	const char *const names[] = {"Photos", "Camera", "backboardd", "present"};
	const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
	readerSubmits();
	for (int i = 0; i < 4; ++i) {
		setChan(more[i], 0x40 + i, names[i], 10);
		fake::chans.push_back(&more[i]);
	}
	for (int i = 0; i < 3; ++i) {
		tgm_chan_park_promises(&more[i], &promise, 1);
		more[i].submitted += 1;
	}
	reader.error = 1;
	tgm_chan_park_promises(&more[3], &promise, 1);
	more[3].submitted += 1;
	for (auto &chan : more)
		chan.error = 1;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_FALSE(fake::call.pending);
}

// The slot's promise is kept with no pass to see it, and the slot made again: a reader of the new promise takes the
// slot's record over, and the new promise has its own second.
TEST_P(Call, ASlotMadeAgainHasItsRecordForTheNewPromise)
{
	readerSubmits();
	fake::pool[kOffset / 4] = kValue;
	gpuRuns();
	until(fake::now + 500 * kMs);
	tgm_fence_pages.made[0][kOffset / 4 - TGM_FENCE_PROMISE_WORD] = kValue + 1;
	const switch_gpu_backing_token next = {kValue + 1, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
	const uint64_t parked = fake::now;
	tgm_chan_park_promises(&reader, &next, 1);
	reader.submitted += 1;
	until(parked + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(parked + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("(value 10)"), std::string::npos) << lines[0];
	EXPECT_EQ(fake::pool[kOffset / 4], kValue + 1);
}

// The page's earlier owner kept a promise of the same slot and value; the page then went to another connection,
// cleared, whose promise of that value nobody keeps. Its reader is not handed the earlier owner's job: nothing is asked
// of that channel, and the promise is given up at its second.
TEST_P(Call, AKeepOfThePagesEarlierOwnerIsNotTheNewPromisesJob)
{
	makerKeeps(1);
	fake::pool[kOffset / 4] = kValue;
	++fake::owner;
	fake::pool[kOffset / 4] = kValue - 1;
	const auto asked = maker.asked;
	readerSubmits();
	EXPECT_EQ(maker.asked, asked);
	until(fake::now + 1000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::hangLooks, 0u);
}

// More GPU reader channels parked than the record tells apart, and then every one of them dies: the promise is given up
// at its second with its line (the untold readers are taken as parked until then), never kept waiting on.
TEST_P(Call, ReadersBeyondTheRecordThatAllDiedAreGivenUpAtTheSecond)
{
	fake_tgm_chan more[5];
	const char *const names[] = {"Photos", "Camera", "backboardd", "present", "host1x-bridge"};
	const switch_gpu_backing_token promise = {kValue, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
	readerSubmits();
	for (int i = 0; i < 5; ++i) {
		setChan(more[i], 0x40 + i, names[i], 10);
		fake::chans.push_back(&more[i]);
		tgm_chan_park_promises(&more[i], &promise, 1);
		more[i].submitted += 1;
	}
	reader.error = 1;
	for (auto &chan : more)
		chan.error = 1;
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_FALSE(fake::call.pending);
}

// Every way a record ends gives its reader set back. Twice as many promises as there are sets go through each, one
// after another: the word moved, the give-up, the readers gone, the slot made again, the last waiter leaving.
TEST_P(Call, EveryWayARecordEndsGivesItsReaderSetBack)
{
	constexpr int kRounds = 2 * SWITCH_GPU_GIVEUP_READER_SETS;
	uint32_t value = kValue;
	auto park = [&](uint32_t v) {
		fake::pool[kOffset / 4] = v - 1;
		tgm_fence_pages.made[0][kOffset / 4 - TGM_FENCE_PROMISE_WORD] = v;
		const switch_gpu_backing_token promise = {v, 0, SWITCH_GPU_ENGINE_PROMISE, kOffset};
		tgm_chan_park_promises(&reader, &promise, 1);
		reader.submitted += 1;
	};
	for (int i = 0; i < kRounds; ++i, ++value) {
		park(value);
		fake::pool[kOffset / 4] = value;
		reader.fence = reader.submitted;
		until(fake::now + 1000 * kMs);
	}
	EXPECT_EQ(readerSetsTaken(), 0u) << "the word moved";
	for (int i = 0; i < kRounds; ++i, ++value) {
		park(value);
		until(fake::now + 1000 * kMs);
		reader.fence = reader.submitted;
	}
	EXPECT_EQ(lines.size(), size_t(kRounds));
	EXPECT_EQ(readerSetsTaken(), 0u) << "the give-up";
	for (int i = 0; i < kRounds; ++i, ++value) {
		park(value);
		reader.error = 1;
		until(fake::now + 1000 * kMs);
		reader.error = 0;
		reader.fence = reader.submitted;
	}
	EXPECT_EQ(readerSetsTaken(), 0u) << "the readers gone";
	for (int i = 0; i < kRounds; ++i, ++value) {
		park(value);
		fake::pool[kOffset / 4] = value;
		reader.fence = reader.submitted;
		EXPECT_EQ(readerSetsTaken(), 1u) << "the slot made again";
	}
	until(fake::now + 1000 * kMs);
	EXPECT_EQ(readerSetsTaken(), 0u);
	for (int i = 0; i < kRounds; ++i, ++value) {
		park(value);
		ASSERT_EQ(tgm_promise_park(kOffset, value, "SpringBoard", "a CPU lock", nullptr, 0),
			  1);
		reader.fence = reader.submitted;
		tgm_promise_leave(kOffset, value, 0);
	}
	EXPECT_EQ(readerSetsTaken(), 0u) << "the last waiter leaving";
	EXPECT_EQ(lines.size(), size_t(kRounds)) << "only the give-ups were told";
}

// A promise kept before its reader parks is never waited on.
TEST_P(Call, AWordAlreadyKeptArmsNothing)
{
	fake::pool[kOffset / 4] = kValue;
	readerSubmits();
	EXPECT_EQ(fake::call.armed, 0u);
	for (const uint64_t used : giveups.used)
		EXPECT_EQ(used, 0u);
}

// The reader parks after keptPromises ran (its token was taken before): the kept job is recalled, so a job queued
// behind another is still judged from its turn.
TEST_P(Call, AReaderParkedAfterTheKeepFindsTheJob)
{
	makerKeeps(1);
	readerSubmits();
	EXPECT_EQ(maker.asked, std::vector<uint32_t>{maker.submitted});
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty()) << "deferred behind the job ahead";
}

// The call takes tgm_live_lock, a sleeping lock: it may never run where interrupts are off (a filter, under a simple
// lock). It runs in thread context, the raise its only window with interrupts off, and its line logged outside it.
TEST_P(Call, TheCallRunsInThreadContextAndOnlyTheRaiseHasInterruptsOff)
{
	readerSubmits();
	fake::now += 1000 * kMs;
	unsigned long flags = 0;
	local_irq_save(flags);
	EXPECT_THROW(giveup_run(nullptr), std::runtime_error);
	local_irq_restore(flags);
	tgm_live_locked = 0;
	interrupts_off::windows.clear();
	interrupts_off::watched = &fake::pool[kOffset / 4];
	EXPECT_NO_THROW(giveup_run(nullptr));
	interrupts_off::watched = nullptr;
	EXPECT_EQ(lines.size(), 1u);
	const std::vector<std::pair<uint32_t, uint32_t>> raise = {{kValue - 1, kValue}};
	std::vector<std::pair<uint32_t, uint32_t>> moved;
	for (const auto &window : interrupts_off::windows)
		if (window.first != window.second)
			moved.push_back(window);
	EXPECT_EQ(moved, raise)
		<< "the word moves inside one window with interrupts off, the raise's";
	EXPECT_GE(interrupts_off::windows.size(), 3u)
		<< "the raise, and the call's arming under its simple lock";
}

INSTANTIATE_TEST_SUITE_P(Raw, Call, testing::Values(kKinds[0]),
			 [](const testing::TestParamInfo<Kind> &info) {
				 return std::string(info.param.name);
			 });
}  // namespace
