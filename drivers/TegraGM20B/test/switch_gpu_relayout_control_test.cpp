// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "switch_gpu_relayout_control_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.cpp"
#include "gm20b_ce_model.h"

struct tgm_vm {};

struct tgm_mem {};

struct tgm_chan {};

struct switch_gpu_mapping {};

namespace
{
std::vector<uint8_t> pushBytes(4096);
Memory memory;
tgm_vm space;
tgm_mem pushMemory, fenceMemory;
volatile uint32_t fence;
unsigned madeMemory, vmFailures, poolFrees;
bool pushVolatile;
switch_gpu_mapping pushWindow;

// A move that faults past its checkpoint: its channel errors with dirty lines of the surface left in the L2
constexpr uint64_t kSurfacePa = 0x10000000, kRowsPa = 0x20000000, kSurfaceGpuVa = 0x40000000;
tgm_chan channels[4];
unsigned channelsMade, channelsDeleted, flushJobs, cpuReads, cpuReadsBehindL2;
bool channelsOpen, faultMoves, beforeCheckpoint;
tgm_chan *faulted, *flushedOn;
uint32_t flushVisibility;
std::vector<uint8_t> surfaceBytes(16 * 4096), rowsBytes(16 * 4096);
SwitchGpuRelayoutMapping surfaceMap{}, rowsMap{};
}

extern "C" void *tgm_pool_alloc(uint64_t bytes, uint64_t, uint64_t *pa)
{
	EXPECT_EQ(bytes, pushBytes.size());
	*pa = 0x100000;
	return pushBytes.data();
}

extern "C" int tgm_fence_kernel(unsigned, volatile uint32_t **cpu, uint64_t *page, uint32_t *offset)
{
	*cpu = &fence;
	*page = 0x200000;
	*offset = 128;
	return 0;
}

extern "C" int tgm_vm_new(tgm_vm **out)
{
	if (vmFailures) {
		--vmFailures;
		return -12;
	}
	*out = &space;
	return 0;
}

extern "C" void tgm_vm_del(tgm_vm **out)
{
	*out = nullptr;
}

extern "C" void tgm_mem_del(tgm_mem **out)
{
	*out = nullptr;
}

extern "C" void tgm_pool_free(void *, uint64_t, uint64_t)
{
	++poolFrees;
}

extern "C" void tgm_vm_claim(tgm_vm *, const void *)
{
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **out)
{
	*out = madeMemory++ ? &fenceMemory : &pushMemory;
	return 0;
}

extern "C" int tgm_vm_map_ordered(tgm_vm *, uint64_t va, uint64_t bytes, tgm_mem *mem,
				  uint64_t offset, uint8_t kind, int readOnly, uint8_t *)
{
	EXPECT_EQ(mem, &pushMemory);
	EXPECT_EQ(bytes, pushBytes.size());
	EXPECT_EQ(offset, 0u);
	EXPECT_EQ(kind, 0u);
	memory.map(va, pushBytes, readOnly != 0);
	return 0;
}

extern "C" int switch_gpu_vm_map_volatile(tgm_vm *vm, uint64_t va, uint64_t bytes, tgm_mem *mem,
					  uint64_t offset, uint8_t kind, int readOnly, int vol,
					  switch_gpu_mapping **out)
{
	pushVolatile = vol != 0;
	*out = &pushWindow;
	return tgm_vm_map_ordered(vm, va, bytes, mem, offset, kind, readOnly, nullptr);
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **mapping)
{
	*mapping = nullptr;
}

extern "C" int tgm_vm_kmap(tgm_vm *, tgm_mem *, uint64_t, uint64_t *va)
{
	*va = 0x200000;
	return 0;
}

extern "C" uint32_t tgm_chan_error(tgm_chan *chan)
{
	if (chan && chan == faulted)
		return 1;
	return 0;
}

extern "C" void tgm_chan_del(tgm_chan **chan)
{
	*chan = nullptr;
	++channelsDeleted;
}

extern "C" void tgm_chan_on_fence(tgm_chan *, void (*)(void *), void *)
{
	ADD_FAILURE() << "the relayout channel has no fence callback";
}

extern "C" int tgm_chan_new_copy(tgm_vm *, const void *, volatile uint32_t *, uint64_t,
				 const char *, int, tgm_chan **out)
{
	if (channelsOpen) {
		*out = &channels[channelsMade++ % 4];
		return 0;
	}
	return -12;
}

extern "C" uint32_t tgm_chan_uid(tgm_chan *)
{
	return 1;
}

extern "C" int tgm_chan_exec(tgm_chan *chan, const tgm_push *, uint32_t, uint32_t *seqno,
			     const tgm_prof *)
{
	if (faultMoves) {
		const std::vector<uint8_t> partial(512, 0xc5);
		EXPECT_TRUE(memory.store(kSurfaceGpuVa + 4096, partial.data(), partial.size()));
		gRelayout.space.words[kCheckpointWord] = beforeCheckpoint ? 0 : 1;
		faulted = chan;
		*seqno = 1;
		return 0;
	}
	return -12;
}

