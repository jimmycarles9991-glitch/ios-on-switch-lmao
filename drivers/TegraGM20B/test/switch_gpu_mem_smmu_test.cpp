// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuSmmu.h"
#include "drivers/TegraGM20B/TegraGM20BComp.h"

namespace
{
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using dma_addr_t = uint64_t;

constexpr u8 PAGE_SHIFT = 12;
constexpr int GFP_KERNEL = 0;
constexpr int NVIF_CLASS_MEM_GF100 = 1;

struct nvif_mem_ram_v0 {
	u8 version;
	dma_addr_t *dma;
	void *sgl;
};

struct nvif_mem {
	u8 shift;
	u64 bytes;
	dma_addr_t *dma;
};

struct tgm_vm {};

struct tgm_mem {
	nvif_mem mem;
	u64 bytes;
	u8 page;
	const u64 *pages;
	u32 npages;
	u64 smmu_iova;
	u64 *smmu_pages;
};

struct FakeLtc {
	u32 num_tags = 1024;
};

struct FakeDevice {
	FakeLtc ltcStorage;
	FakeLtc *ltc = &ltcStorage;
};

struct FakeGpu {
	int stage = 3;
	FakeDevice deviceStorage;
	FakeDevice *device = &deviceStorage;
	int mmu = 0;
	int mem_type = 0;
	int lock = 0;
} tgm_gpu;

unsigned gSmmuMaps, gSmmuUnmaps, gCtorCalls, gDtorCalls;
uint64_t gNextIova = 0x10000000ULL;
int gCtorError;
bool gSmmuFail;
u8 gLastCtorPage;
const u64 *gLastCtorDma;

struct AnyPtr {
	void *p;

	template <typename T> operator T *() const
	{
		return static_cast<T *>(p);
	}

	explicit operator bool() const
	{
		return p != nullptr;
	}
};

AnyPtr kzalloc(size_t size, int)
{
	return {calloc(1, size)};
}

AnyPtr kmalloc_array(size_t n, size_t size, int)
{
	return {calloc(n, size)};
}

void kfree(void *ptr)
{
	free(ptr);
}

int stats_on()
{
	return 0;
}

u64 nvxnu_nanotime()
{
	return 0;
}

void tgm_gpu_lock()
{
}

void mutex_unlock(int *)
{
}

int nvif_mem_ctor_type(int *, const char *, int, int, u8 page, u64 bytes, nvif_mem_ram_v0 *args,
		       size_t, nvif_mem *out)
{
	++gCtorCalls;
	gLastCtorPage = page;
	gLastCtorDma = args->dma;
	if (gCtorError)
		return gCtorError;
	*out = {page, bytes, args->dma};
	return 0;
}

void nvif_mem_dtor(nvif_mem *)
{
	++gDtorCalls;
}

void mem_prepare_big_views(tgm_mem *)
{
}

void mem_big_views_drop(tgm_mem *)
{
}

extern "C" uint64_t switch_gpu_smmu_map(const uint64_t *, uint32_t npages)
{
	++gSmmuMaps;
	if (gSmmuFail)
		return 0;
	const uint64_t iova = gNextIova;
	gNextIova += (uint64_t(npages) << 12) + 0x10000ULL;
	return iova;
}

extern "C" void switch_gpu_smmu_unmap(uint64_t, uint32_t)
{
	++gSmmuUnmaps;
}

#include "drivers/TegraGM20B/test/switch_gpu_mem_smmu_source.h"

class GpuMemSmmuTest : public testing::Test
{
protected:
	tgm_vm vm;

	void SetUp() override
	{
		tgm_gpu = {};
		tgm_gpu.device = &tgm_gpu.deviceStorage;
		tgm_gpu.deviceStorage.ltc = &tgm_gpu.deviceStorage.ltcStorage;
		gSmmuMaps = gSmmuUnmaps = gCtorCalls = gDtorCalls = 0;
		gNextIova = 0x10000000ULL;
		gCtorError = 0;
		gSmmuFail = false;
		gLastCtorPage = 0;
		gLastCtorDma = nullptr;
	}

