// SPDX-License-Identifier: MIT
// The copy engine's moves of block-linear IOSurfaces (TegraGM20BRelayout.h) on the kernel's own GPU client: one address
// space and copy channel made at the first move, a push page and a fence page from the GPU pool; a move maps the surface's
// pages twice (TGM_LAYOUT_KIND and kind 0) and the scratch once, and unmaps them after it. Moves are serialised; the caller
// sleeps on the move's fence interrupt (tgm_writer_wait, which recovers a copy that stops progressing as it does a stuck
// writer), so the bytes are in DRAM where they were asked for when it returns. The channel is a client channel to the
// rest of the driver (kernel 0): the present's acquires and a lock's wait for every channel include it, and a fault on it
// kills it alone (the next move makes another).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/task.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BFences.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BRelayout.h"
#include "TegraGM20BTiled.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchScratch/SwitchScratch.h"

namespace
{
constexpr uint64_t kPage = 4096;
constexpr uint64_t kPushVa = 1ULL << 20, kSurfaceVa = 1ULL << 32, kSurfaceRowsVa = 2ULL << 32,
		   kRowsVa = 3ULL << 32;

struct Relayout {
	IOLock *lock;
	tgm_vm *vm;
	tgm_chan *chan;
	tgm_mem *push, *fence;
	uint32_t *pushCpu;
	volatile uint32_t *fenceCpu;
	uint64_t pushPa, fenceGpu;
	bool failed;  // no address space: every move is refused
} gRelayout;

bool openSpace()
{
	Relayout &r = gRelayout;
	uint64_t fencePage = 0;
	uint32_t fenceOffset = 0;
	r.pushCpu = static_cast<uint32_t *>(tgm_pool_alloc(kPage, 0, &r.pushPa));
	if (!r.pushCpu ||
	    tgm_fence_kernel(TGM_FENCE_KERNEL_RELAYOUT, &r.fenceCpu, &fencePage, &fenceOffset) ||
	    tgm_vm_new(&r.vm))
		return false;
	tgm_vm_claim(r.vm, &gRelayout);
	if ((tgm_mem_new(r.vm, &r.pushPa, 1, &r.push) != 0) ||
	    (tgm_vm_map_ordered(r.vm, kPushVa, kPage, r.push, 0, 0, 1, nullptr) != 0) ||
	    (tgm_mem_new(r.vm, &fencePage, 1, &r.fence) != 0) ||
	    (tgm_vm_kmap(r.vm, r.fence, kPage, &r.fenceGpu) != 0))
		return false;
	r.fenceGpu += fenceOffset;
	return true;
}

tgm_chan *channel()
{
	Relayout &r = gRelayout;
	if (r.chan && tgm_chan_error(r.chan))
		tgm_chan_del(&r.chan);
	if (!r.vm && !r.failed && !openSpace()) {
		r.failed = true;
		IOLog("tegra-gm20b: block-linear surfaces cannot be moved: no address space\n");
	}
	if (!r.chan && !r.failed) {
		tgm_chan_new(r.vm, &gRelayout, r.fenceCpu, r.fenceGpu, "relayout", 1, &r.chan);
	}
	return r.chan;
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
		tgm_relayout_waits(r.vm, tgm_chan_uid(chan), writers, nwriters, waits, 0);
	tgm_mem *surfaceMem = nullptr;
	tgm_mem *rowsMem = nullptr;
	int ret = tgm_mem_new(r.vm, surface.pas, surface.npages, &surfaceMem);
	if (!ret)
		ret = tgm_mem_new(r.vm, rows.pas, rows.npages, &rowsMem);
	if (!ret) {
		ret = tgm_vm_map_ordered(r.vm, kSurfaceVa, surfaceBytes, surfaceMem, 0,
					 TGM_LAYOUT_KIND, 0, nullptr);
	}
	if (!ret) {
		ret = tgm_vm_map_ordered(r.vm, kSurfaceRowsVa, surfaceBytes, surfaceMem, 0, 0, 0,
					 nullptr);
	}
	if (!ret)
		ret = tgm_vm_map_ordered(r.vm, kRowsVa, rowsBytes, rowsMem, 0, 0, 0, nullptr);
	if (!ret) {
		const tgm_relayout_move m = {kSurfaceVa, kSurfaceRowsVa, kRowsVa, bytesPerRow,
					     op,         rowsBytes};
		const tgm_push push = {kPushVa, tgm_relayout_push(r.pushCpu, &l, &m, waits, nwaits),
				       0};
		uint32_t seqno = 0;
		ret = tgm_chan_exec(chan, &push, 1, &seqno, nullptr);
		if (!ret) {
			tgm_writer_wait(tgm_chan_uid(chan), seqno);
			ret = tgm_chan_error(chan) ? -5 : 0;
		}
	}
	tgm_vm_unmap(r.vm, kRowsVa, rowsBytes);
	tgm_vm_unmap(r.vm, kSurfaceRowsVa, surfaceBytes);
	tgm_vm_unmap(r.vm, kSurfaceVa, surfaceBytes);
	tgm_mem_del(&rowsMem);
	tgm_mem_del(&surfaceMem);
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
		switch_refusal(
			"gpu-order", "writer-unordered",
			"a kernel copy goes ahead of %u surface writers it cannot acquire on",
			refused);
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
	tgm_cpu_clean_pages(surface->pas, surface->npages);
	tgm_cpu_clean_pages(rows->pas, rows->npages);
	tgm_relayout_cpu(surface, rows, layout, bytes_per_row, static_cast<int>(toRows), physical);
	tgm_cpu_clean_pages(to.pas, to.npages);
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

// kits/SwitchScratch's recycled, wired, cacheable memory: the copy engine writes it, a locking task maps it
extern "C" struct tgm_scratch *tgm_scratch_get(uint64_t bytes, int for_task,
					       struct tgm_relayout_pages *pages)
{
	auto *s = static_cast<tgm_scratch *>(
		switch_scratch_get(bytes, for_task ? current_task() : kernel_task));
	if (s)
		*pages = {s->pas, s->npages};
	return s;
}

extern "C" void tgm_scratch_put(struct tgm_scratch *s)
{
	switch_scratch_put(s);
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

extern "C" void tgm_scratch_stats(uint64_t *held, uint64_t *peak)
{
	switch_scratch_stats(held, peak);
}
