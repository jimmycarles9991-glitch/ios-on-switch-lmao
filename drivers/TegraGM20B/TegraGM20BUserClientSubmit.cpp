// SPDX-License-Identifier: MIT
// CPU access, job scratch, and submission (submitAddress, exec).
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
	if (submitGate_.closed() || !bo || !bo->backing || bo->chunked || bo->nativeView ||
	    in->offset > bo->bytes || in->bytes > bo->bytes - in->offset ||
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
	if (!job.bytes)
		job.bytes = IONew(uint64_t, (kJobBytes + 7) / 8);
	if (!job.dependencies)
		job.dependencies = IONew(switch_gpu_backing_token, TegraGM20BMaxJobDependencies);
	if (!job.events) {
		job.events = IONew(SwitchGpuEventJob, 1);
		if (job.events)
			*job.events = {};
	}
	if (!job.bytes || !job.dependencies || !job.events)
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
		if (!made || !writes || !guards || !surfaces || !leases || !written) {
			IODelete(made, switch_gpu_backing_use, capacity);
			IODelete(writes, switch_gpu_backing_write_range, capacity);
			IODelete(guards, switch_gpu_backing_guard, capacity);
			IODelete(surfaces, SwitchGpuSurfaceRefresh, capacity);
			IODelete(leases, switch_gpu_cpu_lease *, capacity);
			IODelete(written, tgm_coh *, capacity);
			return false;
		}
		IODelete(job.uses, switch_gpu_backing_use, job.capacity);
		IODelete(job.writes, switch_gpu_backing_write_range, job.capacity);
		IODelete(job.guards, switch_gpu_backing_guard, job.capacity);
		IODelete(job.surfaces, SwitchGpuSurfaceRefresh, job.capacity);
		IODelete(job.leases, switch_gpu_cpu_lease *, job.capacity);
		IODelete(job.written, tgm_coh *, job.capacity);
		job.surfaces = surfaces;
		job.leases = leases;
		job.written = written;
		job.uses = made;
		job.writes = writes;
		job.guards = guards;
		job.capacity = capacity;
	}
	return true;
}

void AGXDeviceUserClient::freeJob(JobScratch &job)
{
	IODelete(job.bytes, uint64_t, (kJobBytes + 7) / 8);
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
	job = {};
}

