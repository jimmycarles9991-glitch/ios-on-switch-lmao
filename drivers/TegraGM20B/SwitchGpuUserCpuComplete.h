// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuPathStats.h"

IOReturn AGXDeviceUserClient::cpuComplete(const TegraGM20BAbiCpuComplete *input,
					  TegraGM20BAbiCpuCompleteReply *reply)
{
	if (!input->channel_uid || !input->channel || input->channel > kMaxChannels ||
	    input->reserved || !input->ranges_address || !input->range_count ||
	    input->range_count > TegraGM20BMaxJobUses)
		return kIOReturnBadArgument;
	auto *ranges = IONew(TegraGM20BAbiCpuRange, input->range_count);
	auto *resolved = IONew(SwitchGpuCpuRange, input->range_count);
	if (!ranges || !resolved) {
		IODelete(ranges, TegraGM20BAbiCpuRange, input->range_count);
		IODelete(resolved, SwitchGpuCpuRange, input->range_count);
		return kIOReturnNoMemory;
	}
	IOReturn result =
		copyin(input->ranges_address, ranges, input->range_count * sizeof(*ranges))
			? kIOReturnBadArgument
			: kIOReturnSuccess;
	switch_gpu_backing_token prefix = {};
	bool held = false;
	uint32_t count = 0;
	IOLockLock(lock_);
	auto *channel = chans_[input->channel - 1];
	if (!result && (submitGate_.closed() || !channel))
		result = kIOReturnNotOpen;
	if (!result && (input->channel_uid != switch_gpu_chan_identity(channel) ||
			int32_t(tgm_chan_submitted(channel) - input->seqno) < 0))
		result = kIOReturnNotReady;
	if (!result) {
		switch_gpu_chan_token(channel, input->seqno, &prefix);
		result = ioreturn(switch_gpu_backing_token_retain(&prefix));
		held = !result;
	}
	for (uint32_t i = 0; !result && i < input->range_count; ++i) {
		const auto &range = ranges[i];
		const Bo *bo = findBo(range.handle);
		if (!bo || !bo->backing || bo->chunked || bo->nativeView || bo->representation ||
		    !range.bytes || range.offset > bo->bytes ||
		    range.bytes > bo->bytes - range.offset) {
			result = kIOReturnBadArgument;
			break;
		}
		resolved[count++] = {bo->backing, range.offset, range.bytes};
		switch_gpu_backing_retain(bo->backing);
	}
	IOLockUnlock(lock_);
	for (uint32_t i = 0; !result && i < count; ++i)
		switch_gpu_backing_member_cpu(resolved[i].backing);
	if (!result) {
		result = ioreturn(switch_gpu_cpu_complete(prefix, resolved, count, reply));
		switch_gpu_path_taken(SWITCH_GPU_PATH_CPU_COMPLETE, 1);
	}
	for (uint32_t i = 0; i < count; ++i)
		switch_gpu_backing_release(resolved[i].backing);
	if (held)
		switch_gpu_backing_token_release(&prefix);
	IODelete(resolved, SwitchGpuCpuRange, input->range_count);
	IODelete(ranges, TegraGM20BAbiCpuRange, input->range_count);
	return result;
}
