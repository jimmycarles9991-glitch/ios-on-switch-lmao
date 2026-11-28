// SPDX-License-Identifier: MIT
// CPU access and job scratch.
#include "TegraGM20BUserClientInternal.h"

void AGXDeviceUserClient::releaseCpuLeases()
{
	while (cpuLeases_) {
		auto *lease = cpuLeases_;
		cpuLeases_ = lease->next;
		lease->reservation.release(lease->backing);
		switch_gpu_backing_release(lease->backing);
		IODelete(lease, CpuLease, 1);
	}
}

IOReturn AGXDeviceUserClient::cpuAccess(const TegraGM20BAbiCpuAccess *in, bool release)
{
	if (in->reserved || !in->bytes ||
	    !(in->access & (TegraGM20BCpuRead | TegraGM20BCpuWrite)) ||
	    (in->access &
	     ~uint32_t(TegraGM20BCpuRead | TegraGM20BCpuWrite | TegraGM20BCpuAvoidSync |
		       TegraGM20BCpuDiscard | TegraGM20BCpuPreserveDirty)))
		return kIOReturnBadArgument;
	if (((in->access & TegraGM20BCpuPreserveDirty) &&
	     in->access !=
		     (TegraGM20BCpuRead | TegraGM20BCpuAvoidSync | TegraGM20BCpuPreserveDirty)) ||
	    ((in->access & TegraGM20BCpuDiscard) &&
	     (!(in->access & TegraGM20BCpuWrite) || (in->access & TegraGM20BCpuRead) ||
	      ((in->offset | in->bytes) & 63))))
		return kIOReturnBadArgument;
	IOLockLock(lock_);
	if (release) {
		auto **at = &cpuLeases_;
		while (*at &&
		       ((*at)->access.handle != in->handle || (*at)->access.offset != in->offset ||
			(*at)->access.bytes != in->bytes || (*at)->access.access != in->access))
			at = &(*at)->next;
		auto *lease = *at;
		if (lease)
			*at = lease->next;
		// lock_ keeps the backing's set alive: its release and the connection's close destroy sets under it
		if (lease && (in->access & TegraGM20BCpuWrite))
			if (auto *set = switch_gpu_backing_set(lease->backing))
				switch_gpu_set_host_written(set);
		IOLockUnlock(lock_);
		if (!lease)
			return kIOReturnBadArgument;
		const int result = lease->reservation.release(lease->backing);
		switch_gpu_backing_release(lease->backing);
		IODelete(lease, CpuLease, 1);
		return ioreturn(result);
	}
	const Bo *bo = findBo(in->handle);
	if (submitGate_.closed() || !bo || !bo->backing || bo->chunked || bo->smmuCompressible ||
	    bo->nativeView || in->offset > bo->bytes || in->bytes > bo->bytes - in->offset ||
	    ((in->access & TegraGM20BCpuWrite) && bo->readOnly)) {
		IOLockUnlock(lock_);
		return kIOReturnBadArgument;
	}
	auto *lease = IONew(CpuLease, 1);
	if (!lease) {
		IOLockUnlock(lock_);
		return kIOReturnNoMemory;
	}
	*lease = {nullptr, bo->backing, *in, {}};
	OSObject *surface = bo->surface;
	const uint32_t surfaceId = surface ? tgm_coh_surface_id(bo->coh) : 0;
	if (surface)
		surface->retain();
	switch_gpu_backing_retain(lease->backing);
	IOLockUnlock(lock_);
	switch_gpu_backing_member_cpu(lease->backing);
	int result = lease->reservation.acquire(lease->backing, surface, surfaceId, task_,
						in->offset, in->bytes, in->access);
	if (surface)
		surface->release();
	if (!result) {
		IOLockLock(lock_);
		if (submitGate_.closed()) {
			result = -19;
		} else {
			lease->next = cpuLeases_;
			cpuLeases_ = lease;
		}
		IOLockUnlock(lock_);
		if (result)
			lease->reservation.release(lease->backing);
	}
	if (result) {
		switch_gpu_backing_release(lease->backing);
		IODelete(lease, CpuLease, 1);
	}
	return ioreturn(result);
}