IOReturn AGXDeviceUserClient::submitAddress(const TegraGM20BAbiSubmitAddress *in,
					    TegraGM20BAbiExecReply *out)
{
	const uint64_t entered = statsNowNs();
	if (in->reserved || in->bytes < sizeof(TegraGM20BAbiSubmit) || in->bytes > kJobBytes ||
	    !in->address)
		return kIOReturnBadArgument;
	TegraGM20BAbiSubmit header = {};
	if (copyin(in->address, &header, sizeof(header)) || !header.channel ||
	    header.channel > kMaxChannels)
		return kIOReturnBadArgument;
	const uint32_t slot = header.channel - 1;
	IOLockLock(submitLocks_[slot]);
	auto &job = channelJobs_[slot];
	job.phases.start(entered);
	IOReturn result = kIOReturnBadArgument;
	if (!reserveJob(job, TegraGM20BMaxJobUses + TegraGM20BMaxJobPushes)) {
		result = kIOReturnNoMemory;
	} else if (!copyin(in->address, job.bytes, in->bytes)) {
		auto *submit = reinterpret_cast<TegraGM20BAbiSubmit *>(job.bytes);
		if (submit->channel == header.channel &&
		    tgm_fences::ExecSurfaces::validSubmit(submit, in->bytes, true) &&
		    !(submit->surface_flags & TegraGM20BExecSurfacesAll)) {
			auto *pushes = reinterpret_cast<TegraGM20BAbiPush *>(submit + 1);
			const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(
				pushes + submit->push_count);
			constexpr uint32_t admissionMask =
				TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionContinue |
				TegraGM20BSubmitAdmissionEnd | TegraGM20BSubmitAdmissionCancel;
			const auto *tail =
				reinterpret_cast<const uint8_t *>(uses + submit->surface_count);
			uint64_t admissionCookie = 0;
			if (submit->surface_flags & admissionMask) {
				admissionCookie =
					reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(tail)
						->cookie;
				tail += sizeof(TegraGM20BAbiSubmitAdmission);
			}
			const auto *eventInput =
				submit->surface_flags & TegraGM20BSubmitHasEvents
					? reinterpret_cast<const TegraGM20BAbiSubmitEvents *>(tail)
					: nullptr;
			if (submit->surface_flags & TegraGM20BSubmitAdmissionCancel) {
				IOLockLock(lock_);
				result = admissionCookies_[slot] &&
							 admissionCookies_[slot] != admissionCookie
						 ? kIOReturnBadArgument
						 : kIOReturnSuccess;
				if (!result) {
					admissions_[slot].end();
					admissionCookies_[slot] = 0;
					*out = {};
				}
				IOLockUnlock(lock_);
				IOLockUnlock(submitLocks_[slot]);
				return result;
			}
			const auto *releases =
				eventInput && (submit->surface_flags &
					       TegraGM20BSubmitEventReleases)
					? reinterpret_cast<const TegraGM20BAbiSignalRelease *>(
						  reinterpret_cast<const TegraGM20BAbiEventUse *>(
							  eventInput + 1) +
						  eventInput->wait_count + eventInput->signal_count)
					: nullptr;
			const int prepared =
				job.events->prepare(events_, eventInput, releases, header.channel);
			result = prepared == -5 ? kIOReturnAborted : ioreturn(prepared);
			if (!result) {
				IOLockLock(lock_);
				if (submitGate_.enter(lock_)) {
					result = exec(submit->channel, pushes, submit->push_count,
						      out, uses, submit->surface_count, true, &job,
						      submit->surface_flags, job.events,
						      admissionCookie);
#if TGM_STATS
					if (!result && chans_[slot]) {
						job.phases.end(TGM_SUBMIT_COMMIT);
						tgm_stats_submit(chans_[slot], job.phases.ns);
					}
#endif
					switch_gpu_path_taken(SWITCH_GPU_PATH_ADDRESS_JOB,
							      result ? 0 : 1);
					submitGate_.leave(lock_);
				} else {
					result = kIOReturnNotOpen;
				}
				IOLockUnlock(lock_);
			}
			job.events->clear();
		}
	}
	IOLockUnlock(submitLocks_[slot]);
	return result;
}

