// SwitchGpuSpareBooks.h, driven the way TegraGM20BUserClientSpares.cpp drives it: the refill, the recycle worker, the
// copy engine's completions and BoAlloc's take, over a model of the pool's lists. The 25 Nov device run's patterns are
// replayed through it, and through the decisions the pool made before (the cap check without dirty spares, no room for a
// class below its depth, no reserve, a refill that could make no spare of 512 KiB or more), to count the misses each
// leaves on the allocating thread.
#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "drivers/TegraGM20B/SwitchGpuSpareRecord.h"

namespace
{
constexpr uint64_t kCap = SWITCH_GPU_SPARE_CAP_BYTES, kLearnMax = 32ULL << 20;
constexpr uint64_t kMiB = 1ULL << 20, kKiB = 1ULL << 10;

uint32_t classOf(uint64_t bytes)
{
	return switch_gpu_spare_class_up(bytes);
}

uint64_t bytesOf(uint32_t c)
{
	return switch_gpu_spare_class_bytes(c);
}

uint32_t slotOf(uint64_t bytes, switch_gpu_spare_kind kind)
{
	return switch_gpu_spare_slot(kind, classOf(bytes));
}

struct Pool {
	switch_gpu_spare_books b{};
	std::vector<uint32_t> dirty;  // spare slots, as the kernel's dirty list
	bool copyEngineBusy = false;  // switch_gpu_cached_zero answers -16
	bool unitsRefused = false;    // the refill before the fix: no spare of 512 KiB or more
	bool kicked = false;          // a worker's thread call entered and not run yet
	bool kickAskedOnly =
		false;  // the take before the fix: the refill kicked only for the asked class
	unsigned made = 0, refused = 0, misses = 0;
	uint64_t peak = 0;
	// per slot: BoAllocs asked, served by a spare, spares the refill made
	unsigned asked[SWITCH_GPU_SPARE_SLOTS] = {}, served[SWITCH_GPU_SPARE_SLOTS] = {},
		 refilled[SWITCH_GPU_SPARE_SLOTS] = {}, evicted[SWITCH_GPU_SPARE_SLOTS] = {};

	Pool()
	{
		switch_gpu_spare_books_init(b);
	}

	void track()
	{
		peak = b.held > peak ? b.held : peak;
	}

	void evict(uint32_t s, uint64_t need)
	{
		while (b.held + need > kCap) {
			const uint32_t d = switch_gpu_spare_books_evict(b, s, b.held + need - kCap);
			if (d == SWITCH_GPU_SPARE_NONE)
				break;
			--b.ready[d];
			b.held -= switch_gpu_spare_slot_bytes(d);
			++evicted[d];
		}
	}

	void refill()
	{
		unsigned steps = 0;
		for (uint32_t s;
		     (s = switch_gpu_spare_books_refill(b, kCap)) != SWITCH_GPU_SPARE_NONE;) {
			if (++steps > 4096) {
				ADD_FAILURE() << "the refill never stops";
				return;
			}
			const uint64_t bytes = switch_gpu_spare_slot_bytes(s);
			if (unitsRefused && bytes >= SWITCH_GPU_SPARE_UNITS_FROM) {
				++refused;
				return;
			}
			evict(s, bytes);
			b.held += bytes;
			++b.dirty[s];
			track();
			++made;
			++refilled[s];
			dirty.push_back(s);
			recycle();
		}
	}

	// the thread calls entered since the last settle run, as they would before the next BoAlloc comes
	void settle()
	{
		if (kicked)
			refill();
		kicked = false;
	}

	void recycle()
	{
		while (!dirty.empty()) {
			const uint32_t s = dirty.back();
			dirty.pop_back();
			--b.dirty[s];
			if (!switch_gpu_spare_books_keep(b, s, kCap)) {
				b.held -= switch_gpu_spare_slot_bytes(s);
				continue;
			}
			evict(s, 0);
			if (copyEngineBusy) {
				dirty.insert(dirty.begin(), s);
				++b.dirty[s];
				return;
			}
			++b.ready[s];
		}
	}

