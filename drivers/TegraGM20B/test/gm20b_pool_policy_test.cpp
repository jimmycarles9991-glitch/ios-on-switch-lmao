// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/TegraGM20BPoolPolicy.h on the host: a model of the GPU pool when Maps opened (256 MiB, 221 used,
// 63 of them the compositor's: 35 free) and what Maps asked of it (23 coherent BOs, 74400 KiB, the first 4096 KiB
// refused and VectorKit aborting on the nil buffer), run with the legacy tiers (a refused BO fails) and the current
// ones (it comes from system memory). Also MapKit's 4 KiB with 11 MiB free and the compositor's 16 MiB m2v block with
// the pool full.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../TegraGM20BPoolPolicy.h"

namespace
{
constexpr uint64_t KiB = 1024, MiB = 1024 * KiB;
// the kernel's defaults (TegraGM20BUserClient.cpp kPoolSmallBytes, kPoolFloorBytes, kPoolReserveBytes, kClientPoolBytes)
constexpr tgm_pool::Limits kLimits = {1 * MiB, 16 * MiB, 32 * MiB, 64 * MiB};

struct Pool {
	uint64_t size = 256 * MiB, used = 0;

	uint64_t avail() const
	{
		return size - used;
	}
};

struct Client {
	bool compositor;
	uint64_t held = 0;
};

struct Result {
	unsigned failed = 0, fromPool = 0, fromSystem = 0;
	uint64_t systemBytes = 0;
};

// One coherent BoAlloc as AGXDeviceUserClient::BoAlloc decides it; `legacy`: a refusal fails the BO
bool alloc(Pool &p, Client &c, uint64_t bytes, bool legacy, Result &r)
{
	const char *refused =
		legacy && c.compositor
			? nullptr  // legacy: the compositor passes every tier
			: tgm_pool::refusal(bytes, c.held, p.avail(), kLimits, c.compositor);
	const bool poolHasIt = !refused && p.avail() >= bytes;
	if (legacy && !poolHasIt) {
		r.failed++;
		return false;
	}
	if (tgm_pool::coherentSource(refused, poolHasIt) == tgm_pool::FromPool) {
		p.used += bytes;
		c.held += bytes;
		r.fromPool++;
	} else {
		r.fromSystem++;
		r.systemBytes += bytes;
	}
	return true;
}

#define check(ok, what) EXPECT_TRUE(ok) << (what)

// Maps' 23 BOs (74400 KiB): eighteen of VectorKit's 4096 KiB buffers and five small ones
std::vector<uint64_t> mapsOpening()
{
	std::vector<uint64_t> b(18, 4096 * KiB);
	for (uint64_t k : {256, 128, 128, 112, 48})
		b.push_back(k * KiB);
	return b;
}

Result runR8a(bool legacy, Pool &p, Client &compositor)
{
	p.used = 221 * MiB;
	compositor.held = 63 * MiB;
	Client maps{false};
	Result r;
	for (uint64_t bytes : mapsOpening())
		alloc(p, maps, bytes, legacy, r);
	return r;
}
}  // namespace

// Maps opens with 35 MiB of the pool free
TEST(GM20BPoolPolicy, MapsOpeningAtR8a)
{
	Pool p;
	Client compositor{true};
	const Result before = runR8a(true, p, compositor);
	std::printf("r8a as it was: %u of 23 failed, %u from the pool\n", before.failed,
		    before.fromPool);
	check(before.failed > 0,
	      "reproduced: Maps' 4096 KiB BOs refused with 35 MiB of the pool free");

	Pool q;
	Client c2{true};
	const Result after = runR8a(false, q, c2);
	std::printf("r8a now: %u failed, %u from the pool, %u from system memory (%llu KiB)\n",
		    after.failed, after.fromPool, after.fromSystem,
		    (unsigned long long)(after.systemBytes / KiB));
	check(after.failed == 0, "no BO of Maps fails");
	check(after.fromSystem >= before.failed,
	      "the BOs the tiers kept out came from system memory");
	check(q.avail() >= kLimits.reserveBytes, "the compositor's reserve is still free");
	Result r;
	check(alloc(q, c2, 16 * MiB, false, r) && r.fromPool == 1,
	      "the compositor's next 16 MiB block is the pool's");
}

// MapKit's 4 KiB with 11 MiB free (inside the pool's last 16 MiB)
TEST(GM20BPoolPolicy, MapKitSmallBo)
{
	Pool p;
	p.used = p.size - 11 * MiB;
	Client mapkit{false};
	Result legacy, now;
	alloc(p, mapkit, 4 * KiB, true, legacy);
	alloc(p, mapkit, 4 * KiB, false, now);
	check(legacy.failed == 1, "reproduced: MapKit's 4 KiB refused");
	check(now.failed == 0 && now.fromSystem == 1, "MapKit's 4 KiB from system memory");
}

