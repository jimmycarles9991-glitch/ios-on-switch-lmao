// SPDX-License-Identifier: MIT
// SwitchGpuChannelEpoch.h: a move (a channel destroyed, a connection closed) puts the next value into every connection's
// fence page; a page handed out starts at the current value; pages given back to the pool move it once after the pool
// refused the kernel memory, not at all when it refused none, and not while a channel is being made (its rollback).
#include <gtest/gtest.h>
#include <cstdint>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuChannelEpoch.h"

uint32_t switch_gpu_channel_memory_refused, switch_gpu_channel_creates;

namespace
{
unsigned moves;
}  // namespace

extern "C" void switch_gpu_channel_epoch_move(void)
{
	__atomic_add_fetch(&moves, 1u, __ATOMIC_RELAXED);
}

namespace
{
constexpr size_t kStride = 16384;
constexpr uint32_t kPages = 3;

struct Pages {
	std::vector<uint32_t> words = std::vector<uint32_t>(kPages * kStride / 4);

	uint8_t *base()
	{
		return reinterpret_cast<uint8_t *>(words.data());
	}

	uint32_t epoch(uint32_t page) const
	{
		return words[page * kStride / 4 + TegraGM20BChannelEpochWord];
	}
};

TEST(SwitchGpuChannelEpoch, EachMoveReachesEveryConnectionsPage)
{
	Pages pages;
	uint32_t epoch = 0;
	switch_gpu_channel_epoch_mirror(&epoch, pages.base(), kPages, kStride);
	for (uint32_t p = 0; p < kPages; ++p)
		EXPECT_EQ(pages.epoch(p), 1u) << p;
	switch_gpu_channel_epoch_mirror(&epoch, pages.base(), kPages, kStride);
	for (uint32_t p = 0; p < kPages; ++p)
		EXPECT_EQ(pages.epoch(p), 2u) << p;
	EXPECT_EQ(pages.words[TegraGM20BChannelEpochWord - 1], 0u);
	EXPECT_EQ(pages.words[TegraGM20BChannelEpochWord + 1], 0u);
}

// A client compares by inequality: the move past the last value still differs from what it read
TEST(SwitchGpuChannelEpoch, AMovePastTheLastValueStillDiffers)
{
	Pages pages;
	uint32_t epoch = UINT32_MAX;
	switch_gpu_channel_epoch_mirror(&epoch, pages.base(), kPages, kStride);
	EXPECT_NE(pages.epoch(0), UINT32_MAX);
}

// Moves before the fence range exists are counted, and the first page handed out starts at the count
TEST(SwitchGpuChannelEpoch, APageHandedOutStartsAtTheCurrentValue)
{
	uint32_t epoch = 0;
	switch_gpu_channel_epoch_mirror(&epoch, nullptr, kPages, kStride);
	switch_gpu_channel_epoch_mirror(&epoch, nullptr, kPages, kStride);
	Pages pages;
	switch_gpu_channel_epoch_store(
		reinterpret_cast<volatile uint32_t *>(pages.base() + kStride), epoch);
	EXPECT_EQ(pages.epoch(1), 2u);
	EXPECT_EQ(pages.epoch(0), 0u);
}

TEST(SwitchGpuChannelEpoch, PagesGivenBackWithNoRefusalMoveNothing)
{
	moves = 0;
	switch_gpu_channel_memory_refused = 0;
	for (int i = 0; i < 4; ++i)
		switch_gpu_channel_pool_given_back();
	EXPECT_EQ(moves, 0u);
}

TEST(SwitchGpuChannelEpoch, PagesGivenBackAfterARefusalMoveItOnce)
{
	moves = 0;
	switch_gpu_channel_memory_refused = 0;
	switch_gpu_channel_pool_refused();
	switch_gpu_channel_pool_given_back();
	switch_gpu_channel_pool_given_back();
	EXPECT_EQ(moves, 1u);
	switch_gpu_channel_pool_refused();
	switch_gpu_channel_pool_given_back();
	EXPECT_EQ(moves, 2u);
}

// A channel being made gives back what it took when it fails: those frees move nothing, so a refused client's rollback
// wakes no other refused client (nor itself) to ask again; the first pages given back after it does
TEST(SwitchGpuChannelEpoch, PagesAChannelBeingMadeGivesBackMoveNothing)
{
	moves = 0;
	switch_gpu_channel_memory_refused = 0;
	switch_gpu_channel_create_begins();
	switch_gpu_channel_pool_refused();
	switch_gpu_channel_pool_given_back();
	switch_gpu_channel_create_ends();
	EXPECT_EQ(moves, 0u);
	switch_gpu_channel_create_begins();
	switch_gpu_channel_create_begins();
	switch_gpu_channel_pool_given_back();
	switch_gpu_channel_create_ends();
	switch_gpu_channel_pool_given_back();
	switch_gpu_channel_create_ends();
	EXPECT_EQ(moves, 0u) << "a free while another channel was still being made";
	switch_gpu_channel_pool_given_back();
	EXPECT_EQ(moves, 1u);
}

TEST(SwitchGpuChannelEpoch, FreesRacingForOneRefusalMoveItOnce)
{
	for (int round = 0; round < 200; ++round) {
		moves = 0;
		switch_gpu_channel_pool_refused();
		std::vector<std::thread> frees;
		for (int t = 0; t < 4; ++t)
			frees.emplace_back([] {
				for (int i = 0; i < 50; ++i)
					switch_gpu_channel_pool_given_back();
			});
		for (std::thread &f : frees)
			f.join();
		ASSERT_EQ(moves, 1u) << round;
	}
}
}  // namespace
