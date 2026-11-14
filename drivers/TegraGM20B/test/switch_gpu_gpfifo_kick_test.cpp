// SPDX-License-Identifier: MIT
// chan506f.c's GPFIFO entries and GP_PUT write (the kick every job's fence, wake entry and kernel channel ends with):
// the entries are plain stores into the write-combined ring, and one store barrier publishes them, the pushes and the
// client's own writes before the doorbell.
#include <array>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/test/kick_include/nvif/chan.h"

unsigned switch_gpu_test_delays, switch_gpu_test_warnings, switch_gpu_test_barriers,
	switch_gpu_test_doorbells;
u32 switch_gpu_test_doorbell;

namespace
{
struct Ring {
	std::array<u32, 64> userd = {};
	std::array<u32, 2 * 64> fifo = {};
	std::array<u32, 1024> push = {};
	nvif_chan chan = {};
};
}  // namespace

TEST(GpfifoKick, EntriesAreStoresAndOneBarrierPublishesTheKick)
{
	Ring r;
	ASSERT_EQ(nvif_chan506f_ctor(&r.chan, r.userd.data(), r.fifo.data(), sizeof(r.fifo),
				     r.push.data(), 0x100000, sizeof(r.push)),
		  0);
	switch_gpu_test_barriers = switch_gpu_test_doorbells = 0;
	nvif_chan506f_gpfifo_push(&r.chan, false, 0x200000, 128, false);
	nvif_chan506f_gpfifo_push(&r.chan, false, 0x300000, 64, true);
	nvif_chan506f_gpfifo_push(&r.chan, true, 0x400000, 32, false);
	EXPECT_EQ(switch_gpu_test_barriers, 0u) << "a barrier per GPFIFO entry";
	EXPECT_EQ(switch_gpu_test_doorbells, 0u);
	EXPECT_EQ(r.fifo[0], 0x200000u);
	EXPECT_EQ(r.fifo[3] & 0x80000000u, 0x80000000u) << "the second entry's no-prefetch bit";
	nvif_chan506f_gpfifo_kick(&r.chan);
	EXPECT_EQ(switch_gpu_test_doorbells, 1u);
	EXPECT_EQ(switch_gpu_test_doorbell, 3u) << "GP_PUT names the entry after the last";
	EXPECT_EQ(switch_gpu_test_barriers, 1u) << "barriers before one GP_PUT write";
}