	// spareTake, then the refill only where it kicks it
	bool take(uint64_t bytes, bool compositor,
		  switch_gpu_spare_kind kind = SWITCH_GPU_SPARE_CACHED)
	{
		const uint32_t c = slotOf(bytes, kind);
		const uint32_t d = switch_gpu_spare_books_take(b, c, compositor);
		switch_gpu_spare_books_asked(b, c, d);
		++asked[c];
		if (d != SWITCH_GPU_SPARE_NONE) {
			++served[c];
			--b.ready[d];
			b.held -= switch_gpu_spare_slot_bytes(d);
		} else {
			++misses;
			switch_gpu_spare_books_learn(
				b, c, switch_gpu_spare_books_why(b, c, compositor, false),
				kLearnMax, kCap);
		}
		kicked |= kickAskedOnly ? switch_gpu_spare_books_short(b, c)
					: switch_gpu_spare_books_wants_refill(b, kCap);
		return d != SWITCH_GPU_SPARE_NONE;
	}

	// A freed BO of class `bytes` back to the spares (spareKeep), then the workers run
	void free(uint64_t bytes, switch_gpu_spare_kind kind = SWITCH_GPU_SPARE_CACHED)
	{
		const uint64_t backing = bytesOf(classOf(bytes));
		const uint32_t s =
			switch_gpu_spare_slot(kind, switch_gpu_spare_class_of_spare(backing));
		b.held += backing;
		++b.dirty[s];
		dirty.push_back(s);
		recycle();
		kicked |= switch_gpu_spare_books_wants_refill(b, kCap);
		EXPECT_LE(b.held, kCap) << "once the freed BO has been kept or given back";
	}

	uint64_t readyBytes(switch_gpu_spare_kind kind) const
	{
		uint64_t n = 0;
		for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
			n += uint64_t(b.ready[switch_gpu_spare_slot(kind, c)]) * bytesOf(c);
		return n;
	}

	uint64_t readyBytes() const
	{
		return readyBytes(SWITCH_GPU_SPARE_CACHED) + readyBytes(SWITCH_GPU_SPARE_WC);
	}
};

TEST(SpareBooks, EveryWarmClassIsReadyFromBootUnderTheCap)
{
	Pool p;
	p.refill();
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM)
		EXPECT_EQ(p.b.ready[classOf(w.bytes)], w.ready) << (w.bytes >> 10) << " KiB";
	EXPECT_EQ(p.b.held, switch_gpu_spare_warm_bytes());
	EXPECT_EQ(p.readyBytes(SWITCH_GPU_SPARE_WC), 0u) << "the warm table is all cached";
	EXPECT_LE(p.peak, kCap);
}

// Review KR1: the compositor's 24 MiB class kept two spares, one for it, only by learning its second, which the cap
// had no room for (72.5 + 24 > 96 MiB). From boot the class holds both, and a miss never lowers a warm class's depth.
TEST(SpareBooks, TheCompositorsClassHoldsItsReserveFromBoot)
{
	Pool p;
	p.refill();
	const uint32_t c = classOf(24 * kMiB);
	EXPECT_TRUE(p.b.reserved[c]);
	EXPECT_EQ(p.b.ready[c], 2);
	const uint32_t sixteen = classOf(16 * kMiB);
	p.b.ready[sixteen] = 0;
	p.take(16 * kMiB, false);
	EXPECT_GE(switch_gpu_spare_books_target(p.b, sixteen), 2)
		<< "a miss in a warm class kept two lowered it to the quarter-cap depth";
}

// 25 Nov: gpu.path.launch_target's 1792x1472 RGBA16F target (21,504 KiB), made twice by an app: the first takes one of
// the class's two warm spares, the compositor's stays, and the refill makes the next before the second.
TEST(SpareBooks, AnAppsLaunchTargetLeavesTheCompositorsSpare)
{
	Pool p;
	p.refill();
	const uint64_t target = 21504 * kKiB;
	EXPECT_TRUE(p.take(target, false));
	EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 1);
	EXPECT_FALSE(p.take(target, false)) << "the last one is the compositor's";
	EXPECT_EQ(switch_gpu_spare_books_why(p.b, classOf(target), false, false),
		  SWITCH_GPU_SPARE_RESERVED);
	EXPECT_TRUE(p.take(target, true));
	p.refill();
	EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 2);
}

