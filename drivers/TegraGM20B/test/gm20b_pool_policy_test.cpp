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
// (7296 KiB each) come from the pool until it holds 48 MiB free, and then its next 16 MiB m2v block still does
TEST(GM20BPoolPolicy, CompressedTargetsLeaveCoherentRoom)
{
	Pool p;
	p.used = 54 * MiB;
	Client compositor{true};
	unsigned made = 0;
	while (!tgm_pool::compressibleRefusal(7296 * KiB, compositor.held, p.avail(), kLimits,
					      true)) {
		p.used += 7296 * KiB;
		compositor.held += 7296 * KiB;
		made++;
	}
	EXPECT_EQ(made, 21u);
	EXPECT_GE(p.avail(), kLimits.floorBytes + kLimits.reserveBytes);
	Result r;
	alloc(p, compositor, 16 * MiB, false, r);
	check(r.fromPool == 1, "the compositor's m2v block still comes from the pool");
	Client app{false};
	check(tgm_pool::compressibleRefusal(128 * KiB, app.held, p.avail(), kLimits, false) !=
		      nullptr,
	      "an app's compressed target waits for room as the compositor's do");
	Pool fresh;
	check(tgm_pool::compressibleRefusal(64 * MiB, 0, fresh.avail(), kLimits, false) ==
			      nullptr &&
		      tgm_pool::compressibleRefusal(64 * MiB + 128 * KiB, 0, fresh.avail(), kLimits,
						    false) != nullptr,
	      "an app's compressed targets stay within its share");
}
