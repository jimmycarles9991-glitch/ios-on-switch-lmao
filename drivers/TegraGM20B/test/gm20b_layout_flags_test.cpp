// SPDX-License-Identifier: MIT
// TegraGM20BLayoutAsk.h: what an import's layout ask passes the surface's decision. KERN-28: a surface only sampled at its
// first ask tiles when nothing has written it yet; never one over its creator's memory, a planar one, one another
// import decides meanwhile, or one asked at another shape.
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BLayoutAsk.h"

namespace
{

TEST(LayoutAsk, AWriterOfATileableSurfaceAsksToWrite)
{
	EXPECT_EQ(tgm_layout_ask_flags(1, 1, 0, 1, 1, 0, 0), TGM_LAYOUT_ASK_WRITES);
	EXPECT_EQ(tgm_layout_ask_flags(1, 1, 0, 1, 1, 1, 1),
		  TGM_LAYOUT_ASK_WRITES | TGM_LAYOUT_ASK_FIRST);
}

TEST(LayoutAsk, ASamplerOfAnUnwrittenTileableSurfaceAsksToTile)
{
	EXPECT_EQ(tgm_layout_ask_flags(1, 1, 0, 1, 0, 1, 0), TGM_LAYOUT_ASK_UNWRITTEN);
	EXPECT_EQ(tgm_layout_ask_flags(1, 1, 0, 1, 0, 0, 0), 0);
}

TEST(LayoutAsk, ClientMemoryPlanarSharedOrOtherShapesNeverTile)
{
	for (int asks = 0; asks < 2; ++asks) {
		EXPECT_EQ(tgm_layout_ask_flags(1, 1, 1, 1, asks, 1, 0), 0) << asks;
		EXPECT_EQ(tgm_layout_ask_flags(1, 0, 0, 1, asks, 1, 0), 0) << asks;
		EXPECT_EQ(tgm_layout_ask_flags(0, 1, 0, 1, asks, 1, 0), 0) << asks;
		EXPECT_EQ(tgm_layout_ask_flags(1, 1, 0, 0, asks, 1, 0), 0) << asks;
	}
}

}  // namespace
