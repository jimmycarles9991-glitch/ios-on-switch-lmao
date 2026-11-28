// SPDX-License-Identifier: MIT
// SubmitJob (TegraGM20BJobABI.h method 33): a job over a resource set and its shared uses, without the connection lock.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuQuiet.h"

IOReturn AGXDeviceUserClient::submitJob(const TegraGM20BAbiSubmitAddress *in,
					TegraGM20BAbiExecReply *out)
{
	SwitchGpuRefusalNote why;
	uint32_t channel = 0;
	const IOReturn result = takeJob(in, out, why, channel);
	if (result == kIOReturnSuccess)
		return result;
	logRefusal("SubmitJob", channel, result, why);
	char line[320];
	if (switch_gpu_refusal_loses_frame(critical_, why.site, result) &&
	    switch_gpu_refusal_line(line, sizeof(line), owner_, ownerPid_, "SubmitJob", channel,
				    result, why, 0))
		panic("%s: the compositor's frame is lost", line);
	return result;
}

IOReturn AGXDeviceUserClient::takeJob(const TegraGM20BAbiSubmitAddress *in,
				      TegraGM20BAbiExecReply *out, SwitchGpuRefusalNote &why,
				      uint32_t &channel)
{
	const uint64_t entered = statsNowNs();
	if (in->reserved || in->bytes < sizeof(TegraGM20BAbiJob) ||
	    in->bytes > SwitchGpuJobRecord::kMaxBytes || !in->address)
		return switch_gpu_refuse(why, SwitchGpuRefusal::JobRecord, kIOReturnBadArgument,
					 in->reserved, in->bytes, in->address);
	TegraGM20BAbiJob header = {};
	if (copyin(in->address, &header, sizeof(header)) || !header.channel ||
	    header.channel > kMaxChannels)
		return switch_gpu_refuse(why, SwitchGpuRefusal::JobHeader, kIOReturnBadArgument,
					 in->address);
	channel = header.channel;
	const uint32_t slot = header.channel - 1;
	IOLockLock(submitLocks_[slot]);
	auto &job = channelJobs_[slot];
	job.phases.start(entered);
	IOReturn result = switch_gpu_refuse(why, SwitchGpuRefusal::JobScratch, kIOReturnNoMemory);
	if (reserveJob(job, TegraGM20BMaxJobUses + TegraGM20BMaxJobPushes)) {
		why = {};
		const auto record = job.record->stage(
			header, in->bytes, [&](uint8_t *dst, size_t tail) {
				return copyin(in->address + sizeof(header), dst, tail) == 0;
			});
		const bool parsed = record.valid() && record.job->channel == header.channel;
		result = parsed ? kIOReturnSuccess
				: switch_gpu_refuse(why, SwitchGpuRefusal::JobParse,
						    kIOReturnBadArgument, in->bytes,
						    record.valid() ? record.job->channel : 0);
		if (parsed && (record.job->flags & TegraGM20BJobAdmissionCancel)) {
			IOLockLock(lock_);
			const uint64_t current = admissionCookies_[slot];
			result = current && current != record.admission
					 ? switch_gpu_refuse(why, SwitchGpuRefusal::AdmissionCancel,
							     kIOReturnBadArgument, record.admission,
							     current)
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
		if (parsed) {
			const int prepared = job.events->prepare(events_, record.events,
								 record.releases, header.channel);
			if (prepared)
				result = switch_gpu_refuse(why, SwitchGpuRefusal::EventPrepare,
							   ioreturn(prepared),
							   switch_gpu_refusal_errno(prepared));
		}
		if (!result) {
			bool admitted = submitGate_.enterFast(slot);
			if (!admitted) {
				IOLockLock(lock_);
				admitted = submitGate_.enterSlow(slot, lock_);
				IOLockUnlock(lock_);
			}
			job.gateHeld = admitted;
			job.pushesPrepared = false;
			job.carryHostWritten = false;
			result = admitted ? runJob(record, job, out, why)
					  : switch_gpu_refuse(why, SwitchGpuRefusal::Closing,
							      kIOReturnNotOpen);
			if (job.gateHeld) {
				job.gateHeld = false;
				submitGate_.leaveFast(slot, lock_);
			}
		}
		job.events->clear();
	}
	IOLockUnlock(submitLocks_[slot]);
	return result;
}

SwitchGpuRefusal AGXDeviceUserClient::useRefusal(const Bo *bo, const BindingTable::Entry *binding,
						 const TegraGM20BAbiSurfaceUse &use, uint64_t bytes,
						 uint64_t &offset)
{
	if (!bo)
		return use.access & TegraGM20BSurfaceGpuAddress ? SwitchGpuRefusal::UseNoBinding
								: SwitchGpuRefusal::UseNoBo;
	if (!bo->backing)
		return SwitchGpuRefusal::UseNoBacking;
	if (switch_gpu_backing_set(bo->backing))
		return SwitchGpuRefusal::UseSetMember;
	if (bytes && (!binding || !switch_gpu_binding_offset(*binding, use.handle, bytes, offset)))
		return SwitchGpuRefusal::UseRange;
	if ((use.access & TegraGM20BSurfaceWrite) && bo->readOnly)
		return SwitchGpuRefusal::UseReadOnly;
	return SwitchGpuRefusal::None;
}

void AGXDeviceUserClient::leaveSubmitGateForWait(uint32_t slot, JobScratch &job,
						 switch_gpu_resource_set *set)
{
	if (set)
		switch_gpu_set_leave_slot(set, slot);
	if (job.gateHeld) {
		job.gateHeld = false;
		submitGate_.leaveFast(slot, lock_);
	}
}

bool AGXDeviceUserClient::reenterSubmitGate(uint32_t slot, JobScratch &job)
{
	bool admitted = submitGate_.enterFast(slot);
	if (!admitted) {
		IOLockLock(lock_);
		admitted = submitGate_.enterSlow(slot, lock_);
		IOLockUnlock(lock_);
	}
	job.gateHeld = admitted;
	return admitted;
}

uint32_t AGXDeviceUserClient::collectSiblings(const switch_gpu_resource_set *set, uint32_t slot,
					      switch_gpu_sibling_wait *siblings) const
{
	static_assert(kMaxChannels == SWITCH_GPU_SET_SLOTS, "a set slot per channel");
	const uint32_t used = switch_gpu_set_used(set) & ~(1u << slot);
	uint32_t live = 0, completed[kMaxChannels] = {};
	for (uint32_t other = 0; other < kMaxChannels; ++other) {
		if (chans_[other] && !tgm_chan_error(chans_[other])) {
			live |= 1u << other;
			completed[other] = tgm_chan_completed(chans_[other]);
		} else if (!chans_[other] && (used & (1u << other)) &&
			   __atomic_load_n(&retiringChans_[other].active, __ATOMIC_ACQUIRE)) {
			live |= 1u << other;
			completed[other] = __atomic_load_n(
				fence_va_ + static_cast<size_t>(other) * 16, __ATOMIC_ACQUIRE);
		}
	}
	switch_gpu_set_wait waits[SWITCH_GPU_SET_SLOTS];
	const uint32_t count = switch_gpu_set_waits(set, slot, live, completed, waits);
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t other = waits[i].slot;
		siblings[i] = {chans_[other], waits[i].seqno,
			       chans_[other] ? 0u : fence_gpu_ + uint64_t(other) * 64};
	}
	return count;
}