// The same two rounds on the pool as it ran on 25 Nov: the refill made no spare of 512 KiB or more, so the warm 24 MiB
// class never held one, and the first round zeroed its target on the calling thread ("drained 1", 13,782 us).
TEST(SpareBooks, LaunchTargetReplayBeforeAndAfter)
{
	const auto rounds = [](bool unitsRefused) {
		Pool p;
		p.unitsRefused = unitsRefused;
		p.refill();
		const uint64_t target = 21504 * kKiB;
		for (int round = 0; round < 2; ++round) {
			p.take(target, false);
			p.free(target);
			p.settle();
		}
		return p.misses;
	};
	EXPECT_GE(rounds(true), 1u);
	EXPECT_EQ(rounds(false), 0u);
}

// Review KR2: with the copy engine too busy to zero, every spare the refill makes stays dirty; counting only ready and
// zeroing bytes against the cap, the refill wired past it. Every byte in every state is held now.
TEST(SpareBooks, ABusyCopyEngineCannotLetTheRefillPassTheCap)
{
	Pool p;
	p.copyEngineBusy = true;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
		if (bytesOf(c) <= kLearnMax && bytesOf(c) >= kMiB)
			p.b.depth[c] = switch_gpu_spare_depth_cap(bytesOf(c), kCap, p.b.floor[c]);
	p.refill();
	EXPECT_LE(p.peak, kCap);
	EXPECT_GT(p.made, 0u);
}

// 25 Nov 14:22-14:25: the spares held 94-98 MiB of whatever had been freed, no spare of 20 MiB or more among them,
// while backboardd freed and remade 17,280 KiB BOs; each freed one was dropped (no room under the cap), so each remake
// zeroed 17 MiB on its render thread (~16 ms). Its class learns a depth at the first miss, and a freed BO of a class
// below its depth takes its room from spares kept past theirs.
TEST(SpareBooks, AFullPoolKeepsTheCompositorsChurnedTargets)
{
	const auto churn = [](bool roomForShortClasses) {
		Pool p;
		p.unitsRefused = p.kickAskedOnly = !roomForShortClasses;
		p.refill();
		// two of the compositor's targets live, which the pool before the fix had no spare for anyway
		p.take(24 * kMiB, true);
		p.take(24 * kMiB, true);
		EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 0);
		while (p.b.held + 4 * kMiB <= kCap) {
			p.b.held += 4 * kMiB;
			++p.b.ready[classOf(4 * kMiB)];
		}
		const uint64_t bo = 17280 * kKiB;
		unsigned remade = 0;
		for (int i = 0; i < 10; ++i) {
			if (!p.take(bo, true))
				++remade;
			p.settle();
			if (roomForShortClasses) {
				p.free(bo);
			} else if (p.b.held + bytesOf(classOf(bo)) <= kCap) {
				p.b.held += bytesOf(classOf(bo));
				++p.b.ready[classOf(bo)];
			}
			p.settle();
		}
		EXPECT_LE(p.peak, kCap);
		return remade;
	};
	EXPECT_EQ(churn(false), 10u);
	EXPECT_EQ(churn(true), 1u);
}

// backboardd's 17,280 KiB BOs ask the 20 MiB class, which has no depth until a miss; the 24 MiB class's spares serve
// them. A take that kicked the refill only for the asked class left the class it took from short until a miss.
TEST(SpareBooks, ATakeFromALargerClassRefillsThatClass)
{
	const auto burst = [](bool kickAskedOnly) {
		Pool p;
		p.kickAskedOnly = kickAskedOnly;
		p.refill();
		for (int i = 0; i < 3; ++i) {
			p.take(17280 * kKiB, true);
			p.settle();
		}
		return p.misses;
	};
	EXPECT_EQ(burst(true), 1u);
	EXPECT_EQ(burst(false), 0u);
}

// Review of 7052373bb: learned depths have no total bound, so after warm-up nothing was "past its depth" and a taken
// 24 MiB spare was never made again. The warm table refills first, taking its room from what learned depths keep.
TEST(SpareBooks, LearnedDepthsCannotStarveTheWarmTable)
{
	Pool p;
	p.refill();
	for (const uint64_t bytes : {4 * kMiB, 8 * kMiB}) {
		const uint32_t c = classOf(bytes);
		p.b.depth[c] = switch_gpu_spare_depth_cap(bytes, kCap, p.b.floor[c]);
	}
	p.refill();
	p.take(24 * kMiB, true);
	p.take(24 * kMiB, true);
	p.kicked = true;
	p.settle();
	EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 2);
	EXPECT_LE(p.peak, kCap);
}

