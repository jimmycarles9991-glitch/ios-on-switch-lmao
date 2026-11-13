// SPDX-License-Identifier: MIT
// SubmitJob (TegraGM20BJobABI.h method 33): a job over a resource set and its shared uses, without the connection lock.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuJobRecord.h"
#include "SwitchGpuQuiet.h"

static_assert(SwitchGpuJobRecord::kMaxBytes <= kJobBytes,
	      "a channel's job scratch holds a SubmitJob record");

IOReturn AGXDeviceUserClient::submitJob(const TegraGM20BAbiSubmitAddress *in,
					TegraGM20BAbiExecReply *out)
{
	const uint64_t entered = statsNowNs();
	if (in->reserved || in->bytes < sizeof(TegraGM20BAbiJob) ||
	    in->bytes > SwitchGpuJobRecord::kMaxBytes || !in->address)
		return kIOReturnBadArgument;
	TegraGM20BAbiJob header = {};
	if (copyin(in->address, &header, sizeof(header)) || !header.channel ||
	    header.channel > kMaxChannels)
		return kIOReturnBadArgument;
	const uint32_t slot = header.channel - 1;
	IOLockLock(submitLocks_[slot]);
	auto &job = channelJobs_[slot];
	job.phases.start(entered);
	IOReturn result = kIOReturnNoMemory;
	if (reserveJob(job, TegraGM20BMaxJobUses + TegraGM20BMaxJobPushes)) {
		result = kIOReturnBadArgument;
		const auto record = copyin(in->address, job.bytes, in->bytes)
					    ? SwitchGpuJobRecord{}
					    : SwitchGpuJobRecord::parse(job.bytes, in->bytes);
		if (record.valid() && record.job->channel == header.channel &&
		    (record.job->flags & TegraGM20BJobAdmissionCancel)) {
			IOLockLock(lock_);
			result = admissionCookies_[slot] &&
						 admissionCookies_[slot] != record.admission
					 ? kIOReturnBadArgument
					 : kIOReturnSuccess;
			if (!result) {
				admissions_[slot].end();
				admissionCookies_[slot] = 0;
				*out = {};
			}
			IOLockUnlock(lock_);
			job.events->clear();
			IOLockUnlock(submitLocks_[slot]);
			return result;
		}
		if (record.valid() && record.job->channel == header.channel) {
			const int prepared = job.events->prepare(events_, record.events,
								 record.releases, header.channel);
			result = prepared == -5 ? kIOReturnAborted : ioreturn(prepared);
		}
		if (!result) {
			bool admitted = submitGate_.enterFast();
			if (!admitted) {
				IOLockLock(lock_);
				admitted = submitGate_.enterSlow(lock_);
				IOLockUnlock(lock_);
			}
			result = admitted ? runJob(record, job, out) : kIOReturnNotOpen;
			if (admitted)
				submitGate_.leaveFast(lock_);
		}
		job.events->clear();
	}
	IOLockUnlock(submitLocks_[slot]);
	return result;
}

// Inside the submit gate (channels, BOs, bindings and sets stay as they are) and the channel's submit lock.
IOReturn AGXDeviceUserClient::runJob(const SwitchGpuJobRecord &record, JobScratch &job,
				     TegraGM20BAbiExecReply *out)
{
	const TegraGM20BAbiJob &head = *record.job;
	const uint32_t slot = head.channel - 1;
	tgm_chan *chan = chans_[slot];
	if (!chan)
		return kIOReturnBadArgument;
	const bool background = clientHasPrivilege(current_task(), kIOClientPrivilegeForeground) !=
				kIOReturnSuccess;
	if (background != backgroundRole_ || channelLevels_[slot] != level_) {
		IOLockLock(lock_);
		if (background != backgroundRole_) {
			backgroundRole_ = background;
			applyLevel(false);
		}
		if (channelLevels_[slot] != level_) {
			tgm_chan_set_level(chan, level_);
			channelLevels_[slot] = level_;
		}
		IOLockUnlock(lock_);
	}
	switch_gpu_resource_set *set = head.set ? sets_[head.set - 1] : nullptr;
	if (head.set && !set)
		return kIOReturnBadArgument;
	if (set && !switch_gpu_set_enter(set, head.set_generation)) {
		switch_gpu_job_stale();
		switch_gpu_path_taken(SWITCH_GPU_PATH_STALE_SET, 1);
		return kIOReturnNotReady;
	}
	switch_gpu_sibling_wait siblings[SWITCH_GPU_SET_SLOTS];
	uint32_t siblingCount = 0;
	if (set) {
		static_assert(kMaxChannels == SWITCH_GPU_SET_SLOTS, "a set slot per channel");
		uint32_t live = 0, completed[kMaxChannels] = {};
		for (uint32_t other = 0; other < kMaxChannels; ++other)
			if (chans_[other] && !tgm_chan_error(chans_[other])) {
				live |= 1u << other;
				completed[other] = tgm_chan_completed(chans_[other]);
			}
		switch_gpu_set_wait waits[SWITCH_GPU_SET_SLOTS];
		siblingCount = switch_gpu_set_waits(set, slot, live, completed, waits);
		for (uint32_t i = 0; i < siblingCount; ++i)
			siblings[i] = {chans_[waits[i].slot], waits[i].seqno};
	}
	const IOReturn result = runJobUses(record, job, out, set, siblings, siblingCount);
	if (set)
		switch_gpu_set_leave(set);
	return result;
}

IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record, JobScratch &job,
					 TegraGM20BAbiExecReply *out, switch_gpu_resource_set *set,
					 const switch_gpu_sibling_wait *siblings,
					 uint32_t siblingCount)
{
	const TegraGM20BAbiJob &head = *record.job;
	const uint32_t slot = head.channel - 1, count = head.push_count;
	tgm_chan *chan = chans_[slot];
	auto &admission = admissions_[slot];
	const bool admissionBegin = head.flags & TegraGM20BJobAdmissionBegin;
	const bool admissionContinue = head.flags & TegraGM20BJobAdmissionContinue;
	if (admissionBegin ? (admission.active() || record.admission <= lastAdmissionCookies_[slot])
	    : admissionContinue
		    ? (!admission.active() || admissionCookies_[slot] != record.admission)
		    : admission.active())
		return kIOReturnBadArgument;
	static_assert(sizeof(tgm_push) == sizeof(TegraGM20BAbiPush) &&
			      offsetof(tgm_push, no_prefetch) == offsetof(TegraGM20BAbiPush, flags),
		      "push layouts");
	auto *pushes = reinterpret_cast<tgm_push *>(record.pushes);
	constexpr uint32_t kPushFlags =
		TegraGM20BPushNoPrefetch | TegraGM20BPushIncomplete | TegraGM20BPushFence;
	for (uint32_t i = 0; i < count; ++i) {
		const TegraGM20BAbiPush &p = record.pushes[i];
		if ((p.va & 3) || !p.dwords || p.dwords >= (1u << 21) || (p.flags & ~kPushFlags) ||
		    ((p.flags & TegraGM20BPushIncomplete) &&
		     (i + 1 == count ||
		      !(record.pushes[i + 1].flags & TegraGM20BPushNoPrefetch))) ||
		    p.va < TGM_USER_VA_START || p.va + uint64_t(p.dwords) * 4 > TGM_KERNEL_VA)
			return kIOReturnBadArgument;
		const uint32_t flags = p.flags;
		pushes[i].no_prefetch = (flags & TegraGM20BPushNoPrefetch) ? 1u : 0u;
		pushes[i].incomplete = (flags & TegraGM20BPushIncomplete) ? 1u : 0u;
		pushes[i].fence = (flags & TegraGM20BPushFence) ? 1u : 0u;
		pushes[i].reserved = 0;
	}
	uint32_t fences = 0;
	if (switch_gpu_job_fences(pushes, count, &fences))
		return kIOReturnBadArgument;
#if TGM_STATS
	tgm_prof prof;
	const tgm_prof *profile = scanSubmission(pushes, count, prof, true);
#else
	const tgm_prof *profile = nullptr;
#endif
	uint32_t useTotal = 0, writeCount = 0, surfaceCount = 0, nativeGuardCount = 0,
		 writtenCount = 0, rangedUses = 0, nativeReads = 0;
	bool hostWritten = (head.flags & TegraGM20BJobHostWritten) ||
			   (set && switch_gpu_set_take_host_written(set));
	bool unlockedWrites = false, scanoutWrites = false;
	constexpr uint32_t kUseAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
	constexpr uint32_t kUseFlags = kUseAccess | TegraGM20BSurfaceHostWritten |
				       TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange;
	const uint32_t claim = switch_gpu_claim_flags(admissionBegin || admissionContinue);
	for (uint32_t i = 0; i < head.shared_count; ++i) {
		const TegraGM20BAbiSurfaceUse &use = record.uses[i];
		const bool addressed = use.access & TegraGM20BSurfaceGpuAddress;
		const uint64_t bytes = use.access & TegraGM20BSurfaceRange ? use.reserved : 0;
		if (!(use.access & kUseAccess) || (use.access & ~kUseFlags) ||
		    (!bytes && use.reserved))
			return kIOReturnBadArgument;
		rangedUses += bytes != 0;
		const Binding *binding =
			addressed ? switch_gpu_binding_find(binds_, nbinds_, use.handle,
							    bytes ? bytes : 1)
				  : nullptr;
		const Bo *bo = addressed ? (binding ? binding->bo : nullptr) : findBo(use.handle);
		uint64_t offset = 0;
		if (!bo || !bo->backing || switch_gpu_backing_set(bo->backing) ||
		    (bytes && !switch_gpu_binding_offset(*binding, use.handle, bytes, offset)) ||
		    ((use.access & TegraGM20BSurfaceWrite) && bo->readOnly))
			return kIOReturnBadArgument;
		hostWritten |= (use.access & TegraGM20BSurfaceHostWritten) != 0;
		const uint32_t access = use.access & kUseAccess;
		if (bo->nativeView) {
			++nativeReads;
			const uint32_t first = useTotal;
			if (const int error = switch_gpu_native_surface_add_read(
				    *bo->nativeView, access, job.uses, job.guards, job.capacity,
				    useTotal, nativeGuardCount))
				return ioreturn(error);
			for (uint32_t u = first; u < useTotal; ++u)
				job.uses[u].access |= claim;
			continue;
		}
		job.uses[useTotal++] = {bo->backing, access | claim, 0};
		if (access & TegraGM20BSurfaceWrite) {
			job.writes[writeCount++] = {bo->backing, bo->representation ? 0 : offset,
						    bo->representation ? 0 : bytes};
			unlockedWrites |= bo->unlockedReads;
			scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;
			job.written[writtenCount++] = bo->coh;
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
	}
	const uint32_t guardCount = surfaceCount + nativeGuardCount;
	if (admissionContinue && !admission.covers(job.uses, useTotal))
		return kIOReturnBadArgument;
	tgm_views_job_writes(job.written, writtenCount);
	job.phases.end(TGM_SUBMIT_RESOLVE);
	SwitchGpuEventJob *events = job.events;
	int error = events->sequence(eventChannels_[slot]);
	if (error)
		return error == -5 ? kIOReturnAborted : ioreturn(error);
	// an event's waiters are woken by the interrupt at its signal; the job's semantic fence and scanout writes are
	// known below (switch_gpu_quiet_end)
	const bool quiet =
		(head.flags & TegraGM20BJobQuiet) && !(head.flags & TegraGM20BJobHasEvents);
	const bool closesGroup =
		!(head.flags & TegraGM20BJobAdmission) || (head.flags & TegraGM20BJobAdmissionEnd);
	switch_gpu_release releases[TegraGM20BMaxJobPromises] = {};
	const uint32_t keptCount = record.promises ? record.promises->count : 0;
	// the job's fence releases them: a job with no pushes and no events has none
	if (keptCount && ((!count && !(head.flags & TegraGM20BJobHasEvents)) ||
			  keepPromises(record.promises, releases)))
		return kIOReturnBadArgument;
	const uint32_t submit =
		TegraGM20BSubmitVisibilityExplicit |
		((head.flags & TegraGM20BJobExternal) ? uint32_t(TegraGM20BSubmitExternalVisibility)
						      : 0u) |
		((head.flags & TegraGM20BJobHasEvents) ? uint32_t(TegraGM20BSubmitHasEvents) : 0u);
	switch_gpu_backing_transaction transaction = {};
	switch_gpu_backing_token completion = {};
	uint32_t seqno = 0, dependencies = 0, jobVisibility = 0, leaseCount = 0;
	bool ownershipBusy = false, ownershipFailed = false;
refresh:
	if (admissionContinue) {
		for (uint32_t i = 0; i < surfaceCount; ++i)
			job.guards[nativeGuardCount + i] = {
				job.surfaces[i].backing, job.surfaces[i].generation,
				__atomic_load_n(job.surfaces[i].cached, __ATOMIC_ACQUIRE)};
		error = 0;
	} else {
		error = count || admissionBegin
				? SwitchGpuSurfaceRefresh::prepare(
					  job.surfaces, surfaceCount, task_,
					  job.guards + nativeGuardCount, job.leases, &leaseCount)
				: 0;
	}
	if (!error && admissionBegin) {
		error = admission.begin(job.uses, useTotal, job.guards, guardCount, job.leases,
					leaseCount);
		if (!error) {
			admissionCookies_[slot] = record.admission;
			lastAdmissionCookies_[slot] = record.admission;
		}
	}
	ownershipBusy = error == -16;
	ownershipFailed = error == -5;
	switch_gpu_chan_submit_enter(chan);
	for (; !error;) {
		dependencies = 0;
		const bool runs = count || (head.flags & TegraGM20BJobHasEvents);
		if (runs)
			switch_gpu_chan_job_token(chan, fences, &completion);
		if (count) {
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
		if (!error)
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
		// another connection's writer promises on what the job uses: acquired on the GPU, never a shared event's
		// captured dependency; read under the transaction's locks on the uses' backings, never by taking one again
		if (!error && count) {
			const uint32_t ownPromises = tgm_fence_offset(promiseWord(0));
			error = switch_gpu_backing_promise_dependencies(
				&transaction, job.dependencies, TegraGM20BMaxJobDependencies,
				&dependencies, ownPromises,
				ownPromises + TegraGM20BMaxPromises * 4);
		}
		if (!error)
			error = events->bind(&completion);
		ownershipFailed = error == -5;
		if (!error && dependencies + siblingCount + events->wordCount() >
				      TegraGM20BMaxJobDependencies)
			error = -28;
		if (!error) {
			job.phases.end(TGM_SUBMIT_LOCK);
			const bool quietEnd = switch_gpu_quiet_end(quiet, events->semanticFence(),
								   scanoutWrites, closesGroup);
			jobVisibility =
				switch_gpu_job_visibility(submit, events->signals(),
							  unlockedWrites || scanoutWrites,
							  hostWritten || transaction.host_written) |
				(quietEnd ? uint32_t(SWITCH_GPU_SUBMIT_QUIET) : 0u);
			if (events->released())
				switch_gpu_chan_event_releases(chan, uint32_t(completion.value));
			error = switch_gpu_chan_exec_events(
				chan, pushes, count, job.dependencies, dependencies, siblings,
				siblingCount, events->words(), events->wordCount(), &seqno, profile,
				jobVisibility, releases, keptCount);
			job.phases.end(TGM_SUBMIT_EXEC);
		}
		if (error != -11)
			break;
		switch_gpu_backing_abort(&transaction);
		error = switch_gpu_chan_wait_submission_room(
			chan, pushes, count, dependencies + siblingCount + events->wordCount(),
			profile);
		if (error)
			break;
	}
	if (count && !error)
		switch_gpu_backing_commit(&transaction);
	else
		switch_gpu_backing_abort(&transaction);
	if (keptCount && !error)
		keptPromises(record.promises, slot, completion);
	if (!error) {
		events->commit();
		if (set && (count || (head.flags & TegraGM20BJobHasEvents)))
			switch_gpu_set_note_job(set, slot, &completion);
	}
	switch_gpu_chan_submit_leave();
	SwitchGpuSurfaceRefresh::release(job.surfaces, surfaceCount);
	if (error == SWITCH_GPU_BACKING_STALE && !nativeGuardCount && !admission.active())
		goto refresh;
	if (admission.active() && admissionCookies_[slot] == record.admission &&
	    (error || (head.flags & TegraGM20BJobAdmissionEnd))) {
		admission.end();
		admissionCookies_[slot] = 0;
	}
	if (ownershipFailed)
		return kIOReturnAborted;
	if (ownershipBusy)
		return kIOReturnCannotLock;
	if (error)
		return ioreturn(error);
	if (count) {
		switch_gpu_path_job(jobVisibility, unlockedWrites, leanChannels_[slot], rangedUses);
		switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_READ, nativeReads);
		for (uint32_t i = 0; i < writtenCount; ++i)
			tgm_coherency_wrote_listed(job.written[i], tgm_chan_uid(chan), seqno);
	}
	job.phases.end(TGM_SUBMIT_COMMIT);
#if TGM_STATS
	switch_gpu_job_submitted(job.phases.ns, set != nullptr, head.shared_count, siblingCount);
#endif
	switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, set ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, head.shared_count);
	switch_gpu_path_taken(SWITCH_GPU_PATH_QUIET_JOB,
			      (jobVisibility & SWITCH_GPU_SUBMIT_QUIET) && count ? 1 : 0);
	out->seqno = seqno;
	out->reserved = 0;
	return kIOReturnSuccess;
}
