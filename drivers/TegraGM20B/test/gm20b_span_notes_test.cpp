// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/SwitchGpuSpanNotes.h on the host: the render server's long spans and the compositor's longest job, as
// TegraGM20BMethodPaths' spans op answers them.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "../SwitchGpuSpanNotes.h"

using namespace switch_gpu_spans;

static std::string take(Notes &n, uint64_t jobNs, size_t size = 512)
{
	char out[512];
	switch_gpu_spans::take(n, jobNs, "sync_subs=0", out, size);
	return out;
}

TEST(GM20BSpanNotes, TheLineGivesTheJobAndTheNotesOldestFirstThenStartsAgain)
{
	Notes n = {};
	add(n, "backboardd", "31000 us PBGRAXmw_A3Xghfc k64ed02fd5d7a1865 50401 instr x1 921k");
	add(n, "backboardd", "6000 us pass prologue k0000000000000000 0 instr x1 3686km");
	EXPECT_EQ(take(n, 680'400'000),
		  "job_max_us=680400 sync_subs=0 spans=2; backboardd: 31000 us PBGRAXmw_A3Xghfc "
		  "k64ed02fd5d7a1865 50401 instr x1 921k; "
		  "backboardd: 6000 us pass prologue k0000000000000000 0 instr x1 3686km");
	EXPECT_EQ(take(n, 0), "job_max_us=0 sync_subs=0 spans=0");
}

TEST(GM20BSpanNotes, TheNewestSixteenAreKept)
{
	Notes n = {};
	for (int i = 0; i < 20; i++) {
		char text[16];
		snprintf(text, sizeof text, "s%d", i);
		add(n, "backboardd", text);
	}
	const std::string line = take(n, 1000, 512);
	EXPECT_EQ(line.find("job_max_us=1 sync_subs=0 spans=16; backboardd: s4; "), 0u);
	EXPECT_NE(line.find("backboardd: s19"), std::string::npos);
	EXPECT_EQ(line.find("backboardd: s3;"), std::string::npos);
}

TEST(GM20BSpanNotes, OnlyWholeNotesFit)
{
	Notes n = {};
	add(n, "backboardd", "first");
	add(n, "backboardd", "second");
	EXPECT_EQ(take(n, 0, 56), "job_max_us=0 sync_subs=0 spans=2; backboardd: first");
}

TEST(GM20BSpanNotes, ALongOwnerOrTextIsCut)
{
	Notes n = {};
	add(n, "anownernamelongerthansixteen", std::string(200, 'x').c_str());
	const std::string line = take(n, 0);
	EXPECT_NE(line.find("; anownernamelonge: "), std::string::npos);
	EXPECT_EQ(line.size(),
		  std::string("job_max_us=0 sync_subs=0 spans=1; anownernamelonge: ").size() + 111);
}