IOReturn AGXDeviceUserClient::exec(uint32_t channel, TegraGM20BAbiPush *input, uint32_t count,
				   TegraGM20BAbiExecReply *out, const TegraGM20BAbiSurfaceUse *uses,
				   uint32_t useCount, bool native, JobScratch *scratch,
				   uint32_t visibility, SwitchGpuEventJob *events,
				   uint64_t admissionCookie)
{
	if (!channel || channel > kMaxChannels || !chans_[channel - 1])
		return kIOReturnBadArgument;
	auto &admission = admissions_[channel - 1];
	const bool admissionBegin = visibility & TegraGM20BSubmitAdmissionBegin;
	const bool admissionContinue = visibility & TegraGM20BSubmitAdmissionContinue;
	if (admissionBegin ? (admission.active() || !admissionCookie ||
			      admissionCookie <= lastAdmissionCookies_[channel - 1])
	    : admissionContinue
		    ? (!admission.active() || admissionCookies_[channel - 1] != admissionCookie)
		    : admission.active())
		return kIOReturnBadArgument;
	tgm_chan *chan = chans_[channel - 1];
	// runningboardd's GPU denial, as IOKit reads it for kIOUCForegroundOnly methods (task_is_gpu_denied), at each
	// submission: a denied process runs at its background priority
	const bool background = clientHasPrivilege(current_task(), kIOClientPrivilegeForeground) !=
				kIOReturnSuccess;
	if (background != backgroundRole_) {
		backgroundRole_ = background;
		applyLevel(scratch == nullptr);
	}
	if (scratch && channelLevels_[channel - 1] != level_) {
		tgm_chan_set_level(chan, level_);
		channelLevels_[channel - 1] = level_;
	}
	// Taken whatever happens to the Exec. The writers' acquires are queued for this submission's stream before it is made
	// (one this Exec then does not make leaves them to the channel's next: each still names earlier work).
	const int followed = native ? 0 : surfaces_.before(channel - 1, chan, count, false);
	if (followed < 0)
		return ioreturn(followed);
	const bool listed = followed != 0;
	// TegraGM20BAbiPush and tgm_push share one layout; input is our private copy, so normalise flags in place
	static_assert(sizeof(tgm_push) == sizeof(TegraGM20BAbiPush) &&
			      offsetof(tgm_push, no_prefetch) == offsetof(TegraGM20BAbiPush, flags),
		      "push layouts");
	tgm_push *pushes = reinterpret_cast<tgm_push *>(input);
	const uint32_t fenceFlag = native && scratch ? uint32_t(TegraGM20BPushFence) : 0u;
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiPush &p = input[i];
		if ((p.va & 3) || !p.dwords || p.dwords >= (1u << 21) ||
		    (p.flags & ~static_cast<uint32_t>(TegraGM20BPushNoPrefetch |
						      TegraGM20BPushImportsWritten |
						      TegraGM20BPushIncomplete | fenceFlag)) ||
		    ((p.flags & TegraGM20BPushIncomplete) &&
		     (i + 1 == count || !(input[i + 1].flags & TegraGM20BPushNoPrefetch))) ||
		    p.va < TGM_USER_VA_START ||
		    p.va + static_cast<uint64_t>(p.dwords) * 4 > TGM_KERNEL_VA)
			return kIOReturnBadArgument;
		if (p.flags & TegraGM20BPushImportsWritten)
			importsWritten_ = true;
		const uint32_t flags = p.flags;
		pushes[i].no_prefetch = (flags & TegraGM20BPushNoPrefetch) ? 1u : 0u;
		pushes[i].incomplete = (flags & TegraGM20BPushIncomplete) ? 1u : 0u;
		pushes[i].fence = (flags & TegraGM20BPushFence) ? 1u : 0u;
		pushes[i].reserved = 0;
	}
	uint32_t fences = 0;
	if (switch_gpu_job_fences(pushes, count, &fences))
		return kIOReturnBadArgument;
	tgm_prof prof;
	const tgm_prof *profile = scanSubmission(pushes, count, prof, false);
	uint32_t seqno = 0;
	uint32_t baseUses = native ? useCount + count : 0;
	if (!native)
		for (const Bo *bo = bos_; bo; bo = bo->next)
			++baseUses;
	auto &job = scratch ? *scratch : job_;
	if (baseUses > UINT32_MAX / 2 || !reserveJob(job, baseUses * 2))
		return kIOReturnNoMemory;
	if (!events) {
		events = job.events;
		events->clear();
	}
	uint32_t useTotal = 0, writeCount = 0, surfaceCount = 0, nativeGuardCount = 0,
		 writtenCount = 0, rangedUses = 0, nativeReads = 0;
	bool hostWritten = switch_gpu_submit_acquires_host_writes(visibility);
	bool unlockedWrites = false, scanoutWrites = false;
	const uint32_t claim = switch_gpu_claim_flags(admissionBegin || admissionContinue);
	// flags: the SWITCH_GPU_BACKING_* ownership flags of the use beside its access
	const auto addUse = [&](const Bo *bo, uint32_t access, uint32_t flags, uint64_t offset = 0,
				uint64_t bytes = 0) {
		hostWritten |= (access & TegraGM20BSurfaceHostWritten) != 0;
		access &= ~uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceHostWritten |
				    TegraGM20BSurfaceRange);
		if (bo->nativeView) {
			if (TGM_STATS)
				++nativeReads;
			const uint32_t first = useTotal;
			const int error = switch_gpu_native_surface_add_read(
				*bo->nativeView, access, job.uses, job.guards, job.capacity,
				useTotal, nativeGuardCount);
			for (uint32_t i = first; !error && i < useTotal; ++i)
				job.uses[i].access |= flags;
			return error;
		}
		job.uses[useTotal++] = {bo->backing, access | flags, 0};
		if (access & SWITCH_GPU_BACKING_WRITE) {
			job.writes[writeCount++] = {bo->backing, bo->representation ? 0 : offset,
						    bo->representation ? 0 : bytes};
			unlockedWrites |= bo->unlockedReads;
			scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;
		}
		if (bo->representation)
			job.surfaces[surfaceCount++] = {bo->backing,
							bo->surface,
							bo->representation,
							&const_cast<Bo *>(bo)->representationValue,
							tgm_coh_surface_id(bo->coh),
							access,
							0,
							nullptr,
							nullptr,
							nullptr};
		return 0;
	};
	for (uint32_t i = 0; native && i < useCount; ++i) {
		const bool addressed = uses[i].access & TegraGM20BSurfaceGpuAddress;
		const uint64_t bytes =
			uses[i].access & TegraGM20BSurfaceRange ? uses[i].reserved : 0;
		if (TGM_STATS)
			rangedUses += bytes != 0;
		const Binding *binding =
			addressed ? switch_gpu_binding_find(binds_, nbinds_, uses[i].handle,
							    bytes ? bytes : 1)
				  : nullptr;
		const Bo *bo =
			addressed ? (binding ? binding->bo : nullptr) : findBo(uses[i].handle);
		uint64_t offset = 0;
		if (!bo || !bo->backing ||
		    (bytes &&
		     !switch_gpu_binding_offset(*binding, uses[i].handle, bytes, offset)) ||
		    ((uses[i].access & TegraGM20BSurfaceWrite) && bo->readOnly))
			return kIOReturnBadArgument;
		if (const int error = addUse(bo, uses[i].access, claim, offset, bytes))
			return ioreturn(error);
		if (uses[i].access & TegraGM20BSurfaceWrite)
			job.written[writtenCount++] = bo->coh;
	}
	// A job's pushes lie in the push ring and its indirect arguments' BOs: past kPushBos of them, a use per push.
	constexpr uint32_t kPushBos = 8;
	const Bo *pushed[kPushBos];
	uint32_t pushedCount = 0;
	for (uint32_t i = 0; native && i < count; ++i) {
		const Binding *binding = switch_gpu_binding_find(binds_, nbinds_, pushes[i].va,
								 uint64_t(pushes[i].dwords) * 4);
		const Bo *bo = binding ? binding->bo : nullptr;
		hostWritten |= binding && !binding->retirement->vol;
		if (!bo || !bo->backing)
			return kIOReturnBadArgument;
		uint32_t seen = 0;
		while (seen < pushedCount && pushed[seen] != bo)
			++seen;
		if (seen < pushedCount)
			continue;
		if (pushedCount < kPushBos)
			pushed[pushedCount++] = bo;
		if (const int error = addUse(bo, SWITCH_GPU_BACKING_READ, claim))
			return ioreturn(error);
	}
	if (!native) {
		constexpr uint32_t kAccess = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
		const int error = switch_gpu_legacy_uses(
			bos_, listed,
			[this](uint64_t handle) {
				return surfaces_.listedAccess(handle);
			},
			[&](const Bo *bo, uint32_t access) {
				return addUse(bo, access & kAccess, access & ~kAccess);
			});
		if (error)
			return ioreturn(error);
	}
	const uint32_t guardCount = surfaceCount + nativeGuardCount;
	if (admissionContinue && !admission.covers(job.uses, useTotal))
		return kIOReturnBadArgument;
	switch_gpu_backing_transaction transaction = {};
	switch_gpu_backing_token completion = {};
	uint32_t dependencies = 0, jobVisibility = 0;
	int error = 0;
	bool ownershipBusy = false, ownershipFailed = false;
	// The active-submit gate pins this channel, its bindings and BOs while other channels and CPU releases proceed.
	IOLockUnlock(lock_);
	tgm_views_job_writes(job.written, writtenCount);
	if (scratch)
		scratch->phases.end(TGM_SUBMIT_RESOLVE);
	uint32_t leaseCount = 0;
	if (events)
		error = events->sequence(eventChannels_[channel - 1]);
	if (error) {
		events->clear();
		IOLockLock(lock_);
		return error == -5 ? kIOReturnAborted : ioreturn(error);
	}