extern "C" void tgm_copy_wait(uint32_t, uint32_t, const tgm_relayout_writer *, uint32_t)
{
}

extern "C" int tgm_writer_done(uint32_t, uint32_t)
{
	return 1;
}

extern "C" int tgm_writer_fence_va(tgm_vm *, uint32_t, uint64_t *)
{
	return -22;
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t)
{
}

extern "C" void tgm_cpu_clean_pages(const uint64_t *, uint32_t)
{
}

extern "C" int switch_gpu_chan_exec_visibility(tgm_chan *chan, const tgm_push *, uint32_t count,
					       const switch_gpu_backing_token *, uint32_t,
					       uint32_t *seqno, const tgm_prof *,
					       uint32_t visibility)
{
	EXPECT_EQ(count, 0u);
	++flushJobs;
	flushedOn = chan;
	flushVisibility = visibility;
	if (!(visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) ||
	    (visibility & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY))
		memory.flushL2();
	*seqno = 2;
	return 0;
}

extern "C" void switch_gpu_chan_submit_enter(tgm_chan *)
{
}

extern "C" void switch_gpu_chan_submit_leave(void)
{
}

extern "C" int switch_gpu_chan_wait_submission_room(tgm_chan *, const tgm_push *, uint32_t,
						    uint32_t, const tgm_prof *)
{
	return 0;
}

extern "C" vm_offset_t phystokv(uint64_t pa)
{
	if (faultMoves) {
		++cpuReads;
		cpuReadsBehindL2 += !memory.dirty.empty();
		auto &bytes = pa >= kRowsPa ? rowsBytes : surfaceBytes;
		return vm_offset_t(bytes.data() + (pa - (pa >= kRowsPa ? kRowsPa : kSurfacePa)));
	}
	return 0;
}

extern "C" void switch_refusal(const char *, const char *, const char *, ...)
{
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *)
{
}

switch_gpu_backing *switch_gpu_pin_memory(const uint64_t *, uint32_t, uint32_t,
					  IOMemoryDescriptor *, bool)
{
	return nullptr;
}

int switch_gpu_relayout_mapping(tgm_vm *, SwitchGpuRelayoutCache *, const uint64_t *, uint32_t,
				bool surface, SwitchGpuRelayoutMapping **out)
{
	if (faultMoves) {
		*out = surface ? &surfaceMap : &rowsMap;
		return 0;
	}
	return -12;
}

void switch_gpu_relayout_mapping_put(SwitchGpuRelayoutMapping *)
{
}

void switch_gpu_relayout_cache_clear(tgm_vm *, SwitchGpuRelayoutCache *)
{
}

switch_scratch *switch_scratch_get(uint64_t, task_t)
{
	return nullptr;
}

void switch_scratch_put(switch_scratch *)
{
}

switch_scratch *switch_scratch_reclaim(switch_scratch *, uint64_t, task_t, bool *intact)
{
	*intact = false;
	return nullptr;
}

void switch_scratch_discard(switch_scratch *)
{
}

void switch_scratch_stats(uint64_t *, uint64_t *)
{
}