TEST(SpareBooks, AFreedBoOfAClassNobodyKeepsGoesBackWhenThePoolIsFull)
{
	Pool p;
	p.refill();
	while (p.b.held + 4 * kMiB <= kCap) {
		p.b.held += 4 * kMiB;
		++p.b.ready[classOf(4 * kMiB)];
	}
	const uint64_t before = p.readyBytes();
	p.free(12 * kMiB);
	EXPECT_EQ(p.b.ready[classOf(12 * kMiB)], 0);
	EXPECT_EQ(p.readyBytes(), before);
	EXPECT_LE(p.b.held, kCap);
}

TEST(SpareBooks, ASpareServesItsOwnClassAndTheFourAboveIt)
{
	Pool p;
	p.refill();
	EXPECT_EQ(switch_gpu_spare_books_take(p.b, classOf(17280 * kKiB), true),
		  classOf(24 * kMiB));
	EXPECT_EQ(switch_gpu_spare_books_take(p.b, classOf(300 * kKiB), false),
		  SWITCH_GPU_SPARE_NONE);
}

// A system BO's write-combined buffer and a cached BO's cacheable one never serve each other (BoAlloc's PBENTRY note),
// in either direction, though both kinds hold the same classes
TEST(SpareBooks, TheTwoKindsNeverServeEachOther)
{
	Pool p;
	p.refill();
	const uint32_t cached = slotOf(4 * kMiB, SWITCH_GPU_SPARE_CACHED);
	const uint32_t wc = slotOf(4 * kMiB, SWITCH_GPU_SPARE_WC);
	EXPECT_FALSE(p.take(4 * kMiB, true, SWITCH_GPU_SPARE_WC));
	EXPECT_EQ(p.b.ready[cached], 2);
	p.free(4 * kMiB, SWITCH_GPU_SPARE_WC);
	EXPECT_EQ(p.b.ready[wc], 1);
	EXPECT_TRUE(p.take(4 * kMiB, true));
	EXPECT_EQ(p.b.ready[wc], 1) << "a cached ask took the write-combined spare";
	EXPECT_TRUE(p.take(4 * kMiB, true, SWITCH_GPU_SPARE_WC));
	EXPECT_EQ(p.b.ready[wc], 0);
	// the largest classes' asks look no further than their own kind's slots
	EXPECT_EQ(switch_gpu_spare_books_take(p.b, slotOf(256 * kMiB, SWITCH_GPU_SPARE_WC), true),
		  SWITCH_GPU_SPARE_NONE);
	EXPECT_EQ(switch_gpu_spare_kind_end(slotOf(256 * kMiB, SWITCH_GPU_SPARE_WC)),
		  SWITCH_GPU_SPARE_SLOTS);
}

// A write-combined miss teaches its own slot a depth, which the refill fills under the shared cap; the cached class of
// the same size keeps its own
TEST(SpareBooks, AWriteCombinedMissTeachesItsOwnDepth)
{
	Pool p;
	p.refill();
	const uint32_t cached = slotOf(4 * kMiB, SWITCH_GPU_SPARE_CACHED);
	const uint32_t wc = slotOf(4 * kMiB, SWITCH_GPU_SPARE_WC);
	const uint16_t cachedDepth = p.b.depth[cached];
	EXPECT_EQ(switch_gpu_spare_books_why(p.b, wc, false, false), SWITCH_GPU_SPARE_FIRST);
	EXPECT_FALSE(p.take(4 * kMiB, false, SWITCH_GPU_SPARE_WC));
	EXPECT_EQ(p.b.depth[wc], 1);
	EXPECT_EQ(p.b.depth[cached], cachedDepth);
	p.settle();
	EXPECT_EQ(p.b.ready[wc], 1);
	EXPECT_TRUE(p.take(4 * kMiB, false, SWITCH_GPU_SPARE_WC));
}

