// SPDX-License-Identifier: MIT
// An IOSurface's layout decision (TegraGM20BTiled.cpp decide) and what each one is called on the debug decision line.
// Block-linear unless a named fact rules it out. A CPU mapping in another process is none: the X1's memory controller
// takes linear addresses only (TRM 18.7, "(x,y) to linear address translation is the responsibility of clients"), so
// every CPU access, in any process, reads the surface through IOSurfaceLock's row view (TegraGM20BViews.cpp).
#pragma once
#include <stddef.h>

namespace tgm_layout_decision
{
enum Decision {
	DecidedTiled,
	PitchSampledFirst,
	PitchShared,
	PitchShape,
	PitchMoveFailed,
	Decisions
};

constexpr const char *kWhy[] = {
	"block-linear",
	"pitch: its first GPU import only samples it",
	"pitch: imported before it asked",
	"pitch: a shape that does not tile",
	"pitch: the tiling move failed (decided again at a later import's ask)",
};
static_assert(sizeof(kWhy) / sizeof(kWhy[0]) == Decisions, "a name per decision");

constexpr const char *why(Decision d)
{
	return d < Decisions ? kWhy[d] : "?";
}
}  // namespace tgm_layout_decision