namespace
{
class RelayoutControl : public testing::Test
{
	void SetUp() override
	{
		gRelayout = {};
		memory = {};
		madeMemory = 0;
		std::fill(pushBytes.begin(), pushBytes.end(), 0);
		ASSERT_TRUE(gRelayout.space.openSpace(kShape, &gRelayout));
	}
};

TEST_F(RelayoutControl, FailedOpenIsUndoneAndTheNextMoveOpensAgain)
{
	gRelayout = {};
	madeMemory = 0;
	vmFailures = 1;
	const unsigned frees = poolFrees, logged = relayoutLogs;
	EXPECT_EQ(channel(), nullptr);
	EXPECT_EQ(poolFrees, frees + 1);
	EXPECT_EQ(gRelayout.space.words, nullptr);
	EXPECT_EQ(relayoutLogs, logged + 1);
	EXPECT_EQ(channel(), nullptr);
	EXPECT_EQ(gRelayout.space.vm, &space);
	EXPECT_EQ(relayoutLogs, logged + 1);
}

TEST_F(RelayoutControl, ActualMappingAllowsGpuCheckpointWithoutChangingCommands)
{
	EXPECT_TRUE(pushVolatile);
	const uint32_t n = tgm_relayout_checkpoint(gRelayout.space.words,
						   kPushVa + uint64_t(kCheckpointWord) * 4);
	const std::vector<uint8_t> commands(pushBytes.begin(), pushBytes.begin() + n * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	ASSERT_FALSE(ce.fault);
	EXPECT_EQ(gRelayout.space.words[kCheckpointWord], 1u);
	EXPECT_TRUE(std::equal(commands.begin(), commands.end(), pushBytes.begin()));
}

TEST_F(RelayoutControl, ReadOnlyMappingReproducesHostSemaphoreWriteFault)
{
	memory.map(kPushVa, pushBytes, true);
	const uint32_t n = tgm_relayout_checkpoint(gRelayout.space.words,
						   kPushVa + uint64_t(kCheckpointWord) * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	EXPECT_TRUE(ce.fault);
	EXPECT_EQ(gRelayout.space.words[kCheckpointWord], 0u);
}

TEST_F(RelayoutControl, PushTheDriverNeverMappedFaultsBeforeItsFirstMethod)
{
	memory.unmap(kPushVa, pushBytes.size());
	const uint32_t n = tgm_relayout_checkpoint(gRelayout.space.words,
						   kPushVa + uint64_t(kCheckpointWord) * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	EXPECT_TRUE(ce.fault);
	EXPECT_TRUE(ce.m.empty());
}
}  // namespace

namespace
{
TEST_F(RelayoutControl, MovesTheCpuFinishesAreCountedWithTheStats)
{
	uint64_t moves = 0, bytes = 0;
	tgm_relayout_cpu_stats(&moves, &bytes);
	const uint64_t pas[2] = {0x1000, 0x2000};
	const tgm_relayout_pages surface = {pas, 2};
	const tgm_layout layout = {};
	tgm_relayout_finish_cpu(&surface, &surface, &layout, 0, TGM_RELAYOUT_TO_ROWS);
	tgm_relayout_cpu_stats(&moves, &bytes);
	EXPECT_EQ(moves, TGM_STATS ? 1u : 0u);
	EXPECT_EQ(bytes, TGM_STATS ? 2u * 4096u : 0u);
}
}  // namespace

namespace
{
// A copy that faults past its checkpoint left its partial writes dirty in the L2: a new channel's empty External job
// flushes them before the CPU's finish touches the surface in DRAM.
TEST_F(RelayoutControl, AFaultedMoveFlushesTheL2BeforeTheCpuFinishesIt)
{
	channelsOpen = faultMoves = true;
	memory.map(kSurfaceGpuVa, surfaceBytes, false, true);
	tgm_layout l = {};
	ASSERT_TRUE(tgm_layout_tiled(64, 64, 4, &l));
	std::vector<uint64_t> surfacePas, rowsPas;
	for (uint64_t at = 0; at < surfaceBytes.size(); at += 4096) {
		surfacePas.push_back(kSurfacePa + at);
		rowsPas.push_back(kRowsPa + at);
	}
	const tgm_relayout_pages surface = {surfacePas.data(), uint32_t(surfacePas.size())};
	const tgm_relayout_pages rows = {rowsPas.data(), uint32_t(rowsPas.size())};
	ASSERT_LE(l.bytes, surfaceBytes.size());
	EXPECT_EQ(move(surface, rows, l, 256, TGM_RELAYOUT_TILE_IN_PLACE, nullptr, 0), 0);
	EXPECT_EQ(flushJobs, 1u);
	EXPECT_NE(flushedOn, nullptr);
	EXPECT_NE(flushedOn, faulted);
	EXPECT_TRUE(flushVisibility & SWITCH_GPU_SUBMIT_FORCE_FENCE);
	EXPECT_FALSE(flushVisibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT);
	EXPECT_GT(cpuReads, 0u);
	EXPECT_EQ(cpuReadsBehindL2, 0u);
	channelsOpen = faultMoves = false;
	faulted = flushedOn = nullptr;
}
}  // namespace

namespace
{
// A copy that faults before its checkpoint returns the failure, and its caller moves the texels on the CPU: the L2 is
// flushed before that too.
TEST_F(RelayoutControl, AFaultBeforeTheCheckpointFlushesTheL2ForTheCallersCpuMove)
{
	channelsOpen = faultMoves = beforeCheckpoint = true;
	flushJobs = cpuReads = 0;
	memory.map(kSurfaceGpuVa, surfaceBytes, false, true);
	tgm_layout l = {};
	ASSERT_TRUE(tgm_layout_tiled(64, 64, 4, &l));
	std::vector<uint64_t> surfacePas, rowsPas;
	for (uint64_t at = 0; at < surfaceBytes.size(); at += 4096) {
		surfacePas.push_back(kSurfacePa + at);
		rowsPas.push_back(kRowsPa + at);
	}
	const tgm_relayout_pages surface = {surfacePas.data(), uint32_t(surfacePas.size())};
	const tgm_relayout_pages rows = {rowsPas.data(), uint32_t(rowsPas.size())};
	EXPECT_EQ(move(surface, rows, l, 256, TGM_RELAYOUT_TO_ROWS, nullptr, 0), -5);
	EXPECT_EQ(flushJobs, 1u);
	EXPECT_NE(flushedOn, faulted);
	EXPECT_TRUE(memory.dirty.empty());
	EXPECT_EQ(cpuReads, 0u);
	channelsOpen = faultMoves = beforeCheckpoint = false;
	faulted = flushedOn = nullptr;
}
}  // namespace