// One budget: the demand of both kinds takes the room of warm classes nobody asked for, never past the cap
TEST(SpareBooks, BothKindsShareTheCap)
{
	Pool p;
	p.refill();
	EXPECT_FALSE(p.take(16 * kMiB, false, SWITCH_GPU_SPARE_WC));
	EXPECT_FALSE(p.take(20 * kMiB, false, SWITCH_GPU_SPARE_WC));
	p.settle();
	EXPECT_LE(p.peak, kCap);
	EXPECT_EQ(p.readyBytes(SWITCH_GPU_SPARE_WC), 36 * kMiB);
	EXPECT_EQ(p.readyBytes(SWITCH_GPU_SPARE_CACHED) + 36 * kMiB, p.b.held);
	EXPECT_EQ(p.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_CACHED)], 2)
		<< "the compositor's reserve is not ours to give";
}

// A class asked since takes the room of the ones asked before it, the least recently asked first: here a
// write-combined spare nobody asked for (a freed system BO's) before the 20 MiB one asked earlier
TEST(SpareBooks, AClassAskedLaterTakesTheRoomOfTheLeastRecentlyAsked)
{
	Pool p;
	p.refill();
	EXPECT_FALSE(p.take(20 * kMiB, false, SWITCH_GPU_SPARE_WC));
	p.settle();
	EXPECT_EQ(p.b.ready[slotOf(20 * kMiB, SWITCH_GPU_SPARE_WC)], 1);
	EXPECT_TRUE(p.take(16 * kMiB, true));
	EXPECT_TRUE(p.take(16 * kMiB, true));
	p.free(24 * kMiB, SWITCH_GPU_SPARE_WC);
	EXPECT_EQ(p.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 1) << "kept while it fits";
	p.settle();
	EXPECT_EQ(p.b.ready[slotOf(16 * kMiB, SWITCH_GPU_SPARE_CACHED)], 2);
	EXPECT_EQ(p.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 0);
	EXPECT_EQ(p.b.ready[slotOf(20 * kMiB, SWITCH_GPU_SPARE_WC)], 1);
	EXPECT_LE(p.peak, kCap);
}

// gpu.path.fresh_spare: a 24 MiB Shared buffer, a system BO (the GPU pool refused it as past the client's share), made,
// freed and made again. Beside the full warm table (96.5 MiB of the 120 MiB cap) a 24 MiB write-combined spare needs
// 120.5 MiB: its refill takes the room of one warm 4 MiB spare nobody asked for, the smallest that covers the 512 KiB,
// so the second ask hits.
TEST(SpareBooks, FreshSpareTakesTheRoomOfAWarmClassNobodyAsked)
{
	Pool p;
	p.refill();
	for (int round = 0; round < 2; ++round) {
		p.take(24 * kMiB, false, SWITCH_GPU_SPARE_WC);
		p.settle();
		p.free(24 * kMiB, SWITCH_GPU_SPARE_WC);
		p.settle();
	}
	EXPECT_LE(p.peak, kCap);
	EXPECT_EQ(p.misses, 1u);
	EXPECT_EQ(p.evicted[slotOf(4 * kMiB, SWITCH_GPU_SPARE_CACHED)], 1u);
	EXPECT_EQ(p.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_CACHED)], 2);
}

// A slot short of its target takes no room from a slot asked as recently or later, so two slots that both want the
// same room cannot take it from each other, and the refill stops
TEST(SpareBooks, AnOlderAskNeverTakesTheRoomOfANewerOne)
{
	switch_gpu_spare_books b{};
	switch_gpu_spare_books_init(b);
	const uint32_t older = slotOf(20 * kMiB, SWITCH_GPU_SPARE_WC);
	const uint32_t newer = slotOf(16 * kMiB, SWITCH_GPU_SPARE_WC);
	b.used[older] = 5;
	b.used[newer] = 9;
	b.depth[older] = 1;
	b.depth[newer] = b.ready[newer] = 7;
	b.held = 7 * 16 * kMiB;
	EXPECT_EQ(switch_gpu_spare_books_evictable(b, older, newer), 0);
	EXPECT_EQ(switch_gpu_spare_books_evict(b, older, 12 * kMiB), SWITCH_GPU_SPARE_NONE);
	EXPECT_FALSE(switch_gpu_spare_books_fit(b, older, 20 * kMiB, kCap));
	EXPECT_NE(switch_gpu_spare_books_refill(b, kCap), older);
	b.ready[newer] = 6;
	b.ready[older] = 1;
	b.held = 6 * 16 * kMiB + 20 * kMiB;
	EXPECT_EQ(switch_gpu_spare_books_evict(b, newer, 12 * kMiB), older)
		<< "the newer ask takes the older one's room";
}

