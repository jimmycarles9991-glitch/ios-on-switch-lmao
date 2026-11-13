// SPDX-License-Identifier: MIT
// TegraGM20BTlb.h over a GMMU model: PTEs in DRAM, the L2's copies of them, one TLB per address space shared by its
// channels, and work already running that walks and refills at any time. Maps are GPU-ordered (PTE writes, then the
// generation moves); unmaps are synchronous (PTE cleared, then L2 and TLB invalidated before the call returns), as
// gpu_exec.c does them. No access may ever reach a page the space no longer owns.
#include <cstdint>
#include <map>
#include <optional>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BTlb.h"

namespace
{
using Page = uint32_t;
using Va = uint64_t;

struct Gmmu {
	std::map<std::pair<int, Va>, Page> dram, l2;
	std::map<std::pair<int, Va>, Page> tlb;
	std::map<std::pair<int, Va>, bool> l2Invalid;

	void invalidateL2()
	{
		l2.clear();
		l2Invalid.clear();
	}

	void invalidateTlb()
	{
		tlb.clear();
	}

	// One access: the TLB, else a walk through the L2 (which keeps what it read, a missing PTE included).
	std::optional<Page> access(int space, Va va)
	{
		const auto key = std::make_pair(space, va);
		if (auto t = tlb.find(key); t != tlb.end())
			return t->second;
		std::optional<Page> pte;
		if (auto l = l2.find(key); l != l2.end())
			pte = l->second;
		else if (!l2Invalid.count(key)) {
			if (auto d = dram.find(key); d != dram.end())
				pte = d->second;
			if (pte)
				l2[key] = *pte;
			else
				l2Invalid[key] = true;
		}
		if (pte)
			tlb[key] = *pte;
		return pte;
	}
};

struct Space {
	int id;
	tgm_tlb_space tlb;
	Gmmu &gmmu;

	Space(int i, Gmmu &g) : id(i), gmmu(g)
	{
		tgm_tlb_space_init(&tlb);
	}

	void map(Va va, Page page)
	{
		gmmu.dram[{id, va}] = page;
		tgm_tlb_mapped(&tlb);
	}

	void unmap(Va va)
	{
		gmmu.dram.erase({id, va});
		gmmu.invalidateL2();
		gmmu.invalidateTlb();
	}
};

struct Channel {
	Space &space;
	tgm_tlb_chan tlb;
	int invalidations = 0;

	explicit Channel(Space &s) : space(s)
	{
		tgm_tlb_chan_init(&tlb);
	}

	// A new submission: the prologue, then its work's first access.
	std::optional<Page> submit(Va va)
	{
		if (tgm_tlb_take(&tlb, &space.tlb)) {
			space.gmmu.invalidateL2();
			space.gmmu.invalidateTlb();
			invalidations++;
		}
		return space.gmmu.access(space.id, va);
	}

	// Work submitted before, still running: no prologue.
	std::optional<Page> running(Va va)
	{
		return space.gmmu.access(space.id, va);
	}
};

constexpr Va kVa = 0x100000;
constexpr Page kOld = 7, kNew = 9;

TEST(GpuOrderedTlb, StaleRefillAfterAnotherChannelsInvalidateNeverReachesReleasedPages)
{
	Gmmu gmmu;
	Space vm(1, gmmu);
	Channel a(vm), b(vm);
	vm.map(kVa, kOld);
	ASSERT_EQ(a.submit(kVa), kOld);
	ASSERT_EQ(b.submit(kVa), kOld);

	vm.unmap(kVa);
	EXPECT_FALSE(b.submit(kVa).has_value());
	EXPECT_FALSE(a.running(kVa).has_value());
	// A completes; BoFree's zombie gives kOld back and another process owns it now.
	EXPECT_FALSE(b.submit(kVa).has_value());
	EXPECT_FALSE(b.running(kVa).has_value());
}

TEST(GpuOrderedTlb, RebindOverALiveRangeShowsOnlyThisSpacesPages)
{
	Gmmu gmmu;
	Space vm(1, gmmu);
	Channel a(vm), b(vm);
	vm.map(kVa, kOld);
	ASSERT_EQ(a.submit(kVa), kOld);
	vm.unmap(kVa);
	vm.map(kVa, kNew);
	const auto during = a.running(kVa);
	EXPECT_TRUE(!during || *during == kNew);
	EXPECT_EQ(b.submit(kVa), kNew);
	EXPECT_EQ(a.submit(kVa), kNew);
}

TEST(GpuOrderedTlb, AMapIsSeenByEverySubmissionAfterItEvenOverAStaleMiss)
{
	Gmmu gmmu;
	Space vm(1, gmmu);
	Channel a(vm), b(vm);
	ASSERT_FALSE(a.submit(kVa).has_value());
	vm.map(kVa, kNew);
	EXPECT_FALSE(a.running(kVa).has_value());
	EXPECT_EQ(b.submit(kVa), kNew);
	EXPECT_EQ(a.submit(kVa), kNew);
}

TEST(GpuOrderedTlb, AChannelThatNeverSubmitsHoldsNothingAndCatchesUpIfItDoes)
{
	Gmmu gmmu;
	Space vm(1, gmmu);
	Channel a(vm), idle(vm);
	for (Page p = 1; p <= 3; p++) {
		vm.map(kVa + p * 0x1000, p);
		EXPECT_EQ(a.submit(kVa + p * 0x1000), p);
	}
	vm.unmap(kVa + 0x1000);
	EXPECT_EQ(idle.submit(kVa + 0x3000), 3u);
	EXPECT_EQ(idle.invalidations, 1);
	EXPECT_FALSE(idle.submit(kVa + 0x1000).has_value());
}

TEST(GpuOrderedTlb, OnlyChannelsBehindTheSpaceInvalidate)
{
	Gmmu gmmu;
	Space vm(1, gmmu);
	Channel a(vm), b(vm);
	a.submit(kVa);
	b.submit(kVa);
	EXPECT_EQ(a.invalidations, 1);
	EXPECT_EQ(b.invalidations, 1);
	a.submit(kVa);
	EXPECT_EQ(a.invalidations, 1);
	vm.map(kVa, kNew);
	vm.map(kVa + 0x1000, kNew);
	a.submit(kVa);
	a.submit(kVa);
	b.submit(kVa);
	EXPECT_EQ(a.invalidations, 2);
	EXPECT_EQ(b.invalidations, 2);
}

TEST(GpuOrderedTlb, ANewSpacesFirstSubmissionInvalidatesOverAReusedDirectory)
{
	Gmmu gmmu;
	{
		Space dead(1, gmmu);
		Channel c(dead);
		dead.map(kVa, kOld);
		ASSERT_EQ(c.submit(kVa), kOld);
		gmmu.dram.erase({dead.id, kVa});
	}
	Space reused(1, gmmu);
	Channel c(reused);
	EXPECT_FALSE(c.submit(kVa).has_value());
	EXPECT_EQ(c.invalidations, 1);
}
}  // namespace
