// SPDX-License-Identifier: MIT
// MapSparse's mapping (SwitchGpuVmSparse.h) and the installs it shares with prepared maps (SwitchGpuVmMap.h), over the
// stand-in page tables of switch_gpu_vm_map_stubs.h.
#include "switch_gpu_vm_map_stubs.h"
#include "../SwitchGpuVmSparse.h"

namespace
{
struct Run {
	u64 va, bytes;
	u8 shift;
};

// [0x40011000, 0x40063000): 4 KiB pages up to the first 128 KiB boundary, the two whole big pages, the 4 KiB tail
constexpr u64 kVa = 0x40011000, kRange = 0x52000;
const std::vector<Run> kRuns = {
	{0x40011000, 0xf000, 12}, {0x40020000, 0x40000, 17}, {0x40060000, 0x3000, 12}};

void expectRun(const Call &call, const Run &run)
{
	EXPECT_EQ(call.va, run.va);
	EXPECT_EQ(call.bytes, run.bytes);
	EXPECT_EQ(call.shift, run.shift);
}

// From calls[first]: each run's page tables taken, then its sparse PTEs written, GPU-ordered, with no memory
void expectSparsePtes(size_t first)
{
	ASSERT_EQ(calls.size(), first + 2 * kRuns.size());
	for (size_t i = 0; i < kRuns.size(); ++i) {
		const Call &get = calls[first + 2 * i], &ptes = calls[first + 2 * i + 1];
		EXPECT_EQ(get.operation, 'g');
		EXPECT_EQ(ptes.operation, 'u');
		EXPECT_TRUE(ptes.sparse);
		for (const Call *call : {&get, &ptes}) {
			expectRun(*call, kRuns[i]);
			EXPECT_TRUE(call->deferred);
		}
	}
}
}  // namespace

class VmSparse : public testing::Test
{
protected:
	tgm_vm a{};

	void SetUp() override
	{
		for (unsigned i = 0; i < 256; ++i)
			tgm_gpu.mmu.kind[i] = u8(i);
		a.owner = &a;
		a.vmm.id = 1;
		tgm_tlb_space_init(&a.tlb);
		calls.clear();
		failGet = failMap = -1;
		getCount = mapCount = invalidations = 0;
		flushes = 0;
	}

	void TearDown() override
	{
		EXPECT_EQ(gateInside, 0u);
		EXPECT_FALSE(ordered.active(owner()));
	}
};

TEST_F(VmSparse, EveryPageGetsTheSparsePteOverPageTablesItTakes)
{
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_sparse(&a, kVa, kRange, 1, &mapping), 0);
	expectSparsePtes(0);
	EXPECT_EQ(a.tlb.gen, 2u);
	EXPECT_EQ(flushes, 0u);
	EXPECT_EQ(invalidations, 0);
	EXPECT_TRUE(switch_gpu_mapping_installed(mapping));
	EXPECT_FALSE(switch_gpu_mapping_pending(mapping));
	switch_gpu_vm_unmap(&a, &mapping);
}

TEST_F(VmSparse, ARangeOutsideTheClientsWholePagesIsRefusedUntouched)
{
	tgm_vm unowned{};

	const struct {
		tgm_vm *vm;
		u64 va, range;
	} refused[] = {{&a, kVa + 0x800, 0x1000},
		       {&a, kVa, 0},
		       {&a, TGM_USER_VA_START - 0x1000, 0x2000},
		       {&a, TGM_KERNEL_VA - 0x1000, 0x2000},
		       {&unowned, kVa, 0x1000}};

	for (const auto &each : refused) {
		switch_gpu_mapping *mapping = nullptr;
		EXPECT_EQ(switch_gpu_vm_sparse(each.vm, each.va, each.range, 1, &mapping), -EINVAL);
		EXPECT_EQ(mapping, nullptr);
	}
	EXPECT_TRUE(calls.empty());
}

// Behind queued work: nothing is written until the install, which needs no memory.
TEST_F(VmSparse, APendingSparseRangeWritesNothingUntilInstalled)
{
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_sparse(&a, kVa, kRange, 0, &mapping), 0);
	EXPECT_TRUE(calls.empty());
	EXPECT_TRUE(switch_gpu_mapping_pending(mapping));
	EXPECT_FALSE(switch_gpu_mapping_installed(mapping));
	ASSERT_EQ(switch_gpu_vm_install(&a, mapping, nullptr, 0, 0, 0), 0);
	expectSparsePtes(0);
	switch_gpu_vm_unmap(&a, &mapping);
}

