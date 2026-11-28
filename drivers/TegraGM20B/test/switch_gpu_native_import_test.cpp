// SPDX-License-Identifier: MIT
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/SwitchGpuBoIndex.h"
#include "drivers/TegraGM20B/SwitchGpuNativeSurface.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "gm20b_gpu_memory.h"
#include "kits/SwitchRefusal/SwitchRefusal.h"

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = -22, kIOReturnUnsupported = -95,
	      kIOReturnNoMemory = -12, kIOReturnAborted = -125;
#define IONewZero(T, n) static_cast<T *>(calloc(n, sizeof(T)))
#define IODelete(p, T, n) free(p)

struct Surface {
	unsigned refs = 1;

	void retain()
	{
		++refs;
	}
};

struct switch_gpu_backing {
	unsigned refs = 0;
};

struct tgm_vm {};

struct tgm_mem {
	const uint64_t *pages;
	uint32_t count;
};

namespace
{
int lockDepth = 1, acquireError, memError;
unsigned acquired, acquiredWrites, released, constructed, directPaLatches;
uint64_t pages[] = {0x100000, 0x101000, 0x102000};
volatile uint64_t representation = 3, payload = 1;
switch_gpu_backing logical, physical;
tgm_mem *existingBackingMem;
switch_gpu_native_surface_view answer;
switch_gpu_backing_info allocation;

int ioreturn(int error)
{
	return error;
}

void IOLockUnlock(void *)
{
	EXPECT_EQ(lockDepth, 1);
	--lockDepth;
}

void IOLockLock(void *)
{
	EXPECT_EQ(lockDepth, 0);
	++lockDepth;
}

int acquire(void *surface, switch_gpu_native_surface_view *out)
{
	EXPECT_NE(surface, nullptr);
	EXPECT_EQ(lockDepth, 0);
	++acquired;
	if (acquireError)
		return acquireError;
	*out = answer;
	++out->native->refs;
	++out->logical->refs;
	return 0;
}

int acquireWrite(void *surface, switch_gpu_native_surface_view *out)
{
	EXPECT_NE(surface, nullptr);
	EXPECT_EQ(lockDepth, 0);
	++acquiredWrites;
	if (acquireError)
		return acquireError;
	*out = answer;
	++out->native->refs;
	++out->logical->refs;
	return 0;
}

void release(switch_gpu_native_surface_view *view)
{
	++released;
	if (view->native)
		--view->native->refs;
	if (view->logical)
		--view->logical->refs;
	*view = {};
}

auto acquireFn = acquire;
auto acquireWriteFn = acquireWrite;
auto releaseFn = release;

int tgm_mem_new(tgm_vm *, const uint64_t *addresses, uint32_t count, tgm_mem **out)
{
	++constructed;
	if (memError)
		return memError;
	*out = new tgm_mem{addresses, count};
	return 0;
}
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *out)
{
	*out = allocation;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *value)
{
	++value->refs;
}

extern "C" int switch_gpu_backing_tgm_mem_existing_or_direct(switch_gpu_backing *, uint32_t,
							     tgm_mem **out)
{
	if (!existingBackingMem) {
		++directPaLatches;
		return -2;
	}
	*out = existingBackingMem;
	return 0;
}

class AGXDeviceUserClient
{
public:
	struct Bo {
		Bo *next;
		uint64_t handle, bytes;
		Surface *surface;
		switch_gpu_native_surface_view *nativeView;
		switch_gpu_backing *backing;
		uint64_t *pas;
		uint32_t npages, backing_pages;
		bool readOnly;
		bool backingMemory;
		bool ownsMem;
		tgm_mem *mem;
	};

	struct Gate {
		bool closing = false;

		bool closed()
		{
			return closing;
		}
	} submitGate_;

	void *lock_ = nullptr;
	tgm_vm *vm_ = nullptr;
	uint64_t next_handle_ = 2;
	uint64_t boGen_ = 1;
	Bo *bos_;

	void bumpBoGen()
	{
		++boGen_;
	}

	struct HostMemory {
		template <typename T> static T *allocate(uint32_t count)
		{
			return static_cast<T *>(malloc(sizeof(T) * count));
		}

