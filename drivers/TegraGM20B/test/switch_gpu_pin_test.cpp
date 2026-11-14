// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include "switch_gpu_pin_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuPin.h"

struct switch_gpu_backing {};

struct switch_gpu_cached_mapping {
	unsigned identity;
};

namespace
{
switch_gpu_backing backing;
void *owner;
void (*releaseOwner)(void *);
bool rejected, existing;
unsigned retired;
switch_gpu_cached_mapping *returned;

class CachedPin : public testing::Test
{
	void SetUp() override
	{
		owner = nullptr;
		releaseOwner = nullptr;
		rejected = existing = false;
		retired = 0;
		returned = nullptr;
	}

	void TearDown() override
	{
		EXPECT_EQ(owner, nullptr);
	}
};
}

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *, uint32_t, uint32_t,
							   void *value, void (*release)(void *))
{
	if (rejected)
		return nullptr;
	if (existing)
		release(value);
	else {
		owner = value;
		releaseOwner = release;
	}
	return &backing;
}

IOMemoryMap *returnedMap;

void switch_gpu_cached_retire(SwitchGpuCachedBuffer buffer, uint64_t *pages, uint32_t count,
			      switch_gpu_cached_mapping *mapping)
{
	EXPECT_EQ(count, 4u);
	EXPECT_EQ(pages[3], 0x4000u);
	++retired;
	returned = mapping;
	returnedMap = buffer.map;
	buffer.memory->complete();
	buffer.memory->release();
	IODelete(pages, uint64_t, count);
}

extern "C" void tgm_pool_free_chunks(const uint64_t *, uint32_t)
{
}

namespace
{
constexpr std::array<uint64_t, 4> pages = {0x1000, 0x2000, 0x3000, 0x4000};

TEST_F(CachedPin, PersistentMappingTravelsWithLastBackingOwner)
{
	IOMemoryDescriptor memory;
	switch_gpu_cached_mapping mapping{9};
	ASSERT_EQ(switch_gpu_pin_cached(pages.data(), 4, 2, {&memory, nullptr, nullptr}, &mapping),
		  &backing);
	EXPECT_EQ(retired, 0u);
	EXPECT_EQ(memory.refs, 2u);
	EXPECT_EQ(memory.prepared, 1u);
	releaseOwner(owner);
	owner = nullptr;
	EXPECT_EQ(returned, &mapping);
	EXPECT_EQ(retired, 1u);
	EXPECT_EQ(memory.refs, 1u);
	EXPECT_EQ(memory.prepared, 0u);
}

// A spare made of 128 KiB units is mapped at one kernel address by its own IOMemoryMap: the backing's last owner hands
// that map on with the buffer, so the buffer goes back to the spares whole.
TEST_F(CachedPin, AUnitsBuffersKernelMapTravelsWithTheBuffer)
{
	IOMemoryDescriptor memory;
	IOMemoryMap map{7};
	switch_gpu_cached_mapping mapping{9};
	returnedMap = nullptr;
	ASSERT_EQ(switch_gpu_pin_cached(pages.data(), 4, 2, {&memory, &map, &map}, &mapping),
		  &backing);
	releaseOwner(owner);
	owner = nullptr;
	EXPECT_EQ(returnedMap, &map);
	EXPECT_EQ(memory.refs, 1u);
}

TEST_F(CachedPin, RefusedRegistrationReturnsMappingOwnershipToCaller)
{
	IOMemoryDescriptor memory;
	switch_gpu_cached_mapping mapping{9};
	rejected = true;
	EXPECT_EQ(switch_gpu_pin_cached(pages.data(), 4, 2, {&memory, nullptr, nullptr}, &mapping),
		  nullptr);
	EXPECT_EQ(retired, 0u);
	EXPECT_EQ(memory.refs, 1u);
	EXPECT_EQ(memory.prepared, 0u);
}

TEST_F(CachedPin, ExistingAuthorityDisposesOnlyTheExtraMappingOwner)
{
	IOMemoryDescriptor memory;
	switch_gpu_cached_mapping mapping{9};
	existing = true;
	EXPECT_EQ(switch_gpu_pin_cached(pages.data(), 4, 2, {&memory, nullptr, nullptr}, &mapping),
		  &backing);
	EXPECT_EQ(retired, 1u);
	EXPECT_EQ(returned, &mapping);
	EXPECT_EQ(memory.refs, 1u);
	EXPECT_EQ(memory.prepared, 0u);
}

TEST_F(CachedPin, FailedPrepareDoesNotConsumeMapping)
{
	IOMemoryDescriptor memory;
	switch_gpu_cached_mapping mapping{9};
	memory.refuse = true;
	EXPECT_EQ(switch_gpu_pin_cached(pages.data(), 4, 2, {&memory, nullptr, nullptr}, &mapping),
		  nullptr);
	EXPECT_EQ(retired, 0u);
	EXPECT_EQ(memory.refs, 1u);
	EXPECT_EQ(memory.prepared, 0u);
}
}  // namespace