// The compositor's 16 MiB m2v block with the pool full
TEST(GM20BPoolPolicy, CompositorBlockWithPoolFull)
{
	Pool p;
	p.used = p.size - 2 * MiB;
	Client compositor{true};
	Result legacy, now;
	alloc(p, compositor, 16 * MiB, true, legacy);
	alloc(p, compositor, 16 * MiB, false, now);
	check(legacy.failed == 1, "reproduced: the compositor's 16 MiB refused with the pool full");
	check(now.failed == 0 && now.fromSystem == 1, "the compositor's 16 MiB from system memory");
}

// The compositor near the floor: its BO comes from system memory, the floor stays for nvkm's tables and channels
TEST(GM20BPoolPolicy, CompositorNearTheFloor)
{
	Pool was, p;
	was.used = p.used = p.size - 24 * MiB;
	Client compositor{true}, compositor2{true};
	Result legacy, now;
	alloc(was, compositor, 16 * MiB, true, legacy);
	alloc(p, compositor2, 16 * MiB, false, now);
	check(legacy.fromPool == 1 && was.avail() < kLimits.floorBytes,
	      "as it was: the compositor took the pool's last 16 MiB");
	check(now.fromSystem == 1 && p.avail() == 24 * MiB,
	      "now: the floor stays nvkm's, the compositor's block comes from system memory");
}

// Room for everyone: nothing changes
TEST(GM20BPoolPolicy, RoomForEveryone)
{
	Pool p;
	p.used = 64 * MiB;
	Client app{false};
	Result r;
	for (int i = 0; i < 8; i++)
		alloc(p, app, 4 * MiB, false, r);
	check(r.fromPool == 8 && r.fromSystem == 0, "with room, every BO is the pool's as before");
	alloc(p, app, 64 * MiB, false, r);  // past the client's 64 MiB share
	check(r.fromSystem == 1 && r.failed == 0,
	      "past its share, a client's BO comes from system memory");
}

// Compressed targets on boot 11's pool (54 of 256 MiB used): the compositor's full-screen RGBA16F memoryless targets
// (7296 KiB each) come from the pool down to its floor, which stays nvkm's; its next 16 MiB coherent block then comes
// from system memory, and an app's compressed asks stop at the same floor
TEST(GM20BPoolPolicy, CompressedTargetsLeaveTheFloor)
{
	Pool p;
	p.used = 54 * MiB;
	Client compositor{true};
	unsigned made = 0;
	while (!tgm_pool::compressibleRefusal(7296 * KiB, p.avail(), kLimits)) {
		p.used += 7296 * KiB;
		compositor.held += 7296 * KiB;
		made++;
	}
	EXPECT_EQ(made, 26u);
	EXPECT_GE(p.avail(), kLimits.floorBytes);
	EXPECT_LT(p.avail(), kLimits.floorBytes + 7296 * KiB);
	Result r;
	alloc(p, compositor, 16 * MiB, false, r);
	check(r.fromSystem == 1 && r.failed == 0,
	      "the compositor's coherent block comes from system memory, the floor stays nvkm's");
	while (!tgm_pool::compressibleRefusal(128 * KiB, p.avail(), kLimits))
		p.used += 128 * KiB;
	check(p.avail() >= kLimits.floorBytes && p.avail() < kLimits.floorBytes + 128 * KiB,
	      "an app's compressed asks stop within one comptag line of the floor");
	Pool fresh;
	check(tgm_pool::compressibleRefusal(fresh.size - kLimits.floorBytes, fresh.avail(),
					    kLimits) == nullptr,
	      "no client share: one app may take the pool down to its floor");
}

// Boot 21, an app opening: backboardd held 60 MiB of the pool (12 coherent, 48 compressed) with 55 MiB free, and
// asked for its 1792x1472 BGRA8 target's 12 MiB (kind 0xdb), then its RGBA16F memoryless target's 24 MiB (kind 0xe9).
TEST(GM20BPoolPolicy, Boot21CompositorTargetsTakeTheReserve)
{
	Pool p;
	p.used = p.size - 55 * MiB;
	for (uint64_t bytes : {12 * MiB, 24 * MiB}) {
		const uint64_t avail = p.avail();
		EXPECT_EQ(tgm_pool::compressibleRefusal(bytes, avail, kLimits), nullptr)
			<< bytes / MiB << " MiB at " << avail / MiB << " MiB free";
		p.used += bytes;
	}
	EXPECT_EQ(p.avail(), 19 * MiB);
	EXPECT_STREQ(tgm_pool::compressibleRefusal(4 * MiB, p.avail(), kLimits),
		     "the pool's last 16 MiB stay for nvkm");
}

