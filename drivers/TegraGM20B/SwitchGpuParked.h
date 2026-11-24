// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint64_t SWITCH_GPU_PARKED_CAP_BYTES = 128ULL << 20;
constexpr uint64_t SWITCH_GPU_PARKED_DEMAND_MAX_BYTES = 64ULL << 20;

enum class SwitchGpuParkState : uint8_t {
	Live = 0,
	Parking = 1,
	Parked = 2,
	Unparking = 3,
	Reclaiming = 4,
	Reclaimed = 5,
};

enum class SwitchGpuParkAction : uint8_t {
	Ok = 0,
	Busy = 1,
	InvalidState = 2,
	Reclaimed = 3,
};

enum class SwitchGpuParkReject : uint8_t {
	None = 0,
	Unsupported = 1,
	NotMapped = 2,
	MultipleMappings = 3,
	SizeMismatch = 4,
	CpuLeaseActive = 5,
	GpuBusy = 6,
	Freeing = 7,
	MappingInFlight = 8,
};

enum class SwitchGpuParkReclaimPick : uint8_t {
	None = 0,
	Head = 1,
	OldestCached = 2,
};

struct SwitchGpuParkEligibility {
	bool bufferMemory;
	bool wired;
	bool poolVa;
	bool chunked;
	bool contiguous;
	bool control;
	bool nativeView;
	bool surface;
	bool hasBacking;
	bool sharedBacking;
	bool freeing;
	uint32_t mappingCount;
	bool mappingSizeMatches;
	bool mappingInFlight;
	bool cpuLeaseActive;
	bool gpuBusy;
};

struct SwitchGpuParkedNode {
	SwitchGpuParkedNode *prev;
	SwitchGpuParkedNode *next;
	void *client;
	uint64_t handle;
	uint64_t bytes;
	bool system;
	bool linked;
};

inline bool switch_gpu_park_is_live(SwitchGpuParkState state)
{
	return state == SwitchGpuParkState::Live;
}

inline bool switch_gpu_park_is_transient(SwitchGpuParkState state)
{
	return state == SwitchGpuParkState::Parking || state == SwitchGpuParkState::Unparking ||
	       state == SwitchGpuParkState::Reclaiming;
}

inline bool switch_gpu_park_blocks_use(SwitchGpuParkState state)
{
	return state != SwitchGpuParkState::Live;
}

inline SwitchGpuParkAction switch_gpu_park_begin_park(SwitchGpuParkState state)
{
	if (state == SwitchGpuParkState::Live)
		return SwitchGpuParkAction::Ok;
	if (switch_gpu_park_is_transient(state))
		return SwitchGpuParkAction::Busy;
	return SwitchGpuParkAction::InvalidState;
}

inline SwitchGpuParkAction switch_gpu_park_begin_unpark(SwitchGpuParkState state)
{
	if (state == SwitchGpuParkState::Parked)
		return SwitchGpuParkAction::Ok;
	if (state == SwitchGpuParkState::Reclaimed)
		return SwitchGpuParkAction::Reclaimed;
	if (switch_gpu_park_is_transient(state))
		return SwitchGpuParkAction::Busy;
	return SwitchGpuParkAction::InvalidState;
}

inline SwitchGpuParkAction switch_gpu_park_begin_reclaim(SwitchGpuParkState state)
{
	if (state == SwitchGpuParkState::Parked)
		return SwitchGpuParkAction::Ok;
	if (switch_gpu_park_is_transient(state))
		return SwitchGpuParkAction::Busy;
	return SwitchGpuParkAction::InvalidState;
}

inline SwitchGpuParkAction switch_gpu_park_finish_reclaim(SwitchGpuParkState state)
{
	return state == SwitchGpuParkState::Reclaiming ? SwitchGpuParkAction::Ok
						       : SwitchGpuParkAction::InvalidState;
}

