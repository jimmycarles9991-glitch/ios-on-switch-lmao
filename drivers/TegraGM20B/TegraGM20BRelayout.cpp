// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <kern/task.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BFences.h"
#include "SwitchGpuKqueue.h"
#include "SwitchGpuPin.h"
#include "SwitchGpuRelayoutCache.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BRelayout.h"
#include "TegraGM20BTiled.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchScratch/SwitchScratch.h"

namespace
{
constexpr uint64_t kPage = 4096;
constexpr uint64_t kPushVa = 1ULL << 20;
constexpr uint32_t kCheckpointWord = 1023;
// The push page is GPU-writable: its last word receives the GPU's recovery checkpoint.
constexpr SwitchGpuKqueueShape kShape = {"relayout", TGM_FENCE_KERNEL_RELAYOUT, 1, kPushVa, 0, 1};

struct Relayout {
	IOLock *lock;
	SwitchGpuKqueue space;
	bool unavailable;  // the last open failed (logged once until one succeeds)
	SwitchGpuRelayoutCache mappings;
} gRelayout;

tgm_chan *channel()
{
	Relayout &r = gRelayout;
	if (r.space.channel && tgm_chan_error(r.space.channel))
		tgm_chan_del(&r.space.channel);
	if (!r.space.vm && !r.space.openSpace(kShape, &gRelayout)) {
		r.space.discardSpace(kShape);
		if (!r.unavailable)
			IOLog("tegra-gm20b: block-linear surfaces cannot be moved: no address "
			      "space\n");
		r.unavailable = true;
		return nullptr;
	}
	r.unavailable = false;
	r.space.openChannel(kShape, &gRelayout, nullptr);
	return r.space.channel;
}

void tgm_relayout_finish_cpu(const tgm_relayout_pages *, const tgm_relayout_pages *,
			     const tgm_layout *, uint32_t, tgm_relayout_op);

// A copy that faulted leaves its own partial writes, and its writers' (complete, an Internal job's possibly still there),
// in the GPU's L2, behind which the CPU would read and write: this move's finish, or the caller's tgm_relayout_by_cpu. The faulted channel is gone: a new one's empty
// External job flushes the whole L2. Without a channel only the LTC's MMIO flush could, which polls.
bool flushForCpu()
{
	tgm_chan *chan = channel();
	if (!chan)
		return false;
	uint32_t seqno = 0;
	int ret = 0;
	switch_gpu_chan_submit_enter(chan);
	for (;;) {
		ret = switch_gpu_chan_exec_visibility(chan, nullptr, 0, nullptr, 0, &seqno, nullptr,
						      SWITCH_GPU_SUBMIT_FORCE_FENCE);
		if (ret != -11)
			break;
		ret = switch_gpu_chan_wait_submission_room(chan, nullptr, 0, 0, nullptr);
		if (ret)
			break;
	}
	switch_gpu_chan_submit_leave();
	if (ret)
		return false;
	tgm_copy_wait(tgm_chan_uid(chan), seqno, nullptr, 0);
	return !tgm_chan_error(chan);
}

int move(const tgm_relayout_pages &surface, const tgm_relayout_pages &rows, const tgm_layout &l,
	 uint32_t bytesPerRow, tgm_relayout_op op, const tgm_relayout_writer *writers,
	 uint32_t nwriters)
{
	const Relayout &r = gRelayout;
	const uint64_t surfaceBytes = static_cast<uint64_t>(surface.npages) * kPage;
	const uint64_t rowsBytes = static_cast<uint64_t>(rows.npages) * kPage;
	const uint64_t rowBytes = static_cast<uint64_t>(bytesPerRow) * l.height;
	if (l.bytes > surfaceBytes || bytesPerRow < l.width * l.bpp || rowBytes > surfaceBytes ||
	    rowBytes > rowsBytes)
		return -22;
	tgm_chan *chan = channel();
	if (!chan)
		return -12;
	tgm_relayout_wait waits[TGM_RELAYOUT_MAX_WAITS];
	const uint32_t nwaits =
		tgm_relayout_waits(r.space.vm, tgm_chan_uid(chan), writers, nwriters, waits, 0);
	if (nwaits == TGM_RELAYOUT_WAIT_FAILED)
		return -75;
	SwitchGpuRelayoutMapping *surfaceMap = nullptr, *rowsMap = nullptr;
	bool finish = false;
	int ret = switch_gpu_relayout_mapping(r.space.vm, &gRelayout.mappings, surface.pas,
					      surface.npages, true, &surfaceMap);
	if (!ret)
		ret = switch_gpu_relayout_mapping(r.space.vm, &gRelayout.mappings, rows.pas,
						  rows.npages, false, &rowsMap);
	if (!ret) {
		__atomic_store_n(r.space.words + kCheckpointWord, 0u, __ATOMIC_RELEASE);
		const tgm_relayout_move m = {surfaceMap->tiled,
					     surfaceMap->pitch,
					     rowsMap->pitch,
					     bytesPerRow,
					     op,
					     rowsBytes,
					     kPushVa + uint64_t(kCheckpointWord) * 4};
		const tgm_push push = {
			kPushVa, tgm_relayout_push(r.space.words, &l, &m, waits, nwaits), 0, 0, 0,
			0};
		uint32_t seqno = 0;
		ret = tgm_chan_exec(chan, &push, 1, &seqno, nullptr);
		if (!ret) {
			tgm_relayout_writer acquired[TGM_RELAYOUT_MAX_WAITS];
			for (uint32_t i = 0; i < nwaits; i++)
				acquired[i] = {waits[i].uid, waits[i].seqno};
			tgm_copy_wait(tgm_chan_uid(chan), seqno, acquired, nwaits);
			ret = tgm_chan_error(chan) ? -5 : 0;
			finish = ret &&
				 __atomic_load_n(r.space.words + kCheckpointWord, __ATOMIC_ACQUIRE);
		}
	}
	switch_gpu_relayout_mapping_put(rowsMap);
	switch_gpu_relayout_mapping_put(surfaceMap);
	if (ret && tgm_chan_error(chan)) {
		switch_gpu_relayout_cache_clear(r.space.vm, &gRelayout.mappings);
		if (!flushForCpu())
			switch_refusal("iosurface-layout", "fault-unflushed",
				       "a faulted relayout leaves the GPU's L2 unflushed for the "
				       "CPU's move: "
				       "no copy channel");
	}
	if (finish) {
		tgm_cpu_clean_pages(rows.pas, rows.npages);
		tgm_relayout_finish_cpu(&surface, &rows, &l, bytesPerRow, op);
		ret = 0;
	}
	return ret;
}
}  // namespace

