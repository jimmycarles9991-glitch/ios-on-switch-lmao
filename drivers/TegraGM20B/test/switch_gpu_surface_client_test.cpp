// SPDX-License-Identifier: MIT
#include <map>
#include <gtest/gtest.h>
#include "gm20b_surface_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/SwitchGpuSurfaceGeometry.h"
#include "drivers/TegraGM20B/TegraGM20BRecord.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"

namespace
{
struct Surface : OSObject {
	IOMemoryDescriptor memory;
	SwitchGpuSurfaceGeometry geometry = {};

	Surface(uint64_t bytes, uint64_t first)
	{
		memory.bytes = bytes;
		for (uint64_t offset = 0; offset < ((bytes + 16383) & ~uint64_t(16383));
		     offset += 4096)
			memory.pages.push_back(first + offset);
		geometry = {64, 64, 512, bytes, 8, 1, 1, 0, false};
	}
};

std::map<uint32_t, Surface *> visible;
std::map<uint32_t, switch_gpu_backing *> hints;
std::map<uint64_t, switch_gpu_backing *> allocations;
int prepareResult;
unsigned prepareCalls, prepareOptions;
OSObject *preparedObject;
}

struct switch_gpu_backing {
	uint32_t refs = 1;
	std::vector<uint64_t> pages;
};

extern "C" {
void switch_gpu_surface_idle(void *)
{
}

int switch_gpu_surface_adopt(void *, switch_gpu_backing *)
{
	return 0;
}

OSObject *tegra_gm20b_ios_lookup(IOService *, uint32_t id, task_t)
{
	auto *surface = visible[id];
	if (surface)
		surface->retain();
	return surface;
}

IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *s)
{
	return &static_cast<Surface *>(s)->memory;
}

uint64_t tegra_vic_ios_alloc_size(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.bytes;
}

uint64_t tegra_vic_ios_width(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.width;
}

uint64_t tegra_vic_ios_height(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.height;
}

uint64_t tegra_vic_ios_bytes_per_row(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.pitch;
}

uint32_t tegra_vic_ios_plane_count(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.planes;
}

uint32_t tegra_gm20b_ios_bpe(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.bpp;
}

uint32_t tegra_gm20b_ios_element_width(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.element_width;
}

uint32_t tegra_gm20b_ios_element_height(OSObject *s)
{
	return static_cast<Surface *>(s)->geometry.element_height;
}

uint32_t gSeed;

uint32_t tegra_gm20b_ios_seed(OSObject *)
{
	return gSeed;
}

switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t id)
{
	auto *backing = hints[id];
	if (backing)
		++backing->refs;
	return backing;
}

switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t pa)
{
	auto *backing = allocations[pa];
	if (backing)
		++backing->refs;
	return backing;
}

void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	--backing->refs;
}

int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **, int)
{
	return -22;
}

int switch_native_surface_prepare(OSObject *object, uint32_t, void *, uint32_t options,
				  switch_gpu_backing **backing, switch_gpu_cpu_lease **owned)
{
	auto *surface = static_cast<Surface *>(object);
	EXPECT_GT(surface->memory.cpuUsers, 0u);
	EXPECT_GT(surface->refs, 1u);
	++prepareCalls;
	prepareOptions = options;
	preparedObject = object;
	*backing = nullptr;
	*owned = nullptr;
	return prepareResult;
}

int switch_native_surface_gpu_admission(OSObject *object, uint32_t, switch_gpu_backing *)
{
	EXPECT_GT(static_cast<Surface *>(object)->memory.cpuUsers, 0u);
	return 1;
}

int switch_native_surface_prepare_gpu(OSObject *object, uint32_t id, void *task, uint32_t options,
				      switch_gpu_backing **backing, switch_gpu_cpu_lease **owned)
{
	return switch_native_surface_prepare(object, id, task, options, backing, owned);
}

void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
	*info = {};
	info->pages = backing->pages.data();
	info->page_count = uint32_t(backing->pages.size());
}
}