inline SwitchGpuParkReject switch_gpu_park_check_eligible(const SwitchGpuParkEligibility &in)
{
	if (in.freeing)
		return SwitchGpuParkReject::Freeing;
	if (!in.bufferMemory || in.wired || in.poolVa || in.chunked || in.contiguous ||
	    in.control || in.nativeView || in.surface || !in.hasBacking || in.sharedBacking)
		return SwitchGpuParkReject::Unsupported;
	if (in.mappingInFlight)
		return SwitchGpuParkReject::MappingInFlight;
	if (in.mappingCount == 0u)
		return SwitchGpuParkReject::NotMapped;
	if (in.mappingCount > 1u)
		return SwitchGpuParkReject::MultipleMappings;
	if (!in.mappingSizeMatches)
		return SwitchGpuParkReject::SizeMismatch;
	if (in.cpuLeaseActive)
		return SwitchGpuParkReject::CpuLeaseActive;
	if (in.gpuBusy)
		return SwitchGpuParkReject::GpuBusy;
	return SwitchGpuParkReject::None;
}

inline uint32_t switch_gpu_park_expected_live_retains(bool hasKernelMap)
{
	return 4u + (hasKernelMap ? 1u : 0u);
}

inline SwitchGpuParkReclaimPick
switch_gpu_park_reclaim_pick(uint64_t totalBytes, uint64_t cachedBytes, bool pressure,
			     uint64_t demandBytes, bool spareRoom,
			     uint64_t capBytes = SWITCH_GPU_PARKED_CAP_BYTES)
{
	if (totalBytes == 0u)
		return SwitchGpuParkReclaimPick::None;
	if (pressure || totalBytes > capBytes)
		return SwitchGpuParkReclaimPick::Head;
	if (demandBytes > 0u && spareRoom && cachedBytes > 0u)
		return SwitchGpuParkReclaimPick::OldestCached;
	return SwitchGpuParkReclaimPick::None;
}

inline uint64_t
switch_gpu_park_clamp_demand(uint64_t currentDemand, uint64_t addBytes,
			     uint64_t maxDemand = SWITCH_GPU_PARKED_DEMAND_MAX_BYTES)
{
	if (addBytes >= maxDemand || currentDemand >= maxDemand - addBytes)
		return maxDemand;
	return currentDemand + addBytes;
}

inline uint64_t switch_gpu_park_consume_demand(uint64_t currentDemand, uint64_t reclaimedBytes)
{
	return reclaimedBytes >= currentDemand ? 0u : currentDemand - reclaimedBytes;
}

class SwitchGpuParkedLru
{
public:
	void pushBack(SwitchGpuParkedNode *node)
	{
		if (!node || node->linked)
			return;
		node->prev = tail_;
		node->next = nullptr;
		if (tail_)
			tail_->next = node;
		else
			head_ = node;
		tail_ = node;
		node->linked = true;
		totalBytes_ += node->bytes;
		++count_;
		if (node->system) {
			systemBytes_ += node->bytes;
		} else {
			cachedBytes_ += node->bytes;
			++cachedCount_;
		}
	}

	void remove(SwitchGpuParkedNode *node)
	{
		if (!node || !node->linked)
			return;
		if (node->prev)
			node->prev->next = node->next;
		else
			head_ = node->next;
		if (node->next)
			node->next->prev = node->prev;
		else
			tail_ = node->prev;
		node->prev = nullptr;
		node->next = nullptr;
		node->linked = false;
		totalBytes_ -= node->bytes;
		--count_;
		if (node->system) {
			systemBytes_ -= node->bytes;
		} else {
			cachedBytes_ -= node->bytes;
			--cachedCount_;
		}
	}

	SwitchGpuParkedNode *peekHead() const
	{
		return head_;
	}

	SwitchGpuParkedNode *peekOldestCached() const
	{
		for (SwitchGpuParkedNode *n = head_; n; n = n->next) {
			if (!n->system)
				return n;
		}
		return nullptr;
	}

	uint64_t totalBytes() const
	{
		return totalBytes_;
	}

	uint64_t cachedBytes() const
	{
		return cachedBytes_;
	}

	uint64_t systemBytes() const
	{
		return systemBytes_;
	}

	uint32_t count() const
	{
		return count_;
	}

	uint32_t cachedCount() const
	{
		return cachedCount_;
	}

	bool empty() const
	{
		return head_ == nullptr;
	}

private:
	SwitchGpuParkedNode *head_ = nullptr;
	SwitchGpuParkedNode *tail_ = nullptr;
	uint64_t totalBytes_ = 0;
	uint64_t cachedBytes_ = 0;
	uint64_t systemBytes_ = 0;
	uint32_t count_ = 0;
	uint32_t cachedCount_ = 0;
};
