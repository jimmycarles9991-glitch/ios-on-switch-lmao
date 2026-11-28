// SPDX-License-Identifier: MIT
// The host1x bridge's slots and queue (SwitchGpuBridge.cpp: submission and the consumer calls; SwitchGpuBridgeRetire.cpp:
// retirement and the hang check). Every field is the queue lock's, except three read with no lock while the caller holds
// the bridge: `result` (an atomic store and load, switch_gpu_bridge_failed) and `producer` and `threshold`, fixed for
// the slot's life (switch_gpu_bridge_jobless, switch_gpu_bridge_syncpoint).
#pragma once
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "SwitchGpuBridge.h"
#include "SwitchGpuKqueue.h"
#include "SwitchGpuSubmitTurn.h"
#include "TegraGM20BGpu.h"

struct switch_gpu_bridge {
	switch_gpu_backing_token tokens[SWITCH_GPU_BRIDGE_MAX_TOKENS];
	uint32_t count, refs, syncpoint, threshold, sequence, waiters;
	// 0: the bridge's own GPU job; else its producer's syncpoint, `threshold` that producer's, reached when the job
	// `covering` names (its channel, its last seqno) drained
	uint32_t producer;
	switch_gpu_backing_token covering;
	bool active, notified, watched;
	uint32_t pushSyncpoint;  // syncpoint and gate VA the slot's push was written for (slotPush)
	uint64_t pushGate;
	int result;
	void (*completed)(void *, int);
	void *context;
};

namespace switch_gpu_bridge_queue
{
constexpr uint32_t kSlots = 8, kHangCheckNs = 1000000000;
constexpr uint64_t kPage = 4096, kPushVa = 1ULL << 20;
constexpr SwitchGpuKqueueShape kShape = {
	"host1x-bridge", TGM_FENCE_KERNEL_BRIDGE, kSlots, kPushVa, 1, 0};

struct BridgeQueue {
	IOLock *lock;
	thread_call_t retirement, hangCheck;
	uint64_t checkAt;
	SwitchGpuKqueue space;
	SwitchGpuSubmitTurn turn;
	uint32_t waitingSources;
	switch_gpu_bridge jobs[kSlots];
};

extern BridgeQueue queue;

// queue.lock held
void releaseLocked(switch_gpu_bridge &job);
int producers(const switch_gpu_bridge &job);
// A jobless slot whose covering job resolved (cover 1 or <0): true once its tokens' writes are in DRAM
bool coveringDrained(const switch_gpu_bridge &job, int source, int cover);
bool anyWaited();
void armHangCheck(uint64_t ns);

// SwitchGpuBridgeGate.cpp: gateOpen runs under queue.turn without queue.lock; slotPush with queue.lock held
bool gateOpen(tgm_vm *vm);
uint32_t slotPush(switch_gpu_bridge &job, uint32_t slot);

void checkHang(thread_call_param_t, thread_call_param_t);
void retire(thread_call_param_t, thread_call_param_t);
}  // namespace switch_gpu_bridge_queue
