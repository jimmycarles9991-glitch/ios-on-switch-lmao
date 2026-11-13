// SPDX-License-Identifier: MIT
#include "switch_gpu_event_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuEventWord.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"

namespace
{
class EventWords : public Events
{
protected:
	volatile uint32_t memory = 0;
	switch_gpu_event_word *word = nullptr;

	void SetUp() override
	{
		Events::SetUp();
		word = switch_gpu_event_word_create(&memory, 0x2840, nullptr);
		ASSERT_NE(word, nullptr);
	}

	void TearDown() override
	{
		Events::TearDown();
		switch_gpu_event_word_release(&word);
	}

	uint32_t floor = 0;

	uint64_t arm(uint32_t channel = 1, int expected = 0)
	{
		uint64_t base = ~0ull;
		EXPECT_EQ(switch_gpu_event_word_arm(word, event, channel, &base, &floor), expected);
		return base;
	}

	switch_gpu_event_signal *released(uint64_t value, const switch_gpu_backing_token &t,
					  uint32_t payload, uint32_t channel = 1)
	{
		auto *s = prepare(value, t);
		EXPECT_EQ(switch_gpu_event_signal_release(s, word, channel, payload), 0);
		return s;
	}

	void gpuWrites(uint32_t payload)
	{
		memory = payload;
		switch_gpu_events_completed();
		pump();
	}
};
}  // namespace

TEST_F(EventWords, ReleasedSignalCompletesFromItsWordBeforeItsJobEnds)
{
	EXPECT_EQ(arm(), 0u);
	auto t = token();
	auto *s = released(5, t, 5);
	switch_gpu_event_signal_commit(&s);
	pump();
	EXPECT_EQ(native->value, 0u);
	EXPECT_FALSE(switch_gpu_event_word_idle(word));
	gpuWrites(5);
	EXPECT_EQ(native->value, 5u);
	EXPECT_TRUE(switch_gpu_event_word_idle(word));
	finish(t);
	EXPECT_EQ(memory, 5u);
}

TEST_F(EventWords, WaiterOnAReleasedValueAcquiresOnTheWord)
{
	arm();
	auto t = token();
	auto *s = released(5, t, 5);
	switch_gpu_event_signal_commit(&s);
	auto *use = resolve(3);
	switch_gpu_word_wait wait{};
	ASSERT_TRUE(switch_gpu_event_use_word(use, 3, &wait));
	EXPECT_EQ(wait.pool, 0x2840u);
	EXPECT_EQ(wait.payload, 3u);
	EXPECT_EQ(wait.queue, t.queue);
	switch_gpu_event_use_release(&use);
	auto plain = token();
	signal(9, plain);
	use = resolve(8);
	EXPECT_FALSE(switch_gpu_event_use_word(use, 8, &wait));
	switch_gpu_event_use_release(&use);
	gpuWrites(5);
	finish(t);
	finish(plain);
}

TEST_F(EventWords, ReleaseRefusesAnotherPayloadChannelOrNoGrowth)
{
	native->value = 100;
	EXPECT_EQ(arm(), 100u);
	auto t = token();
	auto *s = prepare(105, t);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 1, 4), -22);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 2, 5), -22);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 1, 5), 0);
	switch_gpu_event_signal_commit(&s);
	auto u = token();
	s = prepare(105, u);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 1, 5), -22);
	switch_gpu_event_signal_abort(&s);
	s = prepare(100 + (1ull << 31), u);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 1, 0x80000000u), -22);
	switch_gpu_event_signal_abort(&s);
	gpuWrites(5);
	EXPECT_EQ(native->value, 105u);
	finish(t);
}

TEST_F(EventWords, AbortGivesBackItsPayloads)
{
	arm();
	auto t = token();
	auto *a = released(3, t, 3);
	auto *b = released(4, t, 4);
	switch_gpu_event_signal_abort(&a);
	switch_gpu_event_signal_abort(&b);
	EXPECT_TRUE(switch_gpu_event_word_idle(word));
	auto *again = released(3, t, 3);
	switch_gpu_event_signal_commit(&again);
	gpuWrites(3);
	EXPECT_EQ(native->value, 3u);
	finish(t);
}

TEST_F(EventWords, JobEndWithoutTheReleaseMovesTheWordAndPoisonsBelowIt)
{
	arm();
	auto t = token();
	auto *s = released(6, t, 6);
	switch_gpu_event_signal_commit(&s);
	finish(t, -5);
	EXPECT_EQ(memory, 6u);
	EXPECT_EQ(native->value, 0u);
	switch_gpu_backing_token none{};
	EXPECT_EQ(switch_gpu_event_resolve(event, 6, &none), -5);
	auto u = token();
	s = prepare(6, u);
	EXPECT_EQ(switch_gpu_event_signal_release(s, word, 1, 6), -22);
	switch_gpu_event_signal_abort(&s);
	s = released(7, u, 7);
	switch_gpu_event_signal_commit(&s);
	pump();
	EXPECT_EQ(native->value, 0u);
	gpuWrites(7);
	EXPECT_EQ(native->value, 7u);
	finish(u);
}

TEST_F(EventWords, ProducersFailureFailsTheReleasedSignal)
{
	arm();
	auto producer = token(), consumer = token();
	signal(2, producer);
	auto *wait = resolve(2);
	auto *b = batch(wait, consumer);
	auto *s = released(4, consumer, 4);
	EXPECT_EQ(switch_gpu_event_signal_inherit(s, b), 0);
	switch_gpu_event_signal_commit(&s);
	commit(b);
	switch_gpu_event_use_release(&wait);
	gpuWrites(4);
	EXPECT_EQ(native->value, 0u);
	finish(producer, -5);
	EXPECT_EQ(native->value, 0u);
	finish(consumer);
	EXPECT_EQ(native->value, 0u);
	switch_gpu_event_waits_release(&b);
}

TEST_F(EventWords, ArmMovesBaseOnlyWhenIdle)
{
	native->value = 10;
	EXPECT_EQ(arm(), 10u);
	auto t = token();
	auto *s = released(12, t, 2);
	switch_gpu_event_signal_commit(&s);
	arm(2, -16);
	EXPECT_EQ(arm(1), 10u);
	EXPECT_EQ(floor, 2u);
	gpuWrites(2);
	native->value = 50;
	EXPECT_EQ(arm(2), 48u);
	EXPECT_EQ(floor, 2u);
	s = released(51, t, 3, 2);
	switch_gpu_event_signal_commit(&s);
	gpuWrites(3);
	EXPECT_EQ(native->value, 51u);
	finish(t);
	memory = TegraGM20BEventReleaseLimit - 1;
	arm(2, -28);
}
