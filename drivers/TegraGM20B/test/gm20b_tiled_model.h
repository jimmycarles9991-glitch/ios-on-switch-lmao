// SPDX-License-Identifier: MIT
// The kernel calls TegraGM20BTiled.cpp and TegraGM20BViews.cpp make, modelled for gm20b_coherency_test and
// gm20b_views_test: physical pages by address, the copy engine's moves on them (gm20b_relayout_test proves the pushes
// give these bytes), the scratch pool, and each task's mapping of a surface with what it maps now.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

// The GPU's fences as the registry asks after them: each channel's last completed seqno; a channel not listed is gone.
// A wait is the fence interrupt arriving: the channel completes what the wait was for.
struct FakeGpu {
	std::map<uint32_t, uint32_t> completed;
	std::set<uint32_t> killed;
	std::vector<std::pair<uint32_t, uint32_t>> waits;
	bool allDone = true;
	int waitAll = 0;
	// GPU writes of the surface that completed but are still in the L2 (an Internal job's), the waits that put them in
	// DRAM, and the CPU moves that ran before one did
	bool l2Dirty = false;
	int visibleWaits = 0, staleCpuMoves = 0;
};

extern FakeGpu gGpu;

extern std::map<uint64_t, std::vector<uint8_t>> gPages;
std::vector<uint8_t> gather(const tgm_relayout_pages &p);
void scatter(const tgm_relayout_pages &p, const std::vector<uint8_t> &bytes);

struct Move {
	int op;
	std::vector<tgm_relayout_writer> writers;
	bool rowsClean;  // no page of the scratch held a CPU line the CPU may still write back
};

// Pages with lines in the CPU's cache that a clean and invalidate has not taken out since (a scratch is zero-filled
// through the cache when it is made)
extern std::set<uint64_t> gCpuLines;

extern std::vector<Move> gMoves;
extern bool gMoveFails;
// the CPU's moves (tgm_relayout_by_cpu, after a failed one), and whether the next scratch cannot be had
extern int gCpuMoves;
extern bool gScratchFails;
extern int gScratchesReclaimed;  // kept scratches a lock took back
extern bool gScratchPurged;
extern uint64_t
	gBackingContent;  // the pinned backing's recorded writes       // a kept scratch's pages come back empty
// switch_refusal's (domain, reason) per call
extern std::vector<std::pair<std::string, std::string>> gRefusals;

struct tgm_scratch {
	std::vector<uint64_t> pas;
	uint64_t handout = 0;
};

struct ViewModel {
	// a task's mapping of surface id through user client `client`
	std::map<std::pair<void *, uint32_t>, tgm_cpu_view> mappings;
	// what each (task, address) maps now: a scratch, or ViewModel::surface(id)
	std::map<std::pair<void *, uint64_t>, void *> mapped;
	std::set<void *> dead;
	std::map<void *, int> holds;
	int remaps = 0, scratchesPut = 0, scratchesDiscarded = 0;
	std::set<void *>
		badMappings;  // clients whose mapping is of part of the descriptor (tgm_cpu_view_of: -1)
	int remapFails = 0;             // the next remaps that fail
	std::set<void *> restoreFails;  // clients whose mapping cannot be given the surface back

	static void *surface(uint32_t id)
	{
		return reinterpret_cast<void *>(uintptr_t(id) << 4 | 1);
	}
};

extern ViewModel gViews;