		template <typename T> static void release(T *items, uint32_t)
		{
			free(items);
		}
	};

	struct Index : SwitchGpuBoIndex<Bo, HostMemory> {
		~Index()
		{
			destroy();
		}
	} boIndex_;

	Bo *findBo(uint64_t handle)
	{
		for (auto *bo = bos_; bo; bo = bo->next)
			if (bo->handle == handle)
				return bo;
		return nullptr;
	}

	IOReturn nativeSurfaceView(const TegraGM20BAbiNativeSurfaceView *,
				   TegraGM20BAbiNativeSurfaceViewReply *);
};

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t)
{
}
#endif

namespace
{
std::vector<std::string> refusals;
}

extern "C" void switch_refusal(const char *domain, const char *reason_key, const char *format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	refusals.push_back(std::string(domain) + " " + reason_key + ": " + text);
}

#define switch_native_surface_gpu_acquire acquireFn
#define switch_native_surface_gpu_acquire_write acquireWriteFn
#define switch_native_surface_gpu_release releaseFn
#include "drivers/TegraGM20B/SwitchGpuNativeImport.h"
#undef switch_native_surface_gpu_acquire
#undef switch_native_surface_gpu_acquire_write
#undef switch_native_surface_gpu_release

class NativeImport : public testing::Test
{
protected:
	Surface surface;
	AGXDeviceUserClient::Bo source = {};
	AGXDeviceUserClient client;
	TegraGM20BAbiNativeSurfaceView request = {1, 0, 0};
	TegraGM20BAbiNativeSurfaceViewReply reply = {};

