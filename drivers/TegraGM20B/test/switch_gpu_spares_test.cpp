// SwitchGpuSpares.h: why a cached BoAlloc missed its zeroed spare, and how deep its class is kept warm afterwards.
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/SwitchGpuSpares.h"

namespace
{

TEST(Spares, EachMissHasItsReason)
{
	EXPECT_EQ(switch_gpu_spare_miss_of(0, 0, false), SWITCH_GPU_SPARE_FIRST);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 1, false), SWITCH_GPU_SPARE_ZEROING);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 0, false), SWITCH_GPU_SPARE_DRAINED);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 1, true), SWITCH_GPU_SPARE_PRESSURE);
}

TEST(Spares, AWarmClassDoublesItsDepthAtEachMissUpToTheMaximum)
{
	uint16_t depth = 0;
	const uint16_t expected[] = {1, 2, 4, 8, 8};
	for (const uint16_t want : expected) {
		depth = switch_gpu_spare_depth_after_miss(
			depth, true, switch_gpu_spare_miss_of(depth, 0, false));
		EXPECT_EQ(depth, want);
	}
}

TEST(Spares, PressureIneligibleAndUnlearnableClassesKeepTheirDepth)
{
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(2, true, SWITCH_GPU_SPARE_PRESSURE), 2);
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(0, true, SWITCH_GPU_SPARE_INELIGIBLE), 0);
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(0, false, SWITCH_GPU_SPARE_FIRST), 0);
}

// A burst of eight BoAllocs of one class, its spares refilled to depth between bursts: the second burst is served whole.
// At the old fixed depth of 1 it zeroed seven of them on the allocating thread again.
TEST(Spares, ARepeatedBurstIsServedFromSpares)
{
	uint16_t depth = 0, ready = 0;
	unsigned missed[2] = {};
	for (unsigned burst = 0; burst < 2; ++burst) {
		for (int i = 0; i < 8; ++i) {
			if (ready) {
				--ready;
				continue;
			}
			++missed[burst];
			depth = switch_gpu_spare_depth_after_miss(
				depth, true, switch_gpu_spare_miss_of(depth, 0, false));
		}
		ready = depth;
	}
	EXPECT_EQ(missed[0], 8u);
	EXPECT_EQ(missed[1], 0u);
}

// Boot 21 14:21:09: "System will begin reaping long-idle processes. memorystatus_available_pages: 48999", with the
// pressure level normal and 95888 KiB of zeroed spares kept wired; the reaper then killed about 200 idle processes
TEST(Spares, GoBackWithinACapOfTheReapersThreshold)
{
	constexpr uint64_t kReaper = 49000, kCap = (96ULL << 20) / 16384;
	EXPECT_TRUE(switch_gpu_spares_give_back(0, 48999, kReaper, kCap));
	EXPECT_TRUE(switch_gpu_spares_give_back(0, kReaper + kCap - 1, kReaper, kCap));
	EXPECT_FALSE(switch_gpu_spares_give_back(0, kReaper + kCap, kReaper, kCap));
}

TEST(Spares, GoBackAtAnyRaisedPressureLevel)
{
	EXPECT_TRUE(switch_gpu_spares_give_back(1, 1u << 30, 0, 0));
	EXPECT_TRUE(switch_gpu_spares_give_back(4, 1u << 30, 49000, 6144));
}

TEST(Spares, WithoutAReaperOnlyThePressureLevelCounts)
{
	EXPECT_FALSE(switch_gpu_spares_give_back(0, 0, 0, 6144));
}

// TegraGM20BUserClientSpares.cpp: spares go back below one cap above the reaper's threshold and are kept or made again
// only above two, so the pages a give-back frees cannot by themselves start the refill
TEST(Spares, BetweenOneAndTwoCapsNothingGoesBackAndNothingIsMade)
{
	constexpr uint64_t kReaper = 49000, kCap = (96ULL << 20) / 16384;
	const uint64_t between = kReaper + kCap + kCap / 2;
	EXPECT_FALSE(switch_gpu_spares_give_back(0, between, kReaper, kCap));
	EXPECT_TRUE(switch_gpu_spares_give_back(0, between, kReaper, 2 * kCap));
	EXPECT_FALSE(switch_gpu_spares_give_back(0, kReaper + 2 * kCap, kReaper, 2 * kCap));
}