extern "C" uint32_t tgm_relayout_waits(struct tgm_vm *vm, uint32_t self,
				       const struct tgm_relayout_writer *writers, uint32_t nwriters,
				       struct tgm_relayout_wait *waits, uint32_t nwaits)
{
	tgm_relayout_writer running[TGM_RELAYOUT_MAX_WAITS];
	uint64_t fenceVa[TGM_RELAYOUT_MAX_WAITS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < nwriters && n < TGM_RELAYOUT_MAX_WAITS; i++) {
		if (tgm_writer_done(writers[i].uid, writers[i].seqno))
			continue;
		running[n] = writers[i];
		if (tgm_writer_fence_va(vm, writers[i].uid, &fenceVa[n]))
			fenceVa[n] = 0;
		n++;
	}
	uint32_t refused =
		nwriters > TGM_RELAYOUT_MAX_WAITS ? nwriters - TGM_RELAYOUT_MAX_WAITS : 0;
	nwaits = tgm_relayout_follow(self, running, fenceVa, n, waits, nwaits, &refused);
	if (refused) {
		switch_refusal("gpu-order", "writer-unordered",
			       "a kernel copy cannot acquire %u surface writers and was refused",
			       refused);
		return TGM_RELAYOUT_WAIT_FAILED;
	}
	return nwaits;
}

extern "C" vm_offset_t phystokv(uint64_t pa);

namespace
{
uint8_t *physical(uint64_t pa)
{
	return reinterpret_cast<uint8_t *>(phystokv(pa));
}

uint64_t cpuMoves, cpuMoveBytes;

void countCpuMove(const tgm_relayout_pages &surface)
{
	if (TGM_STATS) {
		__atomic_fetch_add(&cpuMoves, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&cpuMoveBytes, uint64_t(surface.npages) * kPage,
				   __ATOMIC_RELAXED);
	}
}

void tgm_relayout_finish_cpu(const tgm_relayout_pages *surface, const tgm_relayout_pages *rows,
			     const tgm_layout *layout, uint32_t bytesPerRow, tgm_relayout_op op)
{
	countCpuMove(*surface);
	tgm_cpu_clean_pages(surface->pas, surface->npages);
	if (op == TGM_RELAYOUT_TILE_IN_PLACE)
		tgm_relayout_cpu(surface, rows, layout, bytesPerRow, 0, physical);
	else if (op == TGM_RELAYOUT_UNTILE_IN_PLACE)
		tgm_relayout_rows_cpu(surface, rows, layout, bytesPerRow, physical);
	tgm_cpu_clean_pages(surface->pas, surface->npages);
}
}  // namespace