	static std::vector<uint64_t> scattered(uint32_t npages)
	{
		std::vector<uint64_t> pas(npages);
		for (uint32_t i = 0; i < npages; ++i)
			pas[i] = 0x80001000ULL + uint64_t(i) * 0x2000ULL;
		return pas;
	}

	static std::vector<uint64_t> contiguousAligned(uint64_t base, uint32_t npages)
	{
		std::vector<uint64_t> pas(npages);
		for (uint32_t i = 0; i < npages; ++i)
			pas[i] = base + (uint64_t(i) << 12);
		return pas;
	}
};

TEST_F(GpuMemSmmuTest, NonExclusiveAndDirectExclusiveNeverAllocateSmmuIovas)
{
	const auto pages = scattered(32);
	tgm_mem *a = nullptr, *b = nullptr, *c = nullptr;
	uint64_t waited = 0;

	ASSERT_EQ(tgm_mem_new(&vm, pages.data(), 32, &a), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(a->smmu_iova, 0u);
	EXPECT_EQ(a->pages, pages.data());
	EXPECT_FALSE(tgm_mem_can_compress(a));
	EXPECT_FALSE(tgm_mem_is_smmu(a));

	ASSERT_EQ(tgm_mem_new_waited(&vm, pages.data(), 32, &b, &waited), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(b->smmu_iova, 0u);
	EXPECT_EQ(b->pages, pages.data());

	ASSERT_EQ(tgm_mem_new_exclusive(&vm, pages.data(), 32, &c), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(c->smmu_iova, 0u);
	EXPECT_FALSE(tgm_mem_can_compress(c));

	tgm_mem_del(&a);
	tgm_mem_del(&b);
	tgm_mem_del(&c);
	EXPECT_EQ(gSmmuUnmaps, 0u);
}

TEST_F(GpuMemSmmuTest, DirectExclusiveCompressesPhysicallyAlignedRunsWithoutSmmu)
{
	const auto aligned = contiguousAligned(0xe4000000ULL, 32);
	tgm_mem *mem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive(&vm, aligned.data(), 32, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(mem->smmu_iova, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(mem));
	EXPECT_EQ(mem->page, 16u);
	tgm_mem_del(&mem);
	EXPECT_EQ(gSmmuUnmaps, 0u);
}

TEST_F(GpuMemSmmuTest, ExclusiveSmmuCompactsOnly64KMultipleScatteredAndCleansUp)
{
	const auto panel = scattered(900);
	tgm_mem *panelMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, panel.data(), 900, &panelMem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_can_compress(panelMem));
	tgm_mem_del(&panelMem);

	const auto aligned = contiguousAligned(0xe4000000ULL, 32);
	tgm_mem *directMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, aligned.data(), 32, &directMem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(directMem));
	EXPECT_FALSE(tgm_mem_is_smmu(directMem));
	tgm_mem_del(&directMem);

	const auto pages = scattered(32);
	tgm_mem *compMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 32, &compMem), 0);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_NE(compMem->smmu_iova, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(compMem));
	EXPECT_TRUE(tgm_mem_is_smmu(compMem));
	EXPECT_EQ(compMem->page, 16u);
	EXPECT_TRUE(switch_gpu_smmu_is_iova(compMem->pages[0]));
	EXPECT_EQ(compMem->pages[16] - compMem->pages[0], 0x10000ULL);
	tgm_mem_del(&compMem);
	EXPECT_EQ(gSmmuUnmaps, 1u);
}

TEST_F(GpuMemSmmuTest, ExclusiveSmmuUnmapsOnCtorFailureAndSkipsWhenCompDisabled)
{
	const auto pages = scattered(16);
	tgm_mem *mem = nullptr;

	gCtorError = -ENOMEM;
	EXPECT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 16, &mem), -ENOMEM);
	EXPECT_EQ(mem, nullptr);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_EQ(gSmmuUnmaps, 1u);

	gCtorError = 0;
	tgm_gpu.deviceStorage.ltcStorage.num_tags = 0;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 16, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_FALSE(tgm_mem_can_compress(mem));
	tgm_mem_del(&mem);
}
}  // namespace
