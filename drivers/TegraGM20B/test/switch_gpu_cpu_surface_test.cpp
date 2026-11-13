// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuCpuSurface.h"

struct switch_gpu_backing {
	unsigned refs = 0;
};

struct switch_gpu_cpu_lease {
	switch_gpu_backing *backing;
};

namespace
{
struct Surface {
	switch_gpu_backing *backing;
	bool native = true;
	bool detached = false;
	unsigned tickets = 0;
	int error = 0;
};

unsigned acquired, transferred, released, dropped;
int acquireError, transferError;
uint64_t actualOffset, actualBytes;
uint32_t actualAccess;
Surface *preparing;

class CpuSurface : public testing::Test
{
protected:
	switch_gpu_backing backing;
	Surface surface{&backing};
	SwitchGpuCpuSurface reservation;

	void SetUp() override
	{
		acquired = transferred = released = dropped = 0;
		acquireError = transferError = 0;
		preparing = nullptr;
	}

	void TearDown() override
	{
		reservation.release(&backing);
		EXPECT_EQ(backing.refs, 0u);
		EXPECT_EQ(surface.tickets, 0u);
	}

	int acquire(uint32_t access = SWITCH_GPU_BACKING_READ)
	{
		return reservation.acquire(&backing, &surface, 7, nullptr, 64, 128, access);
	}
};
}

extern "C" int switch_gpu_surface_prepare_cpu(void *opaque, uint32_t, void *, uint32_t access,
					      switch_gpu_backing **logical,
					      switch_gpu_cpu_lease **owned, void **descriptor)
{
	auto *surface = static_cast<Surface *>(opaque);
	if (surface->error)
		return surface->error;
	++surface->tickets;
	*descriptor = surface;
	preparing = surface;
	if (surface->native) {
		*logical = surface->backing;
		(*logical)->refs += 2;
		*owned = new switch_gpu_cpu_lease{*logical};
		surface->detached = access & SWITCH_GPU_BACKING_WRITE;
	}
	return 0;
}

extern "C" void switch_gpu_surface_finish(void *descriptor)
{
	EXPECT_FALSE(static_cast<Surface *>(descriptor)->tickets == 0);
	--static_cast<Surface *>(descriptor)->tickets;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	--backing->refs;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *info)
{
	*info = {};
	info->page_count = 4;
}

extern "C" int switch_gpu_backing_cpu_acquire(switch_gpu_backing *backing, uint64_t offset,
					      uint64_t bytes, uint32_t access)
{
	++acquired;
	actualOffset = offset;
	actualBytes = bytes;
	actualAccess = access;
	if (!acquireError)
		++backing->refs;
	return acquireError;
}

extern "C" int switch_gpu_backing_cpu_transfer_owned(switch_gpu_backing *backing,
						     switch_gpu_cpu_lease **owned, uint32_t access)
{
	++transferred;
	actualAccess = access;
	EXPECT_EQ((*owned)->backing, backing);
	EXPECT_GT(backing->refs, 0u);
	EXPECT_EQ(preparing->tickets, 1u);
	if (!transferError) {
		delete *owned;
		*owned = nullptr;
	}
	return transferError;
}

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
	EXPECT_EQ(wrote, 0);
	++dropped;
	--(*owned)->backing->refs;
	delete *owned;
	*owned = nullptr;
	return 0;
}

extern "C" int switch_gpu_backing_cpu_release(switch_gpu_backing *backing, uint64_t offset,
					      uint64_t bytes, uint32_t access)
{
	++released;
	--backing->refs;
	actualOffset = offset;
	actualBytes = bytes;
	actualAccess = access;
	if (preparing)
		EXPECT_EQ(preparing->tickets, 1u);
	return 0;
}

