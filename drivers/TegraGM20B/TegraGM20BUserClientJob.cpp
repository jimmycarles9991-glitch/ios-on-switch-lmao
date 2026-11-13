// SPDX-License-Identifier: MIT
// The job interface (TegraGM20BJobABI.h): persistent resource sets.
#include "TegraGM20BUserClientInternal.h"

static const char *setRefusal(uint32_t reason)
{
	switch (reason) {
	case TegraGM20BSetRefusedReserved:
		return "reserved word not zero";
	case TegraGM20BSetRefusedAccess:
		return "access neither read nor write";
	default:
		return "no such BO in this connection";
	}
}

IOReturn AGXDeviceUserClient::resourceSetCreate(const TegraGM20BAbiResourceSetCreate *in,
						TegraGM20BAbiResourceSetCreateReply *out)
{
	if (!in->count || in->count > TegraGM20BMaxSetMembers || !in->uses_address ||
	    in->set > TegraGM20BMaxResourceSets)
		return kIOReturnBadArgument;
	auto *uses = IONew(TegraGM20BAbiSurfaceUse, in->count);
	auto *members = IONew(switch_gpu_set_member, in->count);
	IOReturn result = !uses || !members ? kIOReturnNoMemory : kIOReturnSuccess;
	if (!result && copyin(in->uses_address, uses, in->count * sizeof(*uses)))
		result = kIOReturnBadArgument;
	uint32_t refused = in->count;
	if (!result) {
		IOLockLock(lock_);
		if (submitGate_.closed() || !submitGate_.beginMutation(lock_)) {
			result = kIOReturnNotOpen;
		} else {
			result = attachSet(in, uses, members, out, refused);
			submitGate_.endMutation(lock_);
		}
		IOLockUnlock(lock_);
	}
	if (refused < in->count)
		IOLog("tegra-gm20b: %s: resource set %u refused for member %u of %u (handle %llu, "
		      "access %#x): %s\n",
		      owner_, in->set, refused, in->count, (unsigned long long)uses[refused].handle,
		      uses[refused].access, setRefusal(uses[refused].reserved));
	if ((!result || refused < in->count) &&
	    copyout(uses, in->uses_address, in->count * sizeof(*uses)))
		result = kIOReturnBadArgument;
	IODelete(uses, TegraGM20BAbiSurfaceUse, in->count);
	IODelete(members, switch_gpu_set_member, in->count);
	return result;
}

// lock_ held, every submission excluded. A member the kernel refuses comes back with access 0 in `uses`. A record
// it cannot take at all refuses the call: `refusedAt` is its index, its reserved word the TegraGM20BSetRefused reason.
IOReturn AGXDeviceUserClient::attachSet(const TegraGM20BAbiResourceSetCreate *in,
					TegraGM20BAbiSurfaceUse *uses,
					switch_gpu_set_member *members,
					TegraGM20BAbiResourceSetCreateReply *out,
					uint32_t &refusedAt)
{
	constexpr uint32_t kAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
	static_assert(uint32_t(TegraGM20BSurfaceRead) == SWITCH_GPU_BACKING_READ &&
			      uint32_t(TegraGM20BSurfaceWrite) == SWITCH_GPU_BACKING_WRITE,
		      "use access");
	uint32_t count = 0;
	for (uint32_t i = 0; i < in->count; ++i) {
		const uint32_t access = uses[i].access;
		uint32_t reason = switch_gpu_set_record_refusal(&uses[i]);
		const Bo *bo = nullptr;
		if (!reason &&
		    !(bo = access & TegraGM20BSurfaceGpuAddress ? findAddress(uses[i].handle)
								: findBo(uses[i].handle)))
			reason = TegraGM20BSetRefusedGone;
		if (reason) {
			uses[i].reserved = reason;
			refusedAt = i;
			return kIOReturnBadArgument;
		}
		if (!bo->backing || bo->surface || bo->nativeView || bo->representation ||
		    bo->readOnly)
			uses[i].access = 0;
		else
			members[count++] = {bo->backing, access & kAccess, 0};
	}
	uint32_t index = in->set ? in->set - 1 : 0;
	if (in->set && !sets_[index])
		return kIOReturnBadArgument;
	while (!in->set && index < TegraGM20BMaxResourceSets && sets_[index])
		++index;
	if (index == TegraGM20BMaxResourceSets)
		return kIOReturnNoResources;
	switch_gpu_resource_set *made = nullptr;
	if (!sets_[index]) {
		const int created = switch_gpu_set_create(
			fence_va_ + TegraGM20BSetGenerationWord + index, &made);
		if (created)
			return ioreturn(created);
	}
	uint32_t generation = 0;
	for (;;) {
		switch_gpu_backing *refused = nullptr;
		const int attached = switch_gpu_set_attach(made ? made : sets_[index], members,
							   count, &generation, &refused);
		if (!attached)
			break;
		if (!refused) {
			switch_gpu_set_destroy(made);
			return ioreturn(attached);
		}
		uint32_t kept = 0;
		for (uint32_t m = 0; m < count; ++m)
			if (members[m].backing != refused)
				members[kept++] = members[m];
		if (kept == count) {
			switch_gpu_set_destroy(made);
			return ioreturn(attached);
		}
		count = kept;
		for (uint32_t i = 0; i < in->count; ++i) {
			const Bo *bo = uses[i].access & TegraGM20BSurfaceGpuAddress
					       ? findAddress(uses[i].handle)
					       : findBo(uses[i].handle);
			if (uses[i].access && bo && bo->backing == refused)
				uses[i].access = 0;
		}
	}
	if (made)
		sets_[index] = made;
	*out = {index + 1, generation, kFenceToken, (TegraGM20BSetGenerationWord + index) * 4};
	return kIOReturnSuccess;
}

// lock_ held, every submission excluded
IOReturn AGXDeviceUserClient::resourceSetRelease(const TegraGM20BAbiResourceSetRelease *in)
{
	if (in->reserved || !in->set || in->set > TegraGM20BMaxResourceSets || !sets_[in->set - 1])
		return kIOReturnBadArgument;
	switch_gpu_set_destroy(sets_[in->set - 1]);
	sets_[in->set - 1] = nullptr;
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::destroySets()
{
	for (auto &set : sets_) {
		switch_gpu_set_destroy(set);
		set = nullptr;
	}
}