namespace
{
class SurfaceClient : public testing::Test
{
protected:
	void SetUp() override
	{
		visible.clear();
		hints.clear();
		allocations.clear();
		prepareResult = 0;
		prepareCalls = 0;
	}
};

TEST_F(SurfaceClient, UnimportedCpuLockRetainsDescriptorBeforeCheckingNativeAttachment)
{
	Surface surface(32768, 0x90000000);
	visible[4] = &surface;
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	ASSERT_EQ(switch_gpu_surface_prepare(4, nullptr, 1, &backing, &owned, &descriptor), 0);
	EXPECT_EQ(backing, nullptr);
	EXPECT_EQ(owned, nullptr);
	EXPECT_EQ(descriptor, &surface.memory);
	EXPECT_EQ(surface.memory.cpuUsers, 1u);
	EXPECT_EQ(surface.memory.refs, 2u);
	EXPECT_EQ(surface.memory.prepared, 0u);
	EXPECT_EQ(prepareCalls, 1u);
	EXPECT_EQ(surface.refs, 1u);
	switch_gpu_surface_finish(descriptor);
	EXPECT_EQ(surface.memory.cpuUsers, 0u);
	EXPECT_EQ(surface.memory.refs, 1u);
}

TEST_F(SurfaceClient, AttachmentPreparationFailureDropsTheDescriptorReservation)
{
	Surface surface(32768, 0x90000000);
	visible[4] = &surface;
	prepareResult = -116;
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	EXPECT_EQ(switch_gpu_surface_prepare(4, nullptr, 1, &backing, &owned, &descriptor), -116);
	EXPECT_EQ(descriptor, nullptr);
	EXPECT_EQ(surface.memory.cpuUsers, 0u);
	EXPECT_EQ(surface.memory.refs, 1u);
	EXPECT_EQ(surface.refs, 1u);
}

TEST_F(SurfaceClient, AvoidSyncCannotPassAnActiveDescriptorPublication)
{
	Surface surface(32768, 0x90000000);
	visible[4] = &surface;
	surface.memory.exclusive = true;
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	EXPECT_EQ(switch_gpu_surface_prepare(4, nullptr, 3, &backing, &owned, &descriptor), -16);
	EXPECT_EQ(descriptor, nullptr);
	EXPECT_EQ(prepareCalls, 0u);
	EXPECT_EQ(surface.memory.cpuUsers, 0u);
	EXPECT_EQ(surface.memory.refs, 1u);
	EXPECT_EQ(surface.refs, 1u);
}

TEST_F(SurfaceClient, LayoutMoveCannotOverlapAnOpenCpuLockOrAdmitOneWhileMoving)
{
	Surface surface(32768, 0x90000000);
	visible[4] = &surface;
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	ASSERT_EQ(switch_gpu_surface_prepare(4, nullptr, 1, &backing, &owned, &descriptor), 0);
	EXPECT_EQ(switch_gpu_surface_layout_exclusive(&surface, true), -16);
	switch_gpu_surface_finish(descriptor);
	ASSERT_EQ(switch_gpu_surface_layout_exclusive(&surface, true), 0);
	EXPECT_EQ(switch_gpu_surface_prepare(4, nullptr, 3, &backing, &owned, &descriptor), -16);
	EXPECT_EQ(switch_gpu_surface_layout_exclusive(&surface, false), 0);
	EXPECT_EQ(surface.memory.refs, 1u);
}

TEST_F(SurfaceClient, NativeTailAttestationNeverQueriesBeyondTheDescriptor)
{
	for (uint64_t bytes : {uint64_t(1344 * 77), uint64_t(576 * 72), uint64_t(1088 * 136)}) {
		Surface surface(bytes, 0x80000000);
		visible[4] = &surface;
		EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, surface.memory.pages.data(),
							 uint32_t(surface.memory.pages.size())),
			  1)
			<< bytes;
		EXPECT_EQ(surface.memory.outside, 0u);
		EXPECT_EQ(surface.memory.prepared, surface.memory.completed);
		EXPECT_EQ(surface.refs, 1u);
	}
}

TEST_F(SurfaceClient, AnAttestedImportRetainsItsActualSurfaceIdentity)
{
	Surface current(32768, 0x90000000);
	visible[4] = &current;
	auto *held = switch_gpu_surface_hold(4, nullptr, current.memory.pages.data(), 8);
	ASSERT_EQ(held, &current);
	EXPECT_EQ(current.refs, 2u);
	held->release();
	EXPECT_EQ(current.refs, 1u);
	auto wrong = current.memory.pages;
	wrong.front() += 16384;
	EXPECT_EQ(switch_gpu_surface_hold(4, nullptr, wrong.data(), 8), nullptr);
	EXPECT_EQ(current.refs, 1u);
}