TEST_F(CpuSurface, OrdinaryBackingKeepsExactRangeAndDescriptorUntilRelease)
{
	surface.native = false;
	ASSERT_EQ(acquire(), 0);
	EXPECT_EQ(acquired, 1u);
	EXPECT_EQ(transferred, 0u);
	EXPECT_EQ(actualOffset, 64u);
	EXPECT_EQ(actualBytes, 128u);
	EXPECT_EQ(surface.tickets, 1u);
	EXPECT_EQ(reservation.release(&backing), 0);
	EXPECT_EQ(surface.tickets, 0u);
}

TEST_F(CpuSurface, NativeReadTransfersWithoutDroppingOwnedReservation)
{
	ASSERT_EQ(acquire(), 0);
	EXPECT_EQ(acquired, 0u);
	EXPECT_EQ(transferred, 1u);
	EXPECT_EQ(dropped, 0u);
	EXPECT_EQ(surface.tickets, 1u);
	EXPECT_FALSE(surface.detached);
	EXPECT_EQ(backing.refs, 1u);
	ASSERT_EQ(reservation.release(&backing), 0);
	EXPECT_EQ(actualOffset, 0u);
	EXPECT_EQ(actualBytes, 16384u);
}

TEST_F(CpuSurface, NativeWriteDetachesBeforeCpuAccessAndReleasesCanonicalRange)
{
	ASSERT_EQ(acquire(SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_DISCARD), 0);
	EXPECT_TRUE(surface.detached);
	EXPECT_EQ(actualAccess, uint32_t(SWITCH_GPU_BACKING_WRITE));
	ASSERT_EQ(reservation.release(&backing), 0);
	EXPECT_EQ(actualBytes, 16384u);
}

TEST_F(CpuSurface, AvoidSyncSurvivesOwnershipTransfer)
{
	ASSERT_EQ(acquire(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC), 0);
	EXPECT_EQ(actualAccess,
		  uint32_t(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC));
}

TEST_F(CpuSurface, FailedTransferDropsReturnedLeaseAndDescriptor)
{
	transferError = -16;
	EXPECT_EQ(acquire(), -16);
	EXPECT_EQ(dropped, 1u);
	EXPECT_EQ(backing.refs, 0u);
	EXPECT_EQ(surface.tickets, 0u);
}

TEST_F(CpuSurface, IncompatibleCanonicalBackingRefusesWithoutLeaking)
{
	switch_gpu_backing other;
	surface.backing = &other;
	EXPECT_EQ(acquire(), -95);
	EXPECT_EQ(transferred, 0u);
	EXPECT_EQ(other.refs, 0u);
}

TEST_F(CpuSurface, FailedOrdinaryAcquireReleasesDescriptor)
{
	surface.native = false;
	acquireError = -5;
	EXPECT_EQ(acquire(), -5);
	EXPECT_EQ(surface.tickets, 0u);
	EXPECT_EQ(backing.refs, 0u);
}

TEST_F(CpuSurface, FailedPreparationDoesNotAcquireOrReleaseBytes)
{
	surface.error = -16;
	EXPECT_EQ(acquire(), -16);
	EXPECT_EQ(acquired, 0u);
	EXPECT_EQ(transferred, 0u);
	EXPECT_EQ(released, 0u);
}

TEST_F(CpuSurface, NonSurfaceBackingDoesNotEnterDescriptorProtocol)
{
	ASSERT_EQ(reservation.acquire(&backing, nullptr, 0, nullptr, 8192, 4096,
				      SWITCH_GPU_BACKING_WRITE),
		  0);
	EXPECT_EQ(actualOffset, 8192u);
	EXPECT_EQ(actualBytes, 4096u);
	EXPECT_EQ(surface.tickets, 0u);
}

TEST_F(CpuSurface, NativeMaterializationCannotClaimUnattestedPreserveDirtyContract)
{
	EXPECT_EQ(acquire(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC |
			  SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY),
		  -95);
	EXPECT_EQ(transferred, 0u);
	EXPECT_EQ(backing.refs, 0u);
	EXPECT_EQ(surface.tickets, 0u);
}