	void SetUp() override
	{
		lockDepth = 1;
		acquireError = memError = 0;
		acquired = acquiredWrites = released = constructed = directPaLatches = 0;
		logical = {};
		physical = {};
		existingBackingMem = nullptr;
		representation = 3;
		payload = 1;
		acquireFn = acquire;
		acquireWriteFn = acquireWrite;
		releaseFn = release;
		answer = {};
		answer.descriptor = &surface;
		answer.native = &physical;
		answer.logical = &logical;
		answer.payload = {&logical, &payload, 1};
		answer.representation = {&logical, &representation, 3};
		answer.layout.format = 0x34323076;
		answer.layout.plane_count = 2;
		answer.layout.planes[0] = {
			0, 8192, 256, 32, 256, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
		answer.layout.planes[1] = {
			8192, 4096, 128, 16, 256, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
		answer.crop_width = 240;
		answer.crop_height = 30;
		allocation = {};
		allocation.pages = pages;
		allocation.page_count = 3;
		allocation.allocation_generation = 4;
		source.handle = 1;
		source.surface = &surface;
		client.bos_ = &source;
		refusals.clear();
	}

	void TearDown() override
	{
		while (client.bos_ != &source) {
			auto *bo = client.bos_;
			client.bos_ = bo->next;
			EXPECT_EQ(bo->ownsMem, !bo->backingMemory);
			if (bo->ownsMem)
				delete bo->mem;
			release(bo->nativeView);
			free(bo->nativeView);
			--bo->backing->refs;
			--bo->surface->refs;
			free(bo);
		}
		EXPECT_EQ(surface.refs, 1u);
		EXPECT_EQ(logical.refs, 0u);
		EXPECT_EQ(physical.refs, 0u);
		EXPECT_EQ(lockDepth, 1);
	}
};

TEST_F(NativeImport, NativePlanesShareOneReadOnlyBoAndBorrowRetainedPersistentPages)
{
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	ASSERT_NE(reply.handle, 0u);
	EXPECT_EQ(reply.kind, 0xfeu);
	EXPECT_EQ(reply.plane_count, 2u);
	EXPECT_EQ(reply.representation_generation, 3u);
	EXPECT_EQ(reply.allocation_generation, 4u);
	EXPECT_EQ(reply.planes[1].offset, 8192u);
	EXPECT_EQ(reply.crop_width, 240u);
	const auto *bo = client.findBo(reply.handle);
	EXPECT_TRUE(bo->readOnly);
	EXPECT_EQ(bo->pas, pages);
	EXPECT_EQ(bo->mem->pages, pages);
	EXPECT_EQ(bo->nativeView->representation.expected, 3u);
	EXPECT_EQ(physical.refs, 2u);
	EXPECT_EQ(logical.refs, 1u);
	EXPECT_EQ(acquired, 1u);
}

// The runtime binds a native view (Kernel::nativeSurfaceView's bindImport) as the view BO's pages: the decoded picture's
// own, read-only, so a draw samples the media unit's block-linear planes where they lie, not the IOSurface's pitch
// pages a conversion would fill, and can never write the picture.
TEST_F(NativeImport, ADrawSamplesThePicturesOwnPagesReadOnly)
{
	std::map<uint64_t, std::vector<uint8_t>> dram;
	for (uint32_t i = 0; i < 3; ++i)
		dram[pages[i]].assign(4096, uint8_t(0xa0 + i));
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	const auto *bo = client.findBo(reply.handle);
	ASSERT_NE(bo, nullptr);
	Memory gpu;
	constexpr uint64_t kVa = 1ull << 32;
	for (uint32_t i = 0; i < bo->mem->count; ++i)
		gpu.map(kVa + uint64_t(i) * Memory::kPage, dram[bo->mem->pages[i]].data(),
			Memory::kPage, bo->readOnly, true);
	EXPECT_EQ(reply.bytes, uint64_t(bo->mem->count) * Memory::kPage);
	for (uint32_t p = 0; p < reply.plane_count; ++p) {
		const uint64_t at = kVa + reply.planes[p].offset;
		ASSERT_NE(gpu.at(at), nullptr);
		EXPECT_EQ(*gpu.at(at), uint8_t(0xa0 + reply.planes[p].offset / Memory::kPage));
		EXPECT_EQ(gpu.at(at, true), nullptr);
	}
}

TEST_F(NativeImport, PitchAuthorityAlsoCreatesAnImmutableGenerationGuardedProxy)
{
	answer.native = &logical;
	answer.payload = {};
	for (auto &plane : answer.layout.planes) {
		plane.layout = SWITCH_GPU_BACKING_PITCH;
		plane.block_height_log2 = 0;
	}
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	EXPECT_NE(reply.handle, 0u);
	EXPECT_EQ(reply.kind, 0u);
	EXPECT_EQ(client.findBo(reply.handle)->nativeView->representation.expected, 3u);
	representation = 4;
	EXPECT_NE(*client.findBo(reply.handle)->nativeView->representation.generation,
		  client.findBo(reply.handle)->nativeView->representation.expected);
}

TEST_F(NativeImport, UnsupportedSurfaceReturnsNoProxyWithoutCreatingMemory)
{
	acquireError = -2;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	EXPECT_EQ(reply.handle, 0u);
	EXPECT_EQ(constructed, 0u);
	EXPECT_EQ(released, 1u);
}

TEST_F(NativeImport, InvalidPlaneExtentFailsBeforeGpuMemoryConstruction)
{
	answer.layout.planes[1].bytes = UINT64_MAX;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnBadArgument);
	EXPECT_EQ(constructed, 0u);
	EXPECT_EQ(reply.handle, 0u);
	ASSERT_EQ(refusals.size(), 1u);
	EXPECT_NE(refusals[0].find("native-view plane-extent: "), std::string::npos) << refusals[0];
	EXPECT_NE(refusals[0].find("do not fit"), std::string::npos) << refusals[0];
	EXPECT_NE(refusals[0].find("(plane 1 "), std::string::npos) << refusals[0];
}

// HW Test gpu.gm20b.ycbcr_texel_buffer_wide: a '420v' 20000 wide (pitch planes 20000x2 and 10000x1) read through a
// texel buffer's 16384x2 rows. The planes are memory; the textures over them are shaped by the client.
TEST_F(NativeImport, PitchPlanesWiderThanATextureAreAView)
{
	answer.native = &logical;
	answer.payload = {};
	answer.layout.planes[0] = {0, 40960, 20000, 2, 20480, 1, SWITCH_GPU_BACKING_PITCH, 0};
	answer.layout.planes[1] = {40960, 20480, 10000, 1, 20480, 2, SWITCH_GPU_BACKING_PITCH, 0};
	answer.crop_width = 20000;
	answer.crop_height = 2;
	uint64_t wide[15];
	for (uint32_t i = 0; i < 15; ++i)
		wide[i] = 0x200000 + uint64_t(i) * 0x1000;
	allocation.pages = wide;
	allocation.page_count = 15;
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	ASSERT_NE(reply.handle, 0u);
	EXPECT_EQ(reply.kind, 0u);
	EXPECT_EQ(reply.planes[0].width, 20000u);
	EXPECT_EQ(reply.planes[1].width, 10000u);
	EXPECT_TRUE(refusals.empty());
}

TEST_F(NativeImport, AllocationFailureDropsBothRetainedBackingReferences)
{
	memError = -12;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnNoMemory);
	EXPECT_EQ(logical.refs, 0u);
	EXPECT_EQ(physical.refs, 0u);
	EXPECT_EQ(surface.refs, 1u);
}