bool AGXDeviceUserClient::reserveJob(JobScratch &job, uint32_t uses)
{
	if (!job.record)
		job.record = static_cast<SwitchGpuJobBuffer *>(IOMallocData(sizeof(*job.record)));
	if (!job.dependencies)
		job.dependencies = IONew(switch_gpu_backing_token, TegraGM20BMaxJobDependencies);
	if (!job.events) {
		job.events = IONew(SwitchGpuEventJob, 1);
		if (job.events)
			*job.events = {};
	}
	if (!job.record || !job.dependencies || !job.events)
		return false;
	if (uses > job.capacity) {
		uint32_t capacity = job.capacity ? job.capacity : 64;
		while (capacity < uses && capacity <= UINT32_MAX / 2)
			capacity *= 2;
		if (capacity < uses)
			return false;
		auto *made = IONew(switch_gpu_backing_use, capacity);
		auto *writes = IONew(switch_gpu_backing_write_range, capacity);
		auto *guards = IONew(switch_gpu_backing_guard, capacity);
		auto *surfaces = IONew(SwitchGpuSurfaceRefresh, capacity);
		auto **leases = IONew(switch_gpu_cpu_lease *, capacity);
		auto **written = IONew(tgm_coh *, capacity);
		auto **nativeWritten = IONew(void *, capacity);
		if (!made || !writes || !guards || !surfaces || !leases || !written ||
		    !nativeWritten) {
			IODelete(made, switch_gpu_backing_use, capacity);
			IODelete(writes, switch_gpu_backing_write_range, capacity);
			IODelete(guards, switch_gpu_backing_guard, capacity);
			IODelete(surfaces, SwitchGpuSurfaceRefresh, capacity);
			IODelete(leases, switch_gpu_cpu_lease *, capacity);
			IODelete(written, tgm_coh *, capacity);
			IODelete(nativeWritten, void *, capacity);
			return false;
		}
		IODelete(job.uses, switch_gpu_backing_use, job.capacity);
		IODelete(job.writes, switch_gpu_backing_write_range, job.capacity);
		IODelete(job.guards, switch_gpu_backing_guard, job.capacity);
		IODelete(job.surfaces, SwitchGpuSurfaceRefresh, job.capacity);
		IODelete(job.leases, switch_gpu_cpu_lease *, job.capacity);
		IODelete(job.written, tgm_coh *, job.capacity);
		IODelete(job.nativeWritten, void *, job.capacity);
		job.surfaces = surfaces;
		job.leases = leases;
		job.written = written;
		job.nativeWritten = nativeWritten;
		job.uses = made;
		job.writes = writes;
		job.guards = guards;
		job.capacity = capacity;
	}
	return true;
}

void AGXDeviceUserClient::freeJob(JobScratch &job)
{
	if (job.record)
		IOFreeData(job.record, sizeof(*job.record));
	IODelete(job.dependencies, switch_gpu_backing_token, TegraGM20BMaxJobDependencies);
	if (job.events) {
		job.events->clear();
		IODelete(job.events, SwitchGpuEventJob, 1);
	}
	IODelete(job.uses, switch_gpu_backing_use, job.capacity);
	IODelete(job.writes, switch_gpu_backing_write_range, job.capacity);
	IODelete(job.guards, switch_gpu_backing_guard, job.capacity);
	IODelete(job.surfaces, SwitchGpuSurfaceRefresh, job.capacity);
	IODelete(job.leases, switch_gpu_cpu_lease *, job.capacity);
	IODelete(job.written, tgm_coh *, job.capacity);
	IODelete(job.nativeWritten, void *, job.capacity);
	job.useCache.release();
	job = {};
}