TEST_F(SurfaceClient, CpuOwnershipFollowsCurrentPagesAcrossNumericIdReuse)
{
	Surface current(32768, 0x90000000), old(65536, 0x80000000);
	switch_gpu_backing retiring{1, old.memory.pages}, live{1, current.memory.pages};
	visible[4] = &current;
	hints[4] = &retiring;
	allocations[old.memory.pages[0]] = &retiring;
	allocations[current.memory.pages[0]] = &live;
	switch_gpu_backing *resolved = nullptr;
	ASSERT_EQ(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, &live);
	EXPECT_EQ(retiring.refs, 1u);
	EXPECT_EQ(live.refs, 2u);
	switch_gpu_backing_release(resolved);
	EXPECT_EQ(current.memory.outside, 0u);
	EXPECT_EQ(current.memory.prepared, current.memory.completed);
	EXPECT_EQ(current.refs, 1u);
}

TEST_F(SurfaceClient, ReusedIdForAnUnimportedSurfaceDoesNotBorrowRetiringOwnership)
{
	Surface current(32768, 0x90000000), old(65536, 0x80000000);
	switch_gpu_backing retiring{1, old.memory.pages};
	visible[4] = &current;
	hints[4] = &retiring;
	allocations[old.memory.pages[0]] = &retiring;
	switch_gpu_backing *resolved = nullptr;
	EXPECT_EQ(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, nullptr);
	EXPECT_EQ(retiring.refs, 1u);
}

TEST_F(SurfaceClient, ASharedFirstPageCannotAttestDifferentRemainingPages)
{
	Surface current(32768, 0x90000000);
	switch_gpu_backing bad{1, current.memory.pages};
	bad.pages.back() += 16384;
	visible[4] = &current;
	hints[4] = &bad;
	allocations[current.memory.pages[0]] = &bad;
	switch_gpu_backing *resolved = nullptr;
	EXPECT_NE(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, nullptr);
	EXPECT_EQ(bad.refs, 1u);
}

TEST_F(SurfaceClient, GeometryIsTheUnderlyingSurfaceRegardlessOfTheFirstTextureView)
{
	Surface current(32768, 0x90000000);
	visible[4] = &current;
	SwitchGpuSurfaceGeometry geometry = {};
	ASSERT_EQ(
		switch_gpu_surface_geometry(4, nullptr, current.memory.pages.data(), 8, &geometry),
		0);
	EXPECT_TRUE(switch_gpu_surface_packed(geometry));
	EXPECT_FALSE(switch_gpu_surface_shape_matches(geometry, 42, 42, 8, 512));
	EXPECT_TRUE(switch_gpu_surface_shape_matches(geometry, 64, 64, 8, 512));
	tgm_layout layout = {};
	ASSERT_TRUE(tgm_layout_tiled(uint32_t(geometry.width), uint32_t(geometry.height),
				     geometry.bpp, &layout));
	EXPECT_EQ(layout.row_bytes, 512u);
	EXPECT_EQ(layout.bytes, 32768u);
}

TEST_F(SurfaceClient, BlocksAndPlanesCannotBecomePackedColorLayouts)
{
	SwitchGpuSurfaceGeometry geometry{64, 64, 512, 32768, 8, 1, 1, 0, false};
	geometry.element_width = 2;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
	geometry.element_width = 1;
	geometry.planes = 2;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
	geometry.planes = 0;
	geometry.bytes = 32767;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
}

TEST_F(SurfaceClient, AnInaccessibleOrUnpreparableSurfaceCannotAttestPages)
{
	Surface current(32768, 0x90000000);
	EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, current.memory.pages.data(), 8), 0);
	visible[4] = &current;
	current.memory.failPrepare = true;
	EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, current.memory.pages.data(), 8), 0);
	EXPECT_EQ(current.memory.prepared, 0u);
	EXPECT_EQ(current.memory.completed, 0u);
	EXPECT_EQ(current.refs, 1u);
}
}