// Inside the submit gate (channels and sets stay as they are) and the channel's submit lock, which keeps every BO and
// binding table it finds published or not yet freed (retireBatch's passSubmitters).
IOReturn AGXDeviceUserClient::runJob(const SwitchGpuJobRecord &record, JobScratch &job,
				     TegraGM20BAbiExecReply *out, SwitchGpuRefusalNote &why)
{
	const TegraGM20BAbiJob &head = *record.job;
	const uint32_t slot = head.channel - 1;
	for (;;) {
		if (!job.gateHeld)
			return switch_gpu_refuse(why, SwitchGpuRefusal::Closing, kIOReturnNotOpen);
		tgm_chan *chan = chans_[slot];
		if (!chan)
			return switch_gpu_refuse(why, SwitchGpuRefusal::NoChannel,
						 kIOReturnBadArgument);
		const uint32_t roleGen = __atomic_load_n(&tgm_uc::appRoleGen, __ATOMIC_ACQUIRE);
		if (roleGen != __atomic_load_n(&roleGen_, __ATOMIC_RELAXED) ||
		    (++job.roleChecks & 63u) == 0u) {
			const bool background =
				clientHasPrivilege(current_task(), kIOClientPrivilegeForeground) !=
				kIOReturnSuccess;
			if (background != backgroundRole_ ||
			    roleGen != __atomic_load_n(&roleGen_, __ATOMIC_RELAXED)) {
				IOLockLock(lock_);
				__atomic_store_n(&roleGen_, roleGen, __ATOMIC_RELEASE);
				if (background != backgroundRole_) {
					backgroundRole_ = background;
					applyLevel();
				}
				IOLockUnlock(lock_);
			}
		}
		if (channelLevels_[slot] != level_)
			syncChannelLevel(slot, chan, false);
		switch_gpu_resource_set *set = head.set ? sets_[head.set - 1] : nullptr;
		if (head.set && !set)
			return switch_gpu_refuse(why, SwitchGpuRefusal::NoSet, kIOReturnBadArgument,
						 head.set);
		if (set && !switch_gpu_set_enter_slot(set, slot, head.set_generation)) {
			switch_gpu_job_stale();
			switch_gpu_path_taken(SWITCH_GPU_PATH_STALE_SET, 1);
			return switch_gpu_refuse(why, SwitchGpuRefusal::StaleSet, kIOReturnNotReady,
						 head.set, head.set_generation);
		}
		job.setHeld = set != nullptr;
		job.roomRetry = false;
		switch_gpu_sibling_wait siblings[SWITCH_GPU_SET_SLOTS];
		const uint32_t siblingCount = set ? collectSiblings(set, slot, siblings) : 0;
		const IOReturn result =
			runJobUses(record, job, out, set, siblings, siblingCount, why);
		if (set && job.setHeld) {
			switch_gpu_set_leave_slot(set, slot);
			job.setHeld = false;
		}
		if (job.roomRetry) {
			job.roomRetry = false;
			if (record.promises && record.promises->count)
				reholdPromises(record.promises);
			const int prepared = job.events->prepare(events_, record.events,
								 record.releases, head.channel);
			if (prepared)
				return switch_gpu_refuse(why, SwitchGpuRefusal::EventPrepare,
							 ioreturn(prepared),
							 switch_gpu_refusal_errno(prepared));
			continue;
		}
		if (result != kIOReturnSuccess && record.promises && record.promises->count)
			reholdPromises(record.promises);
		return result;
	}
}

IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record, JobScratch &job,
					 TegraGM20BAbiExecReply *out, switch_gpu_resource_set *set,
					 switch_gpu_sibling_wait *siblings,
					 uint32_t siblingCount, SwitchGpuRefusalNote &why)
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
		return switch_gpu_refuse(why, SwitchGpuRefusal::AdmissionFlags,
					 kIOReturnBadArgument, head.flags, record.admission,
					 admissionCookies_[slot]);
	static_assert(sizeof(tgm_push) == sizeof(TegraGM20BAbiPush) &&
			      offsetof(tgm_push, no_prefetch) == offsetof(TegraGM20BAbiPush, flags),
		      "push layouts");
	auto *pushes = reinterpret_cast<tgm_push *>(record.pushes);
	if (!job.pushesPrepared) {
		constexpr uint32_t kPushFlags =
			TegraGM20BPushNoPrefetch | TegraGM20BPushIncomplete | TegraGM20BPushFence;
		for (uint32_t i = 0; i < count; ++i) {
			const TegraGM20BAbiPush &p = record.pushes[i];
			if ((p.va & 3) || !p.dwords || p.dwords >= (1u << 21) ||
			    (p.flags & ~kPushFlags) ||
			    ((p.flags & TegraGM20BPushIncomplete) &&
			     (i + 1 == count ||
			      !(record.pushes[i + 1].flags & TegraGM20BPushNoPrefetch))) ||
			    p.va < TGM_USER_VA_START ||
			    p.va + uint64_t(p.dwords) * 4 > TGM_KERNEL_VA)
				return switch_gpu_refuse(why, SwitchGpuRefusal::Push,
							 kIOReturnBadArgument, i, p.va, p.dwords);
			const uint32_t flags = p.flags;
			pushes[i].no_prefetch = (flags & TegraGM20BPushNoPrefetch) ? 1u : 0u;
			pushes[i].incomplete = (flags & TegraGM20BPushIncomplete) ? 1u : 0u;
			pushes[i].fence = (flags & TegraGM20BPushFence) ? 1u : 0u;
			pushes[i].reserved = 0;
		}
		job.pushesPrepared = true;
	}
	uint32_t fences = 0;
	if (switch_gpu_job_fences(pushes, count, &fences))
		return switch_gpu_refuse(why, SwitchGpuRefusal::Fences, kIOReturnBadArgument,
					 count);
