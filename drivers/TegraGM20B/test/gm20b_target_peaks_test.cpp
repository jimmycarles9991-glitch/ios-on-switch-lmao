// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/SwitchGpuTargetPeaks.h on the host: each process's largest target image since boot, and the line
// TegraGM20BMethodPaths answers.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "../SwitchGpuTargetPeaks.h"

using namespace switch_gpu_peaks;

static std::string text(const Peak (&table)[kOwners], size_t size = 512)
{
	char out[512];
	line(table, out, size);
	return out;
}

TEST(GM20BTargetPeaks, AProcessKeepsItsLargestImage)
{
	Peak t[kOwners] = {};
	note(t, "MercuryPosterExt", 720, 1280, 4);
	note(t, "MercuryPosterExt", 64, 64, 1);
	note(t, "backboardd", 720, 1280, 1);
	note(t, "backboardd", 720, 1630, 1);
	EXPECT_EQ(text(t), "backboardd=720x1630x1;MercuryPosterExt=720x1280x4;");
}

TEST(GM20BTargetPeaks, ANewcomerTakesTheSmallestSlotOnlyWhenLarger)
{
	Peak t[kOwners] = {};
	for (uint32_t i = 0; i < kOwners; i++)
		note(t, ("p" + std::to_string(i)).c_str(), 100 + i, 100, 1);
	note(t, "tiny", 10, 10, 1);
	EXPECT_EQ(text(t).find("tiny"), std::string::npos);
	note(t, "MercuryPosterExt", 720, 1280, 4);
	const std::string s = text(t);
	EXPECT_EQ(s.rfind("MercuryPosterExt=720x1280x4;", 0), 0u);
	EXPECT_EQ(s.find("p0="), std::string::npos);
	EXPECT_NE(s.find("p1="), std::string::npos);
}

TEST(GM20BTargetPeaks, TheLineDropsWholeEntriesFromTheSmallest)
{
	Peak t[kOwners] = {};
	note(t, "big", 2000, 2000, 1);
	note(t, "small", 10, 10, 1);
	EXPECT_EQ(text(t, 20), "big=2000x2000x1;");
	EXPECT_EQ(text(t, 4), "");
	note(t, "", 4000, 4000, 1);
	note(t, "none", 0, 4000, 1);
	EXPECT_EQ(text(t), "big=2000x2000x1;small=10x10x1;");
}