TEST_F(SurfaceClient, CpuAccessUsesRetainedObjectAndPreservesAvoidSync)
{
	Surface actual(16384, 0x100000), unrelated(16384, 0x200000);
	visible[4] = &unrelated;
	actual.retain();
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	ASSERT_EQ(switch_gpu_surface_prepare_cpu(&actual, 4, nullptr,
						 SWITCH_GPU_BACKING_READ |
							 SWITCH_GPU_BACKING_CPU_AVOID_SYNC,
						 &backing, &owned, &descriptor),
		  0);
	EXPECT_EQ(preparedObject, &actual);
	EXPECT_EQ(prepareOptions, 3u);
	EXPECT_EQ(actual.memory.cpuUsers, 1u);
	EXPECT_EQ(unrelated.memory.cpuUsers, 0u);
	switch_gpu_surface_finish(descriptor);
	EXPECT_EQ(actual.memory.cpuUsers, 0u);
	actual.release();
}

TEST_F(SurfaceClient, MutableCpuAccessInvokesNativeDetachmentAndUnwindsFailure)
{
	Surface actual(16384, 0x100000);
	actual.retain();
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	prepareResult = -5;
	EXPECT_EQ(switch_gpu_surface_prepare_cpu(&actual, 4, nullptr, SWITCH_GPU_BACKING_WRITE,
						 &backing, &owned, &descriptor),
		  -5);
	EXPECT_EQ(prepareOptions, 0u);
	EXPECT_EQ(descriptor, nullptr);
	EXPECT_EQ(actual.memory.cpuUsers, 0u);
	actual.release();
}

TEST_F(SurfaceClient, GpuPrepareAlwaysUsesAvoidSyncAndRetainsDescriptorUntilFinish)
{
	Surface actual(32768, 0x44000000);
	actual.retain();
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	ASSERT_EQ(switch_gpu_surface_prepare_gpu(&actual, 4, nullptr, SWITCH_GPU_BACKING_READ,
						 &backing, &owned, &descriptor),
		  0);
	EXPECT_EQ(prepareOptions, 3u);
	EXPECT_EQ(actual.memory.cpuUsers, 1u);
	switch_gpu_surface_finish(descriptor);
	EXPECT_EQ(actual.memory.cpuUsers, 0u);
	actual.release();
}

TEST_F(SurfaceClient, GpuAdmissionChecksPublicationGateWithoutPreparingPixels)
{
	Surface actual(32768, 0x44000000);
	switch_gpu_backing backing;
	actual.memory.exclusive = true;
	EXPECT_EQ(switch_gpu_surface_gpu_admission(&actual, &backing, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(actual.memory.cpuUsers, 0u);
	actual.memory.exclusive = false;
	EXPECT_EQ(switch_gpu_surface_gpu_admission(&actual, &backing, SWITCH_GPU_BACKING_READ), 1);
	EXPECT_EQ(actual.memory.cpuUsers, 0u);
	EXPECT_EQ(prepareCalls, 0u);
}

// A surface reads unwritten only at the seed its shared entry was made with and made over memory of its own
TEST(SurfaceUnwritten, OnlyTheFirstSeedOverItsOwnMemory)
{
	struct Raw : OSObject {
		uint8_t bytes[0x400] = {};
	} raw;

	auto set = [&](unsigned long offset, uint64_t value) {
		memcpy(reinterpret_cast<uint8_t *>(&raw) + offset, &value, sizeof(value));
	};
	gSeed = 0;
	EXPECT_TRUE(tgm_surface_unwritten(&raw));
	EXPECT_FALSE(tgm_surface_unwritten(nullptr));
	gSeed = 1;
	EXPECT_FALSE(tgm_surface_unwritten(&raw));
	gSeed = 0;
	for (const unsigned long field :
	     {OriginalIOSurface::clientMemory, OriginalIOSurface::parent,
	      OriginalIOSurface::memoryRegions}) {
		set(field, 0x1000);
		EXPECT_FALSE(tgm_surface_unwritten(&raw)) << std::hex << field;
		set(field, 0);
	}
	EXPECT_TRUE(tgm_surface_unwritten(&raw));
}