refresh:
	if (admissionContinue) {
		for (uint32_t i = 0; i < surfaceCount; ++i) {
			const auto &surface = job.surfaces[i];
			job.guards[nativeGuardCount + i] = {
				surface.backing, surface.generation,
				__atomic_load_n(surface.cached, __ATOMIC_ACQUIRE)};
		}
		error = 0;
	} else {
		error = (count || admissionBegin)
				? SwitchGpuSurfaceRefresh::prepare(
					  job.surfaces, surfaceCount, task_,
					  job.guards + nativeGuardCount, job.leases, &leaseCount)
				: 0;
	}
	if (!error && admissionBegin) {
		error = admission.begin(job.uses, useTotal, job.guards, guardCount, job.leases,
					leaseCount);
		if (!error) {
			admissionCookies_[channel - 1] = admissionCookie;
			lastAdmissionCookies_[channel - 1] = admissionCookie;
		}
	}
	ownershipBusy = error == -16;
	ownershipFailed = error == -5;
	switch_gpu_chan_submit_enter(chan);
	for (; !error;) {
		dependencies = 0;
		if (count || (visibility & TegraGM20BSubmitHasEvents))
			switch_gpu_chan_job_token(chan, fences, &completion);
		if (count && !error) {
			error = leaseCount
					? switch_gpu_backing_begin_conversion_checked(
						  job.uses, useTotal, job.leases, leaseCount,
						  job.guards, guardCount, &completion, &transaction)
					: switch_gpu_backing_begin_checked(
						  job.uses, useTotal, job.guards, guardCount,
						  &completion, &transaction);
			ownershipBusy = error == -16;
			if (!error)
				error = switch_gpu_backing_write_ranges(&transaction, job.writes,
									writeCount);
			if (!error)
				error = switch_gpu_backing_dependencies(
					&transaction, job.dependencies, TegraGM20BMaxJobUses,
					&dependencies);
		}
		if (!error && events)
			error = events->append(job.dependencies, TegraGM20BMaxJobDependencies,
					       &dependencies);
		if (!error) {
			const int captured =
				events->captureDependencies(job.dependencies, dependencies);
			if (captured > 0) {
				switch_gpu_backing_abort(&transaction);
				error = events->inheritDependencies();
				ownershipFailed = error == -5;
				if (error)
					break;
				continue;
			}
			error = captured;
		}
		if (!error)
			error = events->bind(&completion);
		ownershipFailed = error == -5;
		if (!error) {
			if (scratch)
				scratch->phases.end(TGM_SUBMIT_LOCK);
			jobVisibility =
				switch_gpu_job_visibility(visibility, events && events->signals(),
							  unlockedWrites || scanoutWrites,
							  hostWritten || transaction.host_written);
			if (events->released())
				switch_gpu_chan_event_releases(chan, uint32_t(completion.value));
			error = switch_gpu_chan_exec_events(
				chan, pushes, count, job.dependencies, dependencies, nullptr, 0,
				events->words(), events->wordCount(), &seqno, profile,
				jobVisibility, nullptr, 0);
			if (scratch)
				scratch->phases.end(TGM_SUBMIT_EXEC);
		}
		if (error != -11)
			break;
		switch_gpu_backing_abort(&transaction);
		error = switch_gpu_chan_wait_submission_room(
			chan, pushes, count, dependencies + events->wordCount(), profile);
		if (error)
			break;
	}
	if (count && !error)
		switch_gpu_backing_commit(&transaction);
	else
		switch_gpu_backing_abort(&transaction);
	if (!error && events)
		events->commit();
	switch_gpu_chan_submit_leave();
	SwitchGpuSurfaceRefresh::release(job.surfaces, surfaceCount);
	if (error == SWITCH_GPU_BACKING_STALE && !nativeGuardCount && !admission.active())
		goto refresh;
	if (admission.active() && admissionCookies_[channel - 1] == admissionCookie &&
	    (error || (visibility & TegraGM20BSubmitAdmissionEnd))) {
		admission.end();
		admissionCookies_[channel - 1] = 0;
	}
	if (events)
		events->clear();
	IOLockLock(lock_);
	const IOReturn ret = ownershipFailed ? kIOReturnAborted
			     : ownershipBusy ? kIOReturnCannotLock
					     : ioreturn(error);
	if (ret)
		return ret;
	if (count) {
		switch_gpu_path_job(jobVisibility, unlockedWrites, leanChannels_[channel - 1],
				    rangedUses);
		switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_READ, nativeReads);
		// The host imports this submission may have written, marked GPU-dirty with the submission as their writer
		// (atomic stores, no cache maintenance): a CPU IOSurfaceLock of one waits for this seqno, then invalidates
		// (tgm_coherency_*, the XNU hooks), and a listed Exec of another channel waits for it on the GPU.
		if (!native)
			surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);
		else
			for (uint32_t i = 0; i < writtenCount; ++i)
				tgm_coherency_wrote_listed(job.written[i], tgm_chan_uid(chan),
							   seqno);
	}
	out->seqno = seqno;
	out->reserved = 0;
	return kIOReturnSuccess;
}
