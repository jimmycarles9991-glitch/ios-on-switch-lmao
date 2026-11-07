// SPDX-License-Identifier: MIT
// See TegraGM20BCopy.h. Copies run on one channel of the kernel's own GPU client, made at the first copy: kJobs slots, each
// with a push page and two VA windows (source, destination) that its plane's pages are mapped into for the copy. A copy
// returns once its push is kicked; the channel's fence interrupt queues a thread call, which unmaps the finished slots and
// calls their `done`. GPU-ordered mappings (tgm_vm_map_ordered) need no CPU wait: the next submission invalidates.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread_call.h>
#include <stdint.h>
#include "TegraGM20BCopyPush.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BTiled.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace
{
constexpr uint32_t kJobs = 8;
constexpr uint64_t kPage = 4096, kPushVa = 1ull << 20, kWindow = 1ull << 32,
		   kFirstWindow = 8ull << 32;

struct Plane {
	tgm_mem *mem;
	uint64_t *pas;
	uint32_t npages;
	uint64_t va, bytes;
};

struct Job {
	bool busy;
	uint32_t seqno;
	Plane src, dst;
	tegra_gm20b_ce_done done;
};

struct Copier {
	IOLock *lock;
	tgm_vm *vm;
	tgm_chan *chan;
	tgm_mem *push, *fence;
	uint32_t *pushCpu;
	void *fenceCpu;
	uint64_t pushPa, fencePa, fenceGpu;
	thread_call_t reap;
	bool failed;
	Job jobs[kJobs];
} gCopy;

void fenced(void *)
{
	thread_call_enter(gCopy.reap);
}

bool openSpace()
{
	Copier &c = gCopy;
	uint64_t pas[kJobs];
	c.pushCpu = static_cast<uint32_t *>(tgm_pool_alloc(kPage * kJobs, 0, &c.pushPa));
	c.fenceCpu = tgm_pool_alloc(kPage, 0, &c.fencePa);
	if (!c.pushCpu || !c.fenceCpu || tgm_vm_new(&c.vm))
		return false;
	tgm_vm_claim(c.vm, &gCopy);
	for (uint32_t i = 0; i < kJobs; i++)
		pas[i] = c.pushPa + i * kPage;
	return !tgm_mem_new(c.vm, pas, kJobs, &c.push) &&
	       !tgm_vm_map_ordered(c.vm, kPushVa, kPage * kJobs, c.push, 0, 0, 1, nullptr) &&
	       !tgm_mem_new(c.vm, &c.fencePa, 1, &c.fence) &&
	       !tgm_vm_kmap(c.vm, c.fence, kPage, &c.fenceGpu);
}

// lock held: the channel, made again after a fault killed the last one (once its copies were reaped)
tgm_chan *channel()
{
	Copier &c = gCopy;
	if (c.chan && tgm_chan_error(c.chan)) {
		for (const Job &j : c.jobs)
			if (j.busy)
				return nullptr;
		tgm_chan_del(&c.chan);
	}
	if (!c.vm && !c.failed && !openSpace())
		c.failed = true;
	if (!c.chan && c.vm &&
	    !tgm_chan_new(c.vm, &gCopy, static_cast<volatile uint32_t *>(c.fenceCpu), c.fenceGpu,
			  "ce-copy", 0, &c.chan))
		tgm_chan_on_fence(c.chan, fenced, nullptr);
	return c.chan;
}

void unmap(Plane &p)
{
	if (p.mem) {
		tgm_vm_unmap(gCopy.vm, p.va & ~(kPage - 1), uint64_t(p.npages) * kPage);
		tgm_mem_del(&p.mem);
	}
	if (p.pas)
		IODelete(p.pas, uint64_t, p.npages);
	p = {};
}

// The plane's pages, from the 4 KiB page holding its first byte, into `window` under `kind`; p.va is its first byte there
int map(Plane &p, IOMemoryDescriptor *memory, uint64_t offset, uint64_t bytes, uint64_t window,
	uint8_t kind)
{
	const uint64_t first = offset & ~(kPage - 1);
	if (!bytes || offset + bytes > memory->getLength() || offset + bytes - first > kWindow)
		return -22;
	p.npages = uint32_t((offset + bytes - first + kPage - 1) / kPage);
	p.pas = IONew(uint64_t, p.npages);
	if (!p.pas)
		return -12;
	for (uint32_t i = 0; i < p.npages; i++) {
		p.pas[i] = memory->getPhysicalSegment(first + uint64_t(i) * kPage, nullptr,
						      kIOMemoryMapperNone);
		if (!p.pas[i])
			return -22;
	}
	if (const int ret = tgm_mem_new(gCopy.vm, p.pas, p.npages, &p.mem))
		return ret;
	p.va = window + (offset - first);
	p.bytes = bytes;
	return tgm_vm_map_ordered(gCopy.vm, window, uint64_t(p.npages) * kPage, p.mem, 0, kind, 0,
				  nullptr);
}

void reapWork(thread_call_param_t, thread_call_param_t)
{
	Copier &c = gCopy;
	tegra_gm20b_ce_done finished[kJobs];
	int status[kJobs];
	uint32_t n = 0;
	IOLockLock(c.lock);
	const uint32_t completed = c.chan ? tgm_chan_completed(c.chan) : 0;
	const int error = c.chan && tgm_chan_error(c.chan) ? -5 : 0;
	for (Job &j : c.jobs) {
		if (!j.busy || (!error && int32_t(completed - j.seqno) < 0))
			continue;
		unmap(j.src);
		unmap(j.dst);
		j.busy = false;
		if (j.done.fn) {
			finished[n] = j.done;
			status[n++] = error;
		}
	}
	IOLockUnlock(c.lock);
	for (uint32_t i = 0; i < n; i++)
		finished[i].fn(finished[i].arg, status[i]);
}

// The reap call is published before the lock, which says the copier is ready; a racing first call frees what it lost
bool init()
{
	Copier &c = gCopy;
	if (__atomic_load_n(&c.lock, __ATOMIC_ACQUIRE))
		return true;
	thread_call_t reap =
		thread_call_allocate_with_priority(reapWork, nullptr, THREAD_CALL_PRIORITY_KERNEL);
	IOLock *lock = reap ? IOLockAlloc() : nullptr;
	if (!lock) {
		if (reap)
			thread_call_free(reap);
		return false;
	}
	thread_call_t noCall = nullptr;
	if (!__atomic_compare_exchange_n(&c.reap, &noCall, reap, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		thread_call_free(reap);
	IOLock *noLock = nullptr;
	if (!__atomic_compare_exchange_n(&c.lock, &noLock, lock, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		IOLockFree(lock);
	return true;
}

// An IOSurface a copy reads or writes, by the descriptor's first page: its coherency entry (a reference, or null when the
// GPU driver never imported it) and its layout when the kernel made it block-linear
struct Surface {
	tgm_coh *coh = nullptr;
	bool tiled = false;
	tgm_layout layout = {};

	explicit Surface(IOMemoryDescriptor *memory)
	{
		coh = tgm_coh_find_pa(memory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone));
		if (!coh)
			return;
		tgm_coh_lock(coh);
		const tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(coh), __ATOMIC_ACQUIRE);
		if (t && t->tiled) {
			tiled = true;
			layout = t->layout;
		}
		tgm_coh_unlock(coh);
	}

	~Surface()
	{
		if (coh)
			tgm_coherency_put(coh);
	}

	// lock held: its GPU writers' fences, appended at waits[n]
	void writers(tgm_relayout_wait *waits, uint32_t &n) const
	{
		if (!coh)
			return;
		tgm_relayout_writer w[TGM_RELAYOUT_MAX_WAITS];
		tgm_coh_lock(coh);
		const uint32_t found = tgm_coh_writers(coh, w, TGM_RELAYOUT_MAX_WAITS - n);
		tgm_coh_unlock(coh);
		for (uint32_t i = 0; i < found; i++)
			if (!tgm_writer_fence_va(gCopy.vm, w[i].uid, &waits[n].va))
				waits[n++].seqno = w[i].seqno;
	}

	bool whole(const tegra_gm20b_ce_plane &p, uint32_t rowBytes, uint32_t rows) const
	{
		return !p.offset && rowBytes == layout.width * layout.bpp && rows == layout.height;
	}
};

bool shapeOk(const Surface &from, const Surface &to, const tegra_gm20b_ce_plane &src,
	     const tegra_gm20b_ce_plane &dst, uint32_t rowBytes, uint32_t rows)
{
	if (from.tiled && to.tiled)
		return false;
	return (from.tiled ? from.whole(src, rowBytes, rows) : src.pitch >= rowBytes) &&
	       (to.tiled ? to.whole(dst, rowBytes, rows) : dst.pitch >= rowBytes);
}
}  // namespace

extern "C" int tegra_gm20b_ce_copy_plane(const struct tegra_gm20b_ce_plane *src,
					 const struct tegra_gm20b_ce_plane *dst, uint32_t row_bytes,
					 uint32_t rows, const struct tegra_gm20b_ce_wait *wait,
					 const struct tegra_gm20b_ce_done *done)
{
	if (!src || !dst || !src->memory || !dst->memory || !row_bytes || !rows ||
	    ((src->offset | dst->offset) & 15))
		return -22;
	if (!init())
		return -12;
	auto *srcMemory = static_cast<IOMemoryDescriptor *>(src->memory);
	auto *dstMemory = static_cast<IOMemoryDescriptor *>(dst->memory);
	Copier &c = gCopy;
	IOLockLock(c.lock);
	tgm_chan *chan = channel();
	uint32_t slot = 0;
	while (slot < kJobs && c.jobs[slot].busy)
		slot++;
	if (!chan || slot == kJobs) {
		IOLockUnlock(c.lock);
		if (chan)
			switch_refusal(
				"gpu-copy", "busy",
				"every one of the copy engine's %u plane copies is in flight",
				kJobs);
		return chan ? -16 : -12;
	}
	Job &j = c.jobs[slot];
	const Surface from(srcMemory), to(dstMemory);
	tgm_relayout_wait waits[TGM_RELAYOUT_MAX_WAITS];
	uint32_t nwaits = 0;
	from.writers(waits, nwaits);
	to.writers(waits, nwaits);
	int ret = shapeOk(from, to, *src, *dst, row_bytes, rows) ? 0 : -22;
	const uint64_t srcBytes =
		from.tiled ? from.layout.bytes : uint64_t(src->pitch) * (rows - 1) + row_bytes;
	const uint64_t dstBytes =
		to.tiled ? to.layout.bytes : uint64_t(dst->pitch) * (rows - 1) + row_bytes;
	const uint64_t windows = kFirstWindow + uint64_t(slot) * 2 * kWindow;
	if (!ret)
		ret = map(j.src, srcMemory, src->offset, srcBytes, windows,
			  from.tiled ? uint8_t(TGM_LAYOUT_KIND) : 0);
	if (!ret)
		ret = map(j.dst, dstMemory, dst->offset, dstBytes, windows + kWindow,
			  to.tiled ? uint8_t(TGM_LAYOUT_KIND) : 0);
	uint32_t seqno = 0;
	if (!ret) {
		const tgm_copy_job job = {j.src.va,
					  j.dst.va,
					  src->pitch,
					  dst->pitch,
					  row_bytes,
					  rows,
					  from.tiled ? &from.layout : nullptr,
					  to.tiled ? &to.layout : nullptr,
					  wait ? wait->syncpt : TEGRA_GM20B_CE_NO_SYNCPT,
					  wait ? wait->threshold : 0,
					  done ? done->syncpt : TEGRA_GM20B_CE_NO_SYNCPT};
		uint32_t *words = c.pushCpu + slot * (kPage / 4);
		const tgm_push push = {kPushVa + slot * kPage,
				       tgm_copy_push(words, &job, waits, nwaits), 0};
		ret = tgm_chan_exec(chan, &push, 1, &seqno, nullptr);
	}
	if (ret) {
		unmap(j.src);
		unmap(j.dst);
	} else {
		j.busy = true;
		j.seqno = seqno;
		j.done = done ? *done
			      : tegra_gm20b_ce_done{TEGRA_GM20B_CE_NO_SYNCPT, nullptr, nullptr};
		if (to.coh)
			tgm_coherency_wrote(to.coh, 1, tgm_chan_uid(chan), seqno);
	}
	IOLockUnlock(c.lock);
	return ret;
}