// An Unmap, or the cut of a Map over it, removes sparse PTEs as any: after an L2 flush, synchronously, each run's page
// tables given back.
TEST_F(VmSparse, ItsRemovalIsAnyMappingsRemoval)
{
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_sparse(&a, kVa, kRange, 1, &mapping), 0);
	calls.clear();
	switch_gpu_vm_unmap(&a, &mapping);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_EQ(flushes, 1u);
	ASSERT_EQ(calls.size(), 2 * kRuns.size());
	for (size_t i = 0; i < kRuns.size(); ++i) {
		EXPECT_EQ(calls[2 * i].operation, 'u');
		EXPECT_FALSE(calls[2 * i].sparse);
		EXPECT_EQ(calls[2 * i + 1].operation, 'p');
		for (size_t c = 2 * i; c < 2 * i + 2; ++c) {
			expectRun(calls[c], kRuns[i]);
			EXPECT_FALSE(calls[c].deferred);
		}
	}
}

// A Map over 4 KiB of a sparse big page: that page keeps its sparse PTE, whole, until the cut retires; what stays
// outside the Map comes back pending as 4 KiB entries, written sparse again once that PTE is gone.
TEST_F(VmSparse, ACutBigPageStaysSparseAndItsRestIsWrittenSparseAgain)
{
	switch_gpu_mapping *mapping = nullptr, *below = nullptr, *taken = nullptr, *above = nullptr;
	ASSERT_EQ(switch_gpu_vm_sparse(&a, 0x40000000, 0x40000, 1, &mapping), 0);
	ASSERT_EQ(mapping->count, 1u);
	calls.clear();
	ASSERT_EQ(switch_gpu_vm_split(&a, &mapping, nullptr, 0, 0, 0, 0x40008000, 0x1000, &below,
				      &taken, &above),
		  0);
	EXPECT_TRUE(calls.empty());
	ASSERT_EQ(taken->count, 1u);
	EXPECT_EQ(taken->entries[0].shift, 17);
	EXPECT_EQ(taken->entries[0].bytes, 0x40000u);
	EXPECT_FALSE(taken->entries[0].pending);
	EXPECT_TRUE(below->sparse && taken->sparse && above->sparse);
	EXPECT_TRUE(switch_gpu_mapping_pending(below));
	EXPECT_TRUE(switch_gpu_mapping_pending(above));
	switch_gpu_vm_unmap(&a, &taken);
	calls.clear();
	ASSERT_EQ(switch_gpu_vm_install(&a, below, nullptr, 0, 0, 0), 0);
	ASSERT_EQ(switch_gpu_vm_install(&a, above, nullptr, 0, 0, 0), 0);
	const std::vector<::Run> rest = {{0x40000000, 0x8000, 12}, {0x40009000, 0x37000, 12}};
	ASSERT_EQ(calls.size(), 4u);
	for (size_t i = 0; i < rest.size(); ++i) {
		EXPECT_EQ(calls[2 * i].operation, 'g');
		EXPECT_EQ(calls[2 * i + 1].operation, 'u');
		EXPECT_TRUE(calls[2 * i + 1].sparse);
		expectRun(calls[2 * i + 1], rest[i]);
	}
	switch_gpu_vm_unmap(&a, &below);
	switch_gpu_vm_unmap(&a, &above);
}

// One run without page tables refuses the range: the runs written are removed synchronously, and the page tables the
// failed get dropped inside the GPU-ordered scope (no TLB invalidate there) are invalidated before the lock goes.
TEST_F(VmSparse, ARangeThatCannotGetItsPageTablesIsRefusedWhole)
{
	failGet = 1;
	switch_gpu_mapping *mapping = nullptr;
	EXPECT_EQ(switch_gpu_vm_sparse(&a, kVa, kRange, 1, &mapping), -ENOMEM);
	EXPECT_EQ(mapping, nullptr);
	EXPECT_EQ(invalidations, 1);
	const std::vector<char> operations = {'g', 'u', 'g', 'g', 'u', 'u', 'p', 'u', 'p'};
	ASSERT_EQ(calls.size(), operations.size());
	for (size_t i = 0; i < calls.size(); ++i) {
		EXPECT_EQ(calls[i].operation, operations[i]);
		EXPECT_EQ(calls[i].deferred, i < 5);
	}
	expectRun(calls[5], kRuns[0]);
	expectRun(calls[7], kRuns[2]);
}

// The same for a Map prepared behind queued work.
TEST_F(VmSparse, APreparedMapThatCannotGetItsPageTablesInvalidatesThem)
{
	std::vector<u64> pages(8);
	for (unsigned i = 0; i < pages.size(); ++i)
		pages[i] = 0x80000000 + i * PAGE_SIZE;
	tgm_mem memory = {{12, pages.size() * PAGE_SIZE},
			  pages.size() * PAGE_SIZE,
			  12,
			  pages.data(),
			  u32(pages.size()),
			  nullptr,
			  true};
	switch_gpu_mapping *mapping = nullptr;
	ASSERT_EQ(switch_gpu_vm_prepare(&a, 0x40000000, memory.bytes, &memory, 0, 0, &mapping), 0);
	failGet = 0;
	EXPECT_EQ(switch_gpu_vm_install(&a, mapping, &memory, 0, 0, 0), -ENOMEM);
	EXPECT_EQ(invalidations, 1);
	EXPECT_EQ(mapping->count, 0u);
	switch_gpu_vm_unmap(&a, &mapping);
}
