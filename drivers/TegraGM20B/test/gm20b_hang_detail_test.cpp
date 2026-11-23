// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "../TegraGM20BHangDecode.h"

namespace
{
std::string format(const tgm_hang_detail &d)
{
	char buf[TGM_HANG_LINE];
	tgm_hang_detail_format(buf, sizeof(buf), &d);
	return buf;
}

TEST(GM20BHangDetail, ABlamedChannelBesideTheLateOne)
{
	tgm_hang_detail d = {};
	d.chid = 20;
	d.stalled_ms = 1270;
	d.late_chid = 23;
	d.late_done = 741;
	d.late_submitted = 763;
	d.late_ms = 120;
	EXPECT_EQ(format(d),
		  "ch 20 (no progress for 1270 ms); late ch 23: 741/763, moved 120 ms ago");
}

TEST(GM20BHangDetail, TheLateChannelBlamedItself)
{
	tgm_hang_detail d = {};
	d.chid = 4;
	d.stalled_ms = 2000;
	d.late_chid = 4;
	d.late_done = 152;
	d.late_submitted = 153;
	d.late_ms = 2000;
	EXPECT_EQ(format(d),
		  "ch 4 (no progress for 2000 ms); late ch 4: 152/153, moved 2000 ms ago");
}
}  // namespace