#if TGM_STATS
	tgm_prof prof;
	const tgm_prof *profile = scanSubmission(slot, fences, pushes, count, prof, true);
#else
	const tgm_prof *profile = nullptr;
#endif
	uint32_t useTotal = 0, writeCount = 0, surfaceCount = 0, nativeGuardCount = 0,
		 writtenCount = 0, nativeWrittenCount = 0, rangedUses = 0, nativeReads = 0;
	bool hostWritten = job.carryHostWritten || (head.flags & TegraGM20BJobHostWritten) ||
			   (set && switch_gpu_set_take_host_written(set));
	job.carryHostWritten = hostWritten;
	bool unlockedWrites = false, scanoutWrites = false;
	constexpr uint32_t kUseAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
	constexpr uint32_t kUseFlags = kUseAccess | TegraGM20BSurfaceHostWritten |
				       TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange;
	const uint32_t claim = switch_gpu_claim_flags(admissionBegin || admissionContinue);
	// A refused job's writes never come: until each surface's next write, its frame is not the one asked of it
	// (switch_gpu_backing_write_refused). Not a busy one: a display lease on the surface's current frame refuses it.
	const auto refuseWrites = [&](int32_t ret) {
		switch_gpu_backing_writes_refused(job.uses, useTotal);
		return ret;
	};
	const uint64_t chanUid = tgm_chan_uid(chan);
	const uint64_t snapBindGen = __atomic_load_n(&bindGen_, __ATOMIC_ACQUIRE);
	const uint64_t snapBoGen = __atomic_load_n(&boGen_, __ATOMIC_ACQUIRE);
	const bool sameUses = (head.flags & TegraGM20BJobSameUses) != 0;
	bool usesHostWritten = false;
	if (sameUses) {
		if (!job.useCache.matches(chanUid, snapBindGen, snapBoGen, claim, head.set)) {
			job.useCache.invalidate();
			return switch_gpu_refuse(why, SwitchGpuRefusal::StaleUses,
						 kIOReturnNotReady, snapBindGen, snapBoGen);
		}
		job.useCache.restore(job.uses, useTotal, job.writes, writeCount, job.surfaces,
				     surfaceCount, job.written, writtenCount, rangedUses,
				     unlockedWrites, hostWritten);
		for (uint32_t i = 0; i < writeCount; ++i)
			scanoutWrites |= switch_gpu_backing_scanned(job.writes[i].backing) != 0;
	} else {
		job.useCache.invalidate();
		for (uint32_t i = 0; i < head.shared_count; ++i) {
			const TegraGM20BAbiSurfaceUse &use = record.uses[i];
			const bool addressed = use.access & TegraGM20BSurfaceGpuAddress;
			const uint64_t bytes =
				use.access & TegraGM20BSurfaceRange ? use.reserved : 0;
			if (!(use.access & kUseAccess) || (use.access & ~kUseFlags) ||
			    (!bytes && use.reserved))
				return refuseWrites(switch_gpu_refuse(
					why, SwitchGpuRefusal::UseAccess, kIOReturnBadArgument, i,
					use.access, use.reserved));
			rangedUses += bytes != 0;
			const BindingTable::Entry *binding =
				addressed ? bindTable_.find(use.handle, bytes ? bytes : 1)
					  : nullptr;
			const Bo *bo =
				addressed ? (binding ? binding->bo : nullptr) : findBo(use.handle);
			uint64_t offset = 0;
			if (const SwitchGpuRefusal refused =
				    useRefusal(bo, binding, use, bytes, offset);
			    refused != SwitchGpuRefusal::None)
				return refuseWrites(switch_gpu_refuse(
					why, refused, kIOReturnBadArgument, i, use.handle,
					refused == SwitchGpuRefusal::UseReadOnly ? use.access
										 : bytes));
			usesHostWritten |= (use.access & TegraGM20BSurfaceHostWritten) != 0;
			hostWritten |= usesHostWritten;
			const uint32_t access = use.access & kUseAccess;
			if (bo->nativeView) {
				++nativeReads;
				const uint32_t first = useTotal;
				if (const int error = switch_gpu_native_surface_add_use(
					    *bo->nativeView, bo->readOnly, access, job.uses,
					    job.guards, job.capacity, useTotal, nativeGuardCount))
					return refuseWrites(switch_gpu_refuse(
						why, SwitchGpuRefusal::NativeRead, ioreturn(error),
						i, switch_gpu_refusal_errno(error)));
				for (uint32_t u = first; u < useTotal; ++u)
					job.uses[u].access |=
						switch_gpu_claim_use(claim, job.uses[u].access);
				if (access & TegraGM20BSurfaceWrite) {
					switch_gpu_backing *native = bo->nativeView->native;
					job.writes[writeCount++] = {native, 0, 0};
					scanoutWrites |= switch_gpu_backing_scanned(native) != 0;
					unlockedWrites = true;
					if (bo->nativeView->owner)
						job.nativeWritten[nativeWrittenCount++] =
							bo->nativeView->owner;
				}
				continue;
			}
			job.uses[useTotal++] = {bo->backing,
						access | switch_gpu_claim_use(claim, access), 0};
			if (access & TegraGM20BSurfaceWrite) {
				job.writes[writeCount++] = {bo->backing,
							    bo->representation ? 0 : offset,
							    bo->representation ? 0 : bytes};
				unlockedWrites |= bo->unlockedReads;
				scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;
				job.written[writtenCount++] = bo->coh;
			}
			if (bo->representation)
				job.surfaces[surfaceCount++] = {
					bo->backing,
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
	}
	const uint32_t guardCount = surfaceCount + nativeGuardCount;
	[[maybe_unused]] uint32_t backings = 0;
	if (admissionContinue && !admission.covers(job.uses, useTotal))
		return refuseWrites(switch_gpu_refuse(why, SwitchGpuRefusal::AdmissionCover,
						      kIOReturnBadArgument, useTotal));
	if (!sameUses && !nativeReads) {
		job.useCache.save(chanUid, snapBindGen, snapBoGen, claim, head.set, job.uses,
				  useTotal, job.writes, writeCount, job.surfaces, surfaceCount,
				  job.written, writtenCount, rangedUses, unlockedWrites,
				  usesHostWritten);
#if TGM_STATS || SWITCH_GPU_PATH_STATS
		job.useCache.named = head.shared_count;
#endif
	}
#if TGM_STATS || SWITCH_GPU_PATH_STATS
	const uint32_t sharedNamed = sameUses ? job.useCache.named : head.shared_count;
#else
	const uint32_t sharedNamed = 0;
	(void)sharedNamed;
#endif
	tgm_views_job_writes(job.written, writtenCount);
	job.phases.end(TGM_SUBMIT_RESOLVE);
	SwitchGpuEventJob *events = job.events;
	// the job's event waits, then the pack image's word while the image streams in (its programs are read from the
	// window, which fills as the stream lands)
	switch_gpu_word_wait words[TegraGM20BMaxJobEvents + 1];
	uint32_t wordCount = 0;
	uint32_t packPool = 0, packChunks = 0;
	const int packWait = __atomic_load_n(&packClient_.gpuMapping, __ATOMIC_ACQUIRE)
				     ? switch_gpu_pack_job_wait(&packPool, &packChunks)
				     : 0;
	if (packWait < 0)
		return refuseWrites(
			switch_gpu_refuse(why, SwitchGpuRefusal::PackImage, kIOReturnAborted));
	int error = events->sequence(eventChannels_[slot]);
	if (error)
		return refuseWrites(switch_gpu_refuse(why, SwitchGpuRefusal::EventSequence,
						      ioreturn(error),
						      switch_gpu_refusal_errno(error)));
	// an event's waiters are woken by the interrupt at its signal; the job's semantic fence and scanout writes are
	// known below (switch_gpu_quiet_end)
	const bool quiet =
		(head.flags & TegraGM20BJobQuiet) && !(head.flags & TegraGM20BJobHasEvents);
	const bool closesGroup =
		!(head.flags & TegraGM20BJobAdmission) || (head.flags & TegraGM20BJobAdmissionEnd);
	switch_gpu_release releases[TegraGM20BMaxJobPromises] = {};
	const uint32_t keptCount = record.promises ? record.promises->count : 0;
	// the job's fence releases them: a job with no pushes and no events has none
	const bool fenced = count || (head.flags & TegraGM20BJobHasEvents);
	const IOReturn kept =
		keptCount && fenced ? keepPromises(record.promises, releases) : kIOReturnSuccess;
	if (keptCount && (!fenced || kept))
		return refuseWrites(switch_gpu_refuse(
			why, SwitchGpuRefusal::Promises,
			kept == kIOReturnTimeout ? kIOReturnTimeout : kIOReturnBadArgument,
			keptCount, count));
	switch_gpu_backing_transaction transaction = {};
	switch_gpu_backing_token completion = {};
	uint32_t seqno = 0, dependencies = 0, jobVisibility = 0, leaseCount = 0;
	const uint64_t prevAdmissionCookie = lastAdmissionCookies_[slot];
	bool ownershipBusy = false, ownershipFailed = false, semanticBound = false,
	     channelGone = false;
	SwitchGpuRefusal stage = SwitchGpuRefusal::Refresh;
refresh:
	stage = SwitchGpuRefusal::Refresh;
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
		stage = SwitchGpuRefusal::AdmissionBegin;
		error = admission.begin(job.uses, useTotal, job.guards, guardCount, job.leases,
					leaseCount);
		if (!error) {
			admissionCookies_[slot] = record.admission;
			lastAdmissionCookies_[slot] = record.admission;
		}
	}
	job.phases.end(TGM_SUBMIT_REFRESH);
	ownershipBusy = error == -16;
	ownershipFailed = error == -5;
	switch_gpu_chan_submit_enter(chan);
	job.phases.end(TGM_SUBMIT_ENTER);
	for (; !error;) {
		dependencies = 0;
		const bool runs = count || (head.flags & TegraGM20BJobHasEvents);
		if (runs)
			switch_gpu_chan_job_token(chan, fences, &completion);
		if (count) {
			stage = SwitchGpuRefusal::Backings;
			error = switch_gpu_backing_begin_held(job.uses, useTotal, job.leases,
							      leaseCount, job.guards, guardCount,
							      &completion, &transaction);
			backings = transaction.count;
			job.phases.end(TGM_SUBMIT_BEGIN);
			ownershipBusy = error == -16;
			if (!error) {
				stage = SwitchGpuRefusal::WriteRanges;
				error = switch_gpu_backing_write_ranges(&transaction, job.writes,
									writeCount);
			}
			if (!error) {
				stage = SwitchGpuRefusal::Dependencies;
				error = switch_gpu_backing_dependencies(
					&transaction, job.dependencies, TegraGM20BMaxJobUses,
					&dependencies);
			}
		}
		if (!error) {
			stage = SwitchGpuRefusal::EventAppend;
			error = events->append(job.dependencies, TegraGM20BMaxJobDependencies,
					       &dependencies);
		}
		if (!error) {
			const int captured =
				events->captureDependencies(job.dependencies, dependencies);
			if (captured > 0) {
				switch_gpu_backing_abort(&transaction);
				stage = SwitchGpuRefusal::EventInherit;
				error = events->inheritDependencies();
				ownershipFailed = error == -5;
				if (error)
					break;
				job.phases.end(TGM_SUBMIT_LOCK);
				continue;
			}
			error = captured;
		}
		// another connection's writer promises on what the job uses: acquired on the GPU, never a shared event's
		// captured dependency; read under the transaction's locks on the uses' backings, never by taking one again
		if (!error && count) {
			stage = SwitchGpuRefusal::PromiseDependencies;
			const uint32_t ownPromises = tgm_fence_offset(promiseWord(0));
			error = switch_gpu_backing_promise_dependencies(
				&transaction, job.dependencies, TegraGM20BMaxJobDependencies,
				&dependencies, ownPromises,
				ownPromises + TegraGM20BMaxPromises * 4);
		}
		if (!error) {
			stage = SwitchGpuRefusal::EventBind;
			error = events->bind(&completion);
		}
		ownershipFailed = error == -5;
		if (!error) {
			wordCount = events->wordCount();
			memcpy(words, events->words(), wordCount * sizeof(words[0]));
			if (packWait > 0)
				words[wordCount++] = {packPool, packChunks, 0};
		}
		if (!error &&
		    dependencies + siblingCount + wordCount > TegraGM20BMaxJobDependencies) {
			stage = SwitchGpuRefusal::DependencyRoom;
			error = -28;
		}
		if (!error) {
			stage = SwitchGpuRefusal::Exec;
			job.phases.end(TGM_SUBMIT_LOCK);
			semanticBound = events->semanticFence();
			const bool quietEnd = switch_gpu_quiet_end(quiet, semanticBound,
								   scanoutWrites, closesGroup);
			jobVisibility =
				switch_gpu_job_visibility(
					head.flags, events->signals(),
					unlockedWrites || scanoutWrites ||
						(writeCount && switch_gpu_chan_has_syncpt(chan)),
					hostWritten || transaction.host_written) |
				(quietEnd ? uint32_t(SWITCH_GPU_SUBMIT_QUIET) : 0u);
			if (events->released())
				switch_gpu_chan_event_releases(chan, uint32_t(completion.value));
			error = switch_gpu_chan_exec_events_validated(
				chan, pushes, count, fences, job.dependencies, dependencies,
				siblings, siblingCount, words, wordCount, &seqno, profile,
				jobVisibility, releases, keptCount, &transaction);
			job.phases.end(TGM_SUBMIT_EXEC);
		}
		if (error != -11)
			break;
		switch_gpu_backing_abort(&transaction);
		stage = SwitchGpuRefusal::Room;
		// Release submitGate_ (and set) across the bounded GPFIFO room sleep so unrelated
		// mutations in the process are not stalled; submitLocks_[slot] keeps chan alive
		// against retireChannels().
		leaveSubmitGateForWait(slot, job, job.setHeld ? set : nullptr);
		job.setHeld = false;
		error = switch_gpu_chan_wait_submission_room(
			chan, pushes, count, dependencies + siblingCount + wordCount, profile);
		job.phases.end(TGM_SUBMIT_ROOM);
		if (!reenterSubmitGate(slot, job)) {
			useTotal = 0;
			error = -19;
			break;
		}
		const bool gensChanged =
			__atomic_load_n(&boGen_, __ATOMIC_ACQUIRE) != snapBoGen ||
			__atomic_load_n(&bindGen_, __ATOMIC_ACQUIRE) != snapBindGen;
		if (gensChanged) {
			job.useCache.invalidate();
			useTotal = 0;
		}
		if (chans_[slot] != chan) {
			channelGone = true;
			error = -19;
			break;
		}
		if (error)
			break;
		if (set) {
			if (sets_[head.set - 1] != set ||
			    !switch_gpu_set_enter_slot(set, slot, head.set_generation)) {
				job.roomRetry = true;
				error = -11;
				break;
			}
			job.setHeld = true;
		}
		if (gensChanged) {
			job.roomRetry = true;
			error = -11;
			break;
		}
		if (set)
			siblingCount = collectSiblings(set, slot, siblings);
	}
	if (error)
		switch_gpu_backing_abort(&transaction);
	if (keptCount && !error)
		keptPromises(record.promises, slot, completion);
	if (!error) {
		events->commit();
		if (set && (count || (head.flags & TegraGM20BJobHasEvents)))
			switch_gpu_set_note_job(set, slot, &completion);
		if (count && switch_native_surface_gpu_written) {
			for (uint32_t i = 0; i < nativeWrittenCount; ++i)
				switch_native_surface_gpu_written(job.nativeWritten[i]);
		}
	}
	switch_gpu_chan_submit_leave();
	SwitchGpuSurfaceRefresh::release(job.surfaces, surfaceCount);
	if (job.roomRetry) {
		if (admissionBegin && admission.active() &&
		    admissionCookies_[slot] == record.admission) {
			admission.end();
			admissionCookies_[slot] = 0;
			lastAdmissionCookies_[slot] = prevAdmissionCookie;
		}
		constexpr IOReturn kRoomRetry = kIOReturnSuccess;
		return kRoomRetry;
	}
	if (error == SWITCH_GPU_BACKING_STALE && !nativeGuardCount && !admission.active())
		goto refresh;
	if (admission.active() && admissionCookies_[slot] == record.admission &&
	    (error || (head.flags & TegraGM20BJobAdmissionEnd))) {
		admission.end();
		admissionCookies_[slot] = 0;
	}
	if (!job.gateHeld)
		return refuseWrites(
			switch_gpu_refuse(why, SwitchGpuRefusal::Closing, kIOReturnNotOpen));
	if (channelGone)
		return refuseWrites(switch_gpu_refuse(why, SwitchGpuRefusal::NoChannel,
						      kIOReturnNotResponding));
	if (!ownershipFailed && ownershipBusy)
		return switch_gpu_refuse(why, SwitchGpuRefusal::Busy, kIOReturnCannotLock);
	if (ownershipFailed || error) {
		const IOReturn ret = ownershipFailed ? kIOReturnAborted : ioreturn(error);
		if (stage == SwitchGpuRefusal::DependencyRoom)
			return refuseWrites(switch_gpu_refuse(
				why, stage, ret, dependencies + siblingCount + wordCount,
				TegraGM20BMaxJobDependencies));
		const uint64_t counted =
			stage == SwitchGpuRefusal::Refresh       ? surfaceCount
			: stage == SwitchGpuRefusal::WriteRanges ? writeCount
			: stage == SwitchGpuRefusal::EventAppend ||
					stage == SwitchGpuRefusal::PromiseDependencies
				? dependencies
			: stage == SwitchGpuRefusal::Exec || stage == SwitchGpuRefusal::Room
				? count
				: useTotal;
		return refuseWrites(switch_gpu_refuse(
			why, stage, ret, switch_gpu_refusal_errno(error), counted, dependencies));
	}
	if (count) {
		switch_gpu_path_job(jobVisibility, unlockedWrites, leanChannels_[slot], rangedUses);
		switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_READ, nativeReads);
		for (uint32_t i = 0; i < writtenCount; ++i)
			tgm_coherency_wrote(job.written[i], tgm_chan_uid(chan), seqno);
	}
	job.phases.end(TGM_SUBMIT_COMMIT);
#if TGM_STATS
	switch_gpu_job_submitted(job.phases.ns, set != nullptr, sharedNamed, backings, siblingCount,
				 critical_);
#endif
	switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, set ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, sharedNamed);
	switch_gpu_path_taken(SWITCH_GPU_PATH_QUIET_JOB,
			      (jobVisibility & SWITCH_GPU_SUBMIT_QUIET) && count ? 1 : 0);
#if TGM_STATS
	if (count)
		tgm_stats_loud(chan,
			       switch_gpu_quiet_why((head.flags & TegraGM20BJobQuiet) != 0,
						    (head.flags & TegraGM20BJobHasEvents) != 0,
						    semanticBound, scanoutWrites, closesGroup));
#endif
	out->seqno = seqno;
	out->flags = count && (head.flags & TegraGM20BJobQuiet) &&
				     !(jobVisibility & SWITCH_GPU_SUBMIT_QUIET)
			     ? uint32_t(TegraGM20BExecReplyLoud)
			     : 0u;
	return kIOReturnSuccess;
}