// Boot 21: 94 of 96 MiB of spares in classes no one asked for and no depth above 16 MiB. A class up to 32 MiB learns a
// depth, each class's spares hold a quarter of the cap at most, and a class below its depth takes its room from the
// largest class kept past its own.
TEST(Spares, AClassOfADedicatedChunkLearnsADepthUnderAQuarterOfTheCap)
{
	constexpr uint64_t kCap = 96ULL << 20;
	EXPECT_EQ(switch_gpu_spare_depth_cap(24ULL << 20, kCap), 1);
	EXPECT_EQ(switch_gpu_spare_depth_cap(8ULL << 20, kCap), 3);
	EXPECT_EQ(switch_gpu_spare_depth_cap(3600ULL << 10, kCap), 6);
	EXPECT_EQ(switch_gpu_spare_depth_cap(64ULL << 10, kCap), SWITCH_GPU_SPARE_DEPTH_MAX);
}

TEST(Spares, RoomComesFromTheLargestClassKeptPastItsDepth)
{
	const uint16_t count[4] = {8, 3, 0, 2}, target[4] = {8, 1, 2, 0};
	EXPECT_EQ(switch_gpu_spare_over_depth(count, target, 4), 3u);
	const uint16_t settled[4] = {8, 1, 0, 0};
	EXPECT_EQ(switch_gpu_spare_over_depth(settled, target, 4), 4u);
	const uint16_t smaller[4] = {8, 3, 0, 0};
	EXPECT_EQ(switch_gpu_spare_over_depth(smaller, target, 4), 1u);
}

// A class that would not fit even with every spare past its depth given back gives none back: the spares that serve
// other classes' takes stay
// Boot 21 mapped every spare-backed BO (2,535,220 KiB of spare and cached BoAllocs) in 4 KiB PTEs: a spare was one
// buffer of scattered pages. A refilled spare of 512 KiB or more is whole 128 KiB units, each a big page.
TEST(Spares, ARefilledSpareIsWhole128KiBUnits)
{
	const switch_gpu_spare_units chunk = switch_gpu_spare_layout(16ULL << 20);
	EXPECT_EQ(chunk.units, 128u);
	EXPECT_EQ(chunk.tail, 0u);
	const switch_gpu_spare_units dedicated = switch_gpu_spare_layout(9856ULL << 10);
	EXPECT_EQ(dedicated.units, 77u);
	EXPECT_EQ(dedicated.tail, 0u);
	const switch_gpu_spare_units odd = switch_gpu_spare_layout(640ULL << 10);
	EXPECT_EQ(odd.units, 5u);
	EXPECT_EQ(odd.tail, 0u);
	const switch_gpu_spare_units tail = switch_gpu_spare_layout((1ULL << 20) + (64ULL << 10));
	EXPECT_EQ(tail.units, 8u);
	EXPECT_EQ(tail.tail, 64ULL << 10);
	const switch_gpu_spare_units small = switch_gpu_spare_layout(320ULL << 10);
	EXPECT_EQ(small.units, 0u);
	EXPECT_EQ(small.tail, 320ULL << 10);
}

TEST(Spares, NoSpareIsGivenBackForAClassThatStillWouldNotFit)
{
	constexpr uint64_t kCap = 96ULL << 20;
	EXPECT_TRUE(switch_gpu_spare_fits_after(94ULL << 20, 24ULL << 20, 16ULL << 20, kCap));
	EXPECT_FALSE(switch_gpu_spare_fits_after(94ULL << 20, 8ULL << 20, 16ULL << 20, kCap));
	EXPECT_TRUE(switch_gpu_spare_fits_after(64ULL << 20, 0, 16ULL << 20, kCap));
}

}  // namespace
