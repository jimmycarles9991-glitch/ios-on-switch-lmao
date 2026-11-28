// SPDX-License-Identifier: MIT
// The gate every bridge job with a GPU job of its own passes between its producers' acquires and its drain and
// increments: a word in the kernel's fence slot (TGM_FENCE_KERNEL_BRIDGE_GATE) the job's push acquires at kOpen. A path
// that raises a producer's word over writes that never came closes it first (a kill's forward store, an abandoned or
// given-up promise, host1x's release of a failed syncpoint) after marking that producer failed. No bridge job passes on
// that raise until gatePass has cancelled the host1x consumers of every bridge whose producer failed; the job's own
// increments then reach a threshold nothing waits on any more, with no CPU increment that could overshoot later ones.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "SwitchGpuBridgeQueue.h"
#include "TegraGM20BSyncpt.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace switch_gpu_bridge_queue
{
namespace
{
constexpr uint32_t kOpen = 0, kClosed = 1;
// NV906F SEMAPHOREA..D (0x10..0x1c) incrementing group, OPERATION_ACQUIRE | ACQUIRE_SWITCH_ENABLED.
constexpr uint32_t kSemaphoreGroup = (1u << 29) | (4u << 16) | (0x10 >> 2),
		   kAcquireEqual = 1u | (1u << 12);
constexpr uint32_t kGateDwords = 5;

struct Gate {
	volatile uint32_t *word;  // published last: a close before it finds no bridge job to hold
	uint64_t va;              // in the bridge channel's address space
	IOLock *lock;             // serialises closes against a pass's reopen
	uint32_t closes;          // under `lock`
	thread_call_t pass;
} bridgeGate;

// The kernel fence pool is write-combined: plain store-release + DSB reaches GPU DRAM without an exclusive RMW.
void writeGate(volatile uint32_t *word, uint32_t value)
{
	__atomic_store_n(word, value, __ATOMIC_RELEASE);
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ volatile("dsb sy" ::: "memory");
#endif
}

void gatePass(thread_call_param_t, thread_call_param_t)
{
	struct Cancel {
		uint32_t syncpoint, threshold;
	};

	IOLockLock(queue.lock);
	IOLockLock(bridgeGate.lock);
	const uint32_t seen = bridgeGate.closes;
	IOLockUnlock(bridgeGate.lock);
	// Cancelling a bridge's waiters may fail a VIC or NVDEC job that another active bridge waits on; repeat until
	// a pass finds none so transitive chains are cancelled before the gate reopens.
	for (;;) {
		Cancel cancels[kSlots];
		uint32_t count = 0;
		for (auto &job : queue.jobs) {
			if (!job.active || job.producer ||
			    __atomic_load_n(&job.result, __ATOMIC_ACQUIRE))
				continue;
			const int source = producers(job);
			if (source < 0) {
				__atomic_store_n(&job.result, source, __ATOMIC_RELEASE);
				cancels[count++] = {job.syncpoint, job.threshold};
			}
		}
		if (!count)
			break;
		for (uint32_t i = 0; i < count; ++i)
			tegra_host1x_syncpt_cancel_waiters(cancels[i].syncpoint,
							   cancels[i].threshold);
	}
	IOLockLock(bridgeGate.lock);
	const bool closedSince = bridgeGate.closes != seen;
	if (!closedSince)
		writeGate(bridgeGate.word, kOpen);
	IOLockUnlock(bridgeGate.lock);
	IOLockUnlock(queue.lock);
	if (closedSince)
		thread_call_enter(bridgeGate.pass);
}
}  // namespace

bool gateOpen(tgm_vm *vm)
{
	if (!bridgeGate.lock)
		bridgeGate.lock = IOLockAlloc();
	if (!bridgeGate.pass)
		bridgeGate.pass = thread_call_allocate_with_options(
			gatePass, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	volatile uint32_t *word = nullptr;
	uint64_t page = 0;
	uint32_t offset = 0;
	if (!bridgeGate.lock || !bridgeGate.pass ||
	    tgm_fence_kernel(TGM_FENCE_KERNEL_BRIDGE_GATE, &word, &page, &offset))
		return false;
	bridgeGate.va = tgm_fence_kernel_va(vm, TGM_FENCE_KERNEL_BRIDGE_GATE);
	if (!bridgeGate.va)
		return false;
	writeGate(word, kOpen);
	__atomic_store_n(&bridgeGate.word, word, __ATOMIC_RELEASE);
	return true;
}

uint32_t slotPush(switch_gpu_bridge &job, uint32_t slot)
{
	if (job.pushSyncpoint != job.syncpoint || job.pushGate != bridgeGate.va) {
		uint32_t *w = queue.space.words + slot * (kPage / 4);
		w[0] = kSemaphoreGroup;
		w[1] = uint32_t(bridgeGate.va >> 32) & 0xffu;
		w[2] = uint32_t(bridgeGate.va);
		w[3] = kOpen;
		w[4] = kAcquireEqual;
		tgm_syncpt_incr(w + kGateDwords, job.syncpoint);
		job.pushSyncpoint = job.syncpoint;
		job.pushGate = bridgeGate.va;
	}
	return kGateDwords + TGM_SYNCPT_INCR_DWORDS;
}
}  // namespace switch_gpu_bridge_queue

using namespace switch_gpu_bridge_queue;

extern "C" void switch_gpu_bridge_gate_close(void)
{
	volatile uint32_t *word = __atomic_load_n(&bridgeGate.word, __ATOMIC_ACQUIRE);
	if (!word)
		return;
	IOLockLock(bridgeGate.lock);
	++bridgeGate.closes;
	writeGate(word, kClosed);
	IOLockUnlock(bridgeGate.lock);
	thread_call_enter(bridgeGate.pass);
}