extern "C" void tgm_relayout_by_cpu(const struct tgm_relayout_pages *surface,
				    const struct tgm_relayout_pages *rows,
				    const struct tgm_layout *layout, uint32_t bytes_per_row, int op,
				    const struct tgm_relayout_writer *writers, uint32_t nwriters)
{
	for (uint32_t i = 0; i < nwriters; i++)
		tgm_writer_wait(writers[i].uid, writers[i].seqno);
	const bool toRows = op == TGM_RELAYOUT_TO_ROWS;
	const tgm_relayout_pages &to = toRows ? *rows : *surface;
	countCpuMove(*surface);
	tgm_cpu_clean_pages(surface->pas, surface->npages);
	tgm_cpu_clean_pages(rows->pas, rows->npages);
	tgm_relayout_cpu(surface, rows, layout, bytes_per_row, static_cast<int>(toRows), physical);
	tgm_cpu_clean_pages(to.pas, to.npages);
}

extern "C" void tgm_relayout_cpu_stats(uint64_t *moves, uint64_t *bytes)
{
	*moves = __atomic_exchange_n(&cpuMoves, 0, __ATOMIC_RELAXED);
	*bytes = __atomic_exchange_n(&cpuMoveBytes, 0, __ATOMIC_RELAXED);
}

extern "C" int tgm_relayout_init(void)
{
	if (!gRelayout.lock)
		gRelayout.lock = IOLockAlloc();
	return gRelayout.lock ? 0 : -12;
}

extern "C" int tgm_relayout(const struct tgm_relayout_pages *surface,
			    const struct tgm_relayout_pages *rows, const struct tgm_layout *layout,
			    uint32_t bytes_per_row, int op,
			    const struct tgm_relayout_writer *writers, uint32_t nwriters)
{
	if (!gRelayout.lock || !surface || !rows || !layout)
		return -22;
	IOLockLock(gRelayout.lock);
	const int ret = move(*surface, *rows, *layout, bytes_per_row, tgm_relayout_op(op), writers,
			     nwriters);
	IOLockUnlock(gRelayout.lock);
	return ret;
}

struct tgm_scratch : switch_scratch {};

namespace
{
void releaseScratchBacking(void *backing)
{
	switch_gpu_backing_release(static_cast<switch_gpu_backing *>(backing));
}
}

// kits/SwitchScratch's recycled, wired, cacheable memory: the copy engine writes it, a locking task maps it
extern "C" struct tgm_scratch *tgm_scratch_get(uint64_t bytes, int for_task,
					       struct tgm_relayout_pages *pages)
{
	auto *s = static_cast<tgm_scratch *>(
		switch_scratch_get(bytes, for_task ? current_task() : kernel_task));
	if (s && !s->deviceBacking) {
		s->deviceBacking = switch_gpu_pin_memory(s->pas, s->npages, 0, s->memory, true);
		if (!s->deviceBacking) {
			switch_scratch_put(s);
			return nullptr;
		}
		s->releaseDeviceBacking = releaseScratchBacking;
	}
	if (s)
		*pages = {s->pas, s->npages};
	return s;
}

extern "C" void tgm_scratch_put(struct tgm_scratch *s)
{
	switch_scratch_put(s);
}

extern "C" struct tgm_scratch *tgm_scratch_reclaim(struct tgm_scratch *s, uint64_t handout,
						   int *intact)
{
	bool kept = false;
	auto *again = static_cast<tgm_scratch *>(
		switch_scratch_reclaim(s, handout, current_task(), &kept));
	*intact = kept;
	return again;
}

extern "C" uint64_t tgm_scratch_handout(const struct tgm_scratch *s)
{
	return s->handout;
}

extern "C" void tgm_scratch_discard(struct tgm_scratch *s)
{
	switch_scratch_discard(s);
}

extern "C" void *tgm_scratch_memory(const struct tgm_scratch *s)
{
	return s->memory;
}

extern "C" void tgm_scratch_pages(const struct tgm_scratch *s, struct tgm_relayout_pages *pages)
{
	*pages = {s->pas, s->npages};
}

extern "C" void *tgm_scratch_map(struct tgm_scratch *s, void **map)
{
	IOMemoryMap *mapped = s->memory->map();
	*map = mapped;
	return mapped ? reinterpret_cast<void *>(mapped->getVirtualAddress()) : nullptr;
}

extern "C" void tgm_scratch_unmap(void *map)
{
	if (map)
		static_cast<IOMemoryMap *>(map)->release();
}

extern "C" void tgm_scratch_stats(uint64_t *held, uint64_t *peak)
{
	switch_scratch_stats(held, peak);
}
