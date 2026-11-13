// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/SwitchGpuCompositorSync.h on the host: the compositor's sync pushes in its profiled submissions, split
// by whether the kick found an app on GR, as the window line and the spans op give them.
#include <gtest/gtest.h>

#include <string>

#include "../SwitchGpuCompositorSync.h"

static std::string text(const switch_gpu_comp_sync &s)
{
	char out[160];
	switch_gpu_comp_sync_format(out, sizeof out, &s);
	return out;
}

TEST(GM20BCompositorSync, KicksBehindAnAppAreCountedApart)
{
	switch_gpu_comp_sync s = {};
	switch_gpu_comp_sync_add(&s, 1, 6000, 0);
	switch_gpu_comp_sync_add(&s, 1, 2620000, 1);
	switch_gpu_comp_sync_add(&s, 2, 5000, 0);
	EXPECT_EQ(text(s),
		  "sync_subs=3 syncs=4 sync_us=2631 behind=1 behind_sync_us=2620 sync_max_us=2620");
}

TEST(GM20BCompositorSync, TakingStartsAgain)
{
	switch_gpu_comp_sync s = {};
	switch_gpu_comp_sync_add(&s, 1, 3951000, 1);
	switch_gpu_comp_sync taken = {};
	switch_gpu_comp_sync_take(&s, &taken);
	EXPECT_EQ(taken.max_ns, 3951000u);
	EXPECT_EQ(text(s), "sync_subs=0 syncs=0 sync_us=0 behind=0 behind_sync_us=0 sync_max_us=0");
}