TEST_F(NativeImport, ClosingDuringUnlockedAcquisitionUnwindsView)
{
	client.submitGate_.closing = true;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), -19);
	EXPECT_EQ(released, 1u);
	EXPECT_EQ(constructed, 0u);
	ASSERT_EQ(refusals.size(), 1u);
	EXPECT_NE(refusals[0].find("native-view gate-closed: "), std::string::npos) << refusals[0];
}

TEST_F(NativeImport, SourceMustBeAnAttestedOrdinarySurfaceBo)
{
	source.surface = nullptr;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnBadArgument);
	ASSERT_EQ(refusals.size(), 1u);
	EXPECT_NE(refusals[0].find("native-view source: "), std::string::npos) << refusals[0];
	EXPECT_NE(refusals[0].find("not an IOSurface's"), std::string::npos) << refusals[0];
	source.surface = &surface;
	source.nativeView = &answer;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnBadArgument);
	ASSERT_EQ(refusals.size(), 2u);
	EXPECT_NE(refusals[1].find("a native view itself"), std::string::npos) << refusals[1];
	EXPECT_EQ(acquired, 0u);
}

TEST_F(NativeImport, ProducerFailureIsAbortedContentRatherThanDeadChannel)
{
	acquireError = -5;
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnAborted);
	EXPECT_EQ(constructed, 0u);
}

TEST_F(NativeImport, ExistingCanonicalBackingMemoryIsBorrowedWithoutAliasing)
{
	tgm_mem canonical{pages, 3};
	existingBackingMem = &canonical;
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	const auto *bo = client.findBo(reply.handle);
	ASSERT_NE(bo, nullptr);
	EXPECT_TRUE(bo->backingMemory);
	EXPECT_EQ(bo->mem, &canonical);
	EXPECT_EQ(constructed, 0u);
	EXPECT_EQ(directPaLatches, 0u);
}

TEST_F(NativeImport, DirectFallbackLatchesDirectPaBeforeAllocatingMem)
{
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	EXPECT_EQ(directPaLatches, 1u);
	EXPECT_EQ(constructed, 1u);
}

TEST_F(NativeImport, WritableFlagAcquiresWritableNativeBoAndRejectsUnknownFlags)
{
	request.flags = TegraGM20BNativeSurfaceWrite;
	ASSERT_EQ(client.nativeSurfaceView(&request, &reply), 0);
	ASSERT_NE(reply.handle, 0u);
	const auto *bo = client.findBo(reply.handle);
	ASSERT_NE(bo, nullptr);
	EXPECT_FALSE(bo->readOnly);
	EXPECT_EQ(acquiredWrites, 1u);
	EXPECT_EQ(acquired, 0u);

	request.flags = TegraGM20BNativeSurfaceWrite | (1u << 1);
	EXPECT_EQ(client.nativeSurfaceView(&request, &reply), kIOReturnBadArgument);
	ASSERT_EQ(refusals.size(), 1u);
	EXPECT_NE(refusals[0].find("native-view request: "), std::string::npos) << refusals[0];
}