// Boot 22's three kernel lines (the first refusal of each connection), with the client's pool and the pool's ready
// bytes as the kernel printed them: all three were refused for "the pool's last 48 MiB"
TEST(GM20BPoolPolicy, Boot22LoggedRefusalsAreMade)
{
	struct Ask {
		const char *who;
		uint64_t bytes, heldMiB, freeMiB;
	};

	const Ask asks[] = {
		{"iconservicesagent 13:34:57.679 kind 0xce", 1280 * KiB, 22, 48},
		{"backboardd 13:35:03.337 kind 0xe9", 3200 * KiB, 91, 46},
		{"SwitchHWTest 13:35:40.525 kind 0xdb", 3840 * KiB, 29, 49},
	};
	for (const Ask &a : asks)
		EXPECT_EQ(tgm_pool::compressibleRefusal(a.bytes, a.freeMiB * MiB, kLimits), nullptr)
			<< a.who;
}

// Boot 22 in time order. iconservicesagent asked for 18 compressed buffers from 13:34:57.679 to 13:35:02.955 (21120 KiB,
// every one refused), starting at 48 MiB ready; the rest of the system took 2 MiB more before backboardd's 3200 KiB at
// 13:35:03.337 (46 MiB ready then). Served in order, the compositor's ask after them is still made. At 13:35:40 the test
// app's 3840 and 4096 KiB found 49 MiB ready, iconservicesagent's memory given back by then (its pool 28.6 MiB at
// 13:35:18, 7.5 MiB at 13:35:38) and backboardd's 3200 KiB still held.
TEST(GM20BPoolPolicy, Boot22InOrderTheCompositorIsStillServed)
{
	const uint64_t icon[] = {1280, 1536, 1536, 256,  640,  1024, 512,  1280, 1536,
				 1536, 128,  128,  1536, 1536, 1280, 2048, 2048, 1280};
	Pool p;
	p.used = p.size - 48 * MiB;
	uint64_t iconBytes = 0;
	for (uint64_t k : icon) {
		ASSERT_EQ(tgm_pool::compressibleRefusal(k * KiB, p.avail(), kLimits), nullptr)
			<< k << " KiB at " << p.avail() / KiB << " KiB ready";
		p.used += k * KiB;
		iconBytes += k * KiB;
	}
	EXPECT_EQ(iconBytes, 21120 * KiB);
	p.used += 2 * MiB;
	EXPECT_EQ(tgm_pool::compressibleRefusal(3200 * KiB, p.avail(), kLimits), nullptr)
		<< "backboardd's target at " << p.avail() / KiB << " KiB ready";
	p.used += 3200 * KiB;
	EXPECT_GE(p.avail(), kLimits.floorBytes);

	Pool later;
	later.used = later.size - 49 * MiB + 3200 * KiB;
	for (uint64_t k : {3840, 4096}) {
		EXPECT_EQ(tgm_pool::compressibleRefusal(k * KiB, later.avail(), kLimits), nullptr)
			<< "the test app's " << k << " KiB";
		later.used += k * KiB;
	}
}

// Boot 22's share of the pool no client holds (nvkm's page tables and channels, the kernel's own pages: the stats2
// line's used less compositor, clients, compressible and orphans) peaked at 55 MiB and grew by at most 8 MiB in a 5 s
// window (13:34:37 to :42). The floor every compressed buffer leaves holds two such steps.
TEST(GM20BPoolPolicy, TheFloorHoldsTwoOfNvkmsLargestSteps)
{
	constexpr uint64_t largestStep = 8 * MiB;
	EXPECT_GE(kLimits.floorBytes, 2 * largestStep);
}

// A client whose compressed memory passes its 64 MiB share (pool_bytes_ counts both kinds) gets its larger coherent BOs
// from system memory, never a failure
TEST(GM20BPoolPolicy, PastItsShareAnAppsCoherentBosComeFromSystemMemory)
{
	Pool p;
	p.used = 120 * MiB;
	Client app{false, 70 * MiB};
	Result r;
	alloc(p, app, 4 * MiB, false, r);
	check(r.fromSystem == 1 && r.failed == 0, "its 4 MiB coherent BO from system memory");
}