// The device's measured demand, replayed through the pool: a HW Test pass (idle_redraw's 8 MiB heap restore,
// launch_target's two rounds, fresh_spare's held target and 24 MiB Shared buffer) and backboardd's churn of 17,280 KiB
// targets (25 Nov 14:22-14:25), boot after boot. Prints each slot's asks, hits and refills (NOTES: "hit rates and
// refills per class").
void replayDemand(Pool &p)
{
	const auto round = [&p](uint64_t bytes, bool compositor, switch_gpu_spare_kind kind) {
		p.take(bytes, compositor, kind);
		p.settle();
		p.free(bytes, kind);
		p.settle();
	};
	round(8 * kMiB, false, SWITCH_GPU_SPARE_CACHED);
	round(21504 * kKiB, false, SWITCH_GPU_SPARE_CACHED);
	round(21504 * kKiB, false, SWITCH_GPU_SPARE_CACHED);
	p.take(21504 * kKiB, false);
	p.settle();
	round(24 * kMiB, false, SWITCH_GPU_SPARE_WC);
	p.free(21504 * kKiB);
	p.settle();
	for (int i = 0; i < 10; ++i)
		round(17280 * kKiB, true, SWITCH_GPU_SPARE_CACHED);
}

void printDemand(const char *title, const Pool &p)
{
	std::printf("%s\n", title);
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; ++s)
		if (p.asked[s] || p.refilled[s])
			std::printf("  %s %llu KiB: %u of %u asks served, %u refills, %u evicted\n",
				    switch_gpu_spare_slot_kind(s) == SWITCH_GPU_SPARE_WC
					    ? "write-combined"
					    : "cached",
				    (unsigned long long)(switch_gpu_spare_slot_bytes(s) >> 10),
				    p.served[s], p.asked[s], p.refilled[s], p.evicted[s]);
}

TEST(SpareBooks, TheReplayedDemandPerClass)
{
	Pool p;
	p.refill();
	replayDemand(p);
	printDemand("replayed demand, one pass:", p);
	Pool twice;
	twice.refill();
	replayDemand(twice);
	replayDemand(twice);
	printDemand("replayed demand, two passes in one boot:", twice);
	EXPECT_EQ(twice.served[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 1u)
		<< "the second pass's write-combined ask finds the spare its first miss taught";
	EXPECT_LE(p.peak, kCap);
	EXPECT_EQ(p.served[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 0u)
		<< "a class's first ask of a boot misses without a record";
	EXPECT_EQ(p.served[slotOf(20 * kMiB, SWITCH_GPU_SPARE_CACHED)], 10u);
}

// The next boot, seeded from this one's record: fresh_spare's write-combined class is ready from boot, and the warm
// 24 MiB pair an app's launch target and the compositor share stays, since this boot asked it (a seed of the
// write-combined class alone left the pair one short and the launch target's first ask hit the compositor's reserve)
TEST(SpareBooks, ASeededBootServesFreshSpareAndKeepsTheLaunchTargetsPair)
{
	Pool first;
	first.refill();
	replayDemand(first);
	std::vector<uint8_t> record(SWITCH_GPU_SPARE_RECORD_BYTES);
	record.resize(switch_gpu_spare_record_encode(first.b, record.data()));
	Pool next;
	ASSERT_GT(
		switch_gpu_spare_record_seed(next.b, record.data(), uint32_t(record.size()), kCap),
		0u);
	next.refill();
	EXPECT_EQ(next.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 1);
	EXPECT_EQ(next.b.ready[slotOf(24 * kMiB, SWITCH_GPU_SPARE_CACHED)], 2);
	EXPECT_LE(next.peak, kCap);
	replayDemand(next);
	printDemand("replayed demand, the boot after (seeded):", next);
	EXPECT_EQ(next.served[slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC)], 1u);
	EXPECT_EQ(next.misses, 0u);
}

}  // namespace
