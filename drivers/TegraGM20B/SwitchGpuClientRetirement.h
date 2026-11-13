// SPDX-License-Identifier: MIT
#pragma once

void AGXDeviceUserClient::accountRelease(Bo *bo)
{
	if (bo->handle && (bo->pool_va || bo->chunked))
		pool_bytes_ -= bo->bytes;
	if (bo->handle && bo->pool_va && (bo->flags & TegraGM20BBoControl))
		control_bytes_ -= bo->controlCharge;
}

void AGXDeviceUserClient::readyBo(Bo *bo)
{
	if (!bo->zombieLink || bo->bindings)
		return;
	*bo->zombieLink = bo->next;
	if (bo->next)
		bo->next->zombieLink = bo->zombieLink;
	bo->zombieLink = nullptr;
	accountRelease(bo);
	bo->next = readyBos_;
	readyBos_ = bo;
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::queueZombie(Bo *bo)
{
	bo->next = zombies_;
	bo->zombieLink = &zombies_;
	if (zombies_)
		zombies_->zombieLink = &bo->next;
	zombies_ = bo;
	readyBo(bo);
}

void AGXDeviceUserClient::scheduleRetirement()
{
	if (!__atomic_load_n(&retirementPending_, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		return;
	__atomic_add_fetch(&retirementEpoch_, 1u, __ATOMIC_RELEASE);
	thread_call_enter(retireCall_);
}

// A kept writer promise is released by its job's end or, after a kill of its channel, by retirement.
void AGXDeviceUserClient::retirementFence(void *owner)
{
	auto *client = static_cast<AGXDeviceUserClient *>(owner);
	if (__atomic_load_n(&client->promisesKept_, __ATOMIC_RELAXED))
		__atomic_store_n(&client->retirementPending_, 1u, __ATOMIC_RELEASE);
	client->scheduleRetirement();
}

void AGXDeviceUserClient::retirementWork(thread_call_param_t owner, thread_call_param_t)
{
	static_cast<AGXDeviceUserClient *>(owner)->retireBatch();
}

bool AGXDeviceUserClient::bindingIdle(const RetiredBinding *binding) const
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		const auto &snapshot = binding->snapshots[slot];
		if (!snapshot.engine || !chans_[slot])
			continue;
		switch_gpu_backing_token current = {};
		switch_gpu_chan_next_token(chans_[slot], &current);
		if (snapshot.queue == current.queue &&
		    snapshot.reset_generation == current.reset_generation &&
		    !tgm_chan_error(chans_[slot]) &&
		    int32_t(tgm_chan_completed(chans_[slot]) - uint32_t(snapshot.value)) < 0)
			return false;
	}
	return true;
}

void AGXDeviceUserClient::retireBatch()
{
	constexpr uint32_t batch = 64;
	IOLockLock(lock_);
	if (__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE)) {
		IOLockUnlock(lock_);
		return;
	}
	if (!retireRemaining_) {
		retireRemaining_ = retiredCount_;
		retireScanEpoch_ = __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	}
	for (uint32_t checked = 0; retired_ && retireRemaining_ && checked < batch; ++checked) {
		auto *record = retired_;
		retired_ = record->next;
		if (!retired_)
			retiredTail_ = &retired_;
		--retireRemaining_;
		if (bindingIdle(record)) {
			--retiredCount_;
			record->next = unmapping_;
			unmapping_ = record;
		} else {
			record->next = nullptr;
			*retiredTail_ = record;
			retiredTail_ = &record->next;
		}
	}
	auto *ready = unmapping_;
	logFaultBindings();
	releasePromises(false);
	IOLockUnlock(lock_);
	IOLockLock(unmapLock_);
	for (auto *record = ready; record; record = record->next)
		switch_gpu_vm_unmap(vm_, &record->mapping);
	IOLockUnlock(unmapLock_);
	IOLockLock(lock_);
	unmapping_ = nullptr;
	while (ready) {
		auto *next = ready->next;
		noteBind(TGM_BIND_GONE, ready->va, ready->range, ready->bo, 0, ready->kind);
		--ready->bo->bindings;
		readyBo(ready->bo);
		IODelete(ready, RetiredBinding, 1);
		ready = next;
	}
	installPendingBinds();
	Bo *release = nullptr;
	for (uint32_t count = 0; readyBos_ && count < batch; ++count) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		bo->next = release;
		release = bo;
	}
	__atomic_store_n(&retirementPending_, unsigned(retired_ || readyBos_), __ATOMIC_RELEASE);
	const bool again = readyBos_ || retireRemaining_ ||
			   retireScanEpoch_ != __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	if (again && !__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		thread_call_enter(retireCall_);
	IOLockWakeup(lock_, &retired_, false);
	IOLockUnlock(lock_);
	while (release) {
		auto *next = release->next;
		releaseBo(release);
		release = next;
	}
}

// lock_ held. The PTEs of retired bindings in [va, va + bytes) go now, so a new mapping can take the range; their pages
// stay until the GPU work that may use them completes. A record retireBatch is unmapping is waited for (unmapLock_), never
// the GPU.
void AGXDeviceUserClient::takeRetiredOverlaps(uint64_t va, uint64_t bytes)
{
	const auto overlaps = [=](const RetiredBinding *record) {
		uint64_t from = 0, span = 0;
		return !switch_gpu_mapping_span(record->mapping, &from, &span) &&
		       from < va + bytes && va < from + span;
	};
	RetiredBinding *const lists[] = {retired_, unmapping_};
	IOLockLock(unmapLock_);
	for (auto *list : lists)
		for (auto *record = list; record; record = record->next)
			if (record->mapping && overlaps(record)) {
				switch_gpu_vm_unmap(vm_, &record->mapping);
				noteBind(TGM_BIND_GONE, record->va, record->range, record->bo, 0,
					 record->kind);
			}
	IOLockUnlock(unmapLock_);
}

void AGXDeviceUserClient::retire(RetiredBinding *record)
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		record->snapshots[slot] = {};
		if (chans_[slot]) {
			switch_gpu_chan_next_token(chans_[slot], &record->snapshots[slot]);
			record->snapshots[slot].value = uint32_t(record->snapshots[slot].value - 1);
			// its retirement (and the bind epoch behind it) waits for that seqno's interrupt
			tgm_chan_need(chans_[slot], uint32_t(record->snapshots[slot].value));
		}
	}
	record->next = nullptr;
	*retiredTail_ = record;
	retiredTail_ = &record->next;
	++retiredCount_;
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
}

// lock_ held; spare: two records, taken as a binding cut at an edge needs one for what stays on each side, and room for
// two more bindings. [va, va + range) leaves every binding: what a binding keeps below and above it stays bound with its
// PTEs, and its part in the range retires after the GPU work that may use it (now: no such work is queued, so its PTEs
// go at once). What a cut 128 KiB page keeps outside the range is installed once that page's PTE is gone; while it waits
// the connection's submissions wait for the bind epoch. replace: a Map takes the range. On an error before any binding
// changed, that binding and the ones after it are untouched.
IOReturn AGXDeviceUserClient::cutOverlaps(uint64_t va, uint64_t range, bool now, bool replace,
					  RetiredBinding **spare)
{
	const uint64_t end = va + range;
	for (uint32_t i = 0; i < nbinds_;) {
		const Binding b = binds_[i];
		if (b.va >= end || va >= b.va + b.range) {
			++i;
			continue;
		}
		auto *record = b.retirement;
		const bool wasPending = switch_gpu_mapping_pending(record->mapping);
		switch_gpu_mapping *below = nullptr, *taken = record->mapping, *above = nullptr;
		if (b.va < va || b.va + b.range > end) {
			const int error = switch_gpu_vm_split(
				vm_, &record->mapping, b.bo->mem, record->kind, int(b.bo->readOnly),
				record->vol, va, range, &below, &taken, &above);
			if (error)
				return ioreturn(error);
		}
		--nbinds_;
		memmove(&binds_[i], &binds_[i + 1], sizeof(Binding) * (nbinds_ - i));
		const auto keep = [&](switch_gpu_mapping *part) -> RetiredBinding * {
			uint64_t from = 0, bytes = 0;
			if (switch_gpu_mapping_span(part, &from, &bytes))
				return nullptr;
			auto *kept = *spare;
			*spare++ = nullptr;
			*kept = {nullptr, from,         bytes,       b.bo, part,
				 {},      record->kind, record->vol, 0,    false};
			++b.bo->bindings;
			const uint32_t at = switch_gpu_binding_upper(binds_, nbinds_, from);
			memmove(&binds_[at + 1], &binds_[at], sizeof(Binding) * (nbinds_ - at));
			binds_[at] = {from, bytes, b.bo, b.bo_offset + (from - b.va), kept};
			++nbinds_;
			i = at + 1;
			return kept;
		};
		RetiredBinding *const kept[] = {keep(below), keep(above)};
		record->va = b.va > va ? b.va : va;
		record->range = (b.va + b.range < end ? b.va + b.range : end) - record->va;
		noteBind(replace ? TGM_BIND_REPLACED : TGM_BIND_UNMAP, record->va, record->range,
			 b.bo, b.bo_offset + (record->va - b.va), record->kind);
		record->mapping = taken;
		// The snapshot of the work that may still use the cut PTEs is taken before a bind epoch for what they hold
		// up is published: a submission that reads the new epoch is then never in it, so never waits on itself.
		retire(record);
		if (now) {
			switch_gpu_vm_unmap(vm_, &record->mapping);
			switch_gpu_mapping *const parts[] = {below, above};
			for (auto *part : parts)
				if (switch_gpu_mapping_pending(part))
					switch_gpu_vm_install(vm_, part, b.bo->mem, record->kind,
							      int(b.bo->readOnly), record->vol);
		} else if (switch_gpu_mapping_pending(below) || switch_gpu_mapping_pending(above)) {
			const uint32_t epoch = issueBind();
			for (auto *part : kept)
				if (part && switch_gpu_mapping_pending(part->mapping))
					part->epoch = wasPending ? record->epoch : epoch;
		}
	}
	scheduleRetirement();
	return kIOReturnSuccess;
}

// lock_ held; [op.va, op.va + op.range) is free of bindings. The new binding's PTEs go in now, or with queued, in
// retirement after the work queued before this Map completes, its submissions from now on waiting for the bind epoch.
// record is consumed either way.
IOReturn AGXDeviceUserClient::mapRange(const TegraGM20BAbiBindOp &op, Bo *bo,
				       RetiredBinding *record, bool queued)
{
	const uint8_t kind = uint8_t(op.kind), vol = op.op == TegraGM20BBindMapVolatile;
	switch_gpu_mapping *mapping = nullptr;
	if (!queued)
		takeRetiredOverlaps(op.va, op.range);
	const int err =
		queued ? switch_gpu_vm_prepare(vm_, op.va, op.range, bo->mem, op.bo_offset, kind,
					       &mapping)
		       : switch_gpu_vm_map_volatile(vm_, op.va, op.range, bo->mem, op.bo_offset,
						    kind, int(bo->readOnly), vol, &mapping);
	noteBind(err ? TGM_BIND_REFUSED : TGM_BIND_MAP, op.va, op.range, bo, op.bo_offset, kind);
	if (err) {
		IODelete(record, RetiredBinding, 1);
		return ioreturn(err);
	}
	*record = {nullptr, op.va, op.range, bo, mapping, {}, kind, vol, 0, false};
	++bo->bindings;
	const uint32_t at = switch_gpu_binding_upper(binds_, nbinds_, op.va);
	memmove(&binds_[at + 1], &binds_[at], sizeof(Binding) * (nbinds_ - at));
	binds_[at] = {op.va, op.range, bo, op.bo_offset, record};
	++nbinds_;
	if (queued) {
		record->epoch = issueBind();
		scheduleRetirement();
	}
	return kIOReturnSuccess;
}

// lock_ held. Whether [va, va + range) has PTEs, live or retiring, that work queued now could still use (a killed or
// replaced channel's work counts as done, as bindingIdle counts it). A range nothing held maps at once.
bool AGXDeviceUserClient::workQueued(uint64_t va, uint64_t range) const
{
	bool held = false;
	for (uint32_t i = 0; i < nbinds_ && !held; ++i)
		held = binds_[i].va < va + range && va < binds_[i].va + binds_[i].range;
	const RetiredBinding *const lists[] = {retired_, unmapping_};
	for (const auto *list : lists)
		for (const auto *record = list; record && !held; record = record->next) {
			uint64_t from = 0, bytes = 0;
			held = switch_gpu_mapping_installed(record->mapping) &&
			       !switch_gpu_mapping_span(record->mapping, &from, &bytes) &&
			       from < va + range && va < from + bytes;
		}
	if (!held)
		return false;
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot)
		if (chans_[slot] && !tgm_chan_error(chans_[slot]) &&
		    int32_t(tgm_chan_completed(chans_[slot]) - tgm_chan_submitted(chans_[slot])) <
			    0)
			return true;
	return false;
}

// lock_ held. A bind whose PTEs wait for retirement: this connection's submissions from now on acquire, on the GPU, the
// bind epoch it returns, which installPendingBinds releases once every pending PTE of that epoch and the ones before it
// is in place.
uint32_t AGXDeviceUserClient::issueBind()
{
	++bindIssued_;
	tgm_vm_bind_wait(vm_, fence_gpu_ + kBindEpochWord * 4, fence_va_ + kBindEpochWord,
			 bindIssued_);
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
	return bindIssued_;
}

// lock_ held. Pending PTEs go in once no retired binding over their range keeps PTEs (the work before the bind is done:
// retireBatch removed them). The bind epoch moves to the one before the oldest that still waits. It never waits for a
// later epoch: a binding issued after a submission read its epoch can be held up by that very submission (its BO freed
// and the VA mapped again while it is queued), and the submission's acquire would then never pass.
void AGXDeviceUserClient::installPendingBinds()
{
	if (bindDone_ == bindIssued_)
		return;
	uint32_t done = bindIssued_;
	for (uint32_t i = 0; i < nbinds_; ++i) {
		const Binding &b = binds_[i];
		auto *mapping = b.retirement->mapping;
		if (!switch_gpu_mapping_pending(mapping))
			continue;
		// by its PTEs' span: a cut 128 KiB page's PTE reaches past the record's own range
		const auto holds = [&](const RetiredBinding *record) {
			uint64_t from = 0, bytes = 0;
			return switch_gpu_mapping_installed(record->mapping) &&
			       !switch_gpu_mapping_span(record->mapping, &from, &bytes) &&
			       from < b.va + b.range && b.va < from + bytes;
		};
		bool held = false;
		const RetiredBinding *const lists[] = {retired_, unmapping_};
		for (const auto *list : lists)
			for (const auto *record = list; record && !held; record = record->next)
				held = holds(record);
		if (held) {
			if (int32_t(b.retirement->epoch - 1 - done) < 0)
				done = b.retirement->epoch - 1;
			continue;
		}
		// A failed install (no page tables, or no comptag lines for a compressed kind) keeps its epoch back: work
		// waiting for it would otherwise run on a VA with no PTEs and fault in a process told the bind succeeded.
		// Retirements free page tables and comptag lines, so each one tries the install again; a bind that never maps
		// leaves its waiter on the GPU acquire, which the hang rule kills with a report.
		if (!switch_gpu_vm_install(vm_, mapping, b.bo->mem, b.retirement->kind,
					   int(b.bo->readOnly), b.retirement->vol))
			continue;
		if (!b.retirement->unmapped)
			IOLog("tegra-gm20b: %s: bind %#llx+%#llx could not be mapped yet; the work "
			      "after it waits\n",
			      owner_, b.va, b.range);
		b.retirement->unmapped = true;
		if (int32_t(b.retirement->epoch - 1 - done) < 0)
			done = b.retirement->epoch - 1;
	}
	if (int32_t(done - bindDone_) <= 0)
		return;
	bindDone_ = done;
	__atomic_store_n(fence_va_ + kBindEpochWord, bindDone_, __ATOMIC_RELEASE);
	__asm__ volatile("dsb sy" ::: "memory");
}

void AGXDeviceUserClient::reap(bool force)
{
	if (!force)
		return;
	while (retired_) {
		auto *record = retired_;
		retired_ = record->next;
		switch_gpu_vm_unmap(vm_, &record->mapping);
		--record->bo->bindings;
		readyBo(record->bo);
		IODelete(record, RetiredBinding, 1);
	}
	retiredTail_ = &retired_;
	retiredCount_ = retireRemaining_ = 0;
	while (readyBos_) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		releaseBo(bo);
	}
	__atomic_store_n(&retirementPending_, 0u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::drainResources()
{
	IOLockLock(lock_);
	submitGate_.close(lock_);
	__atomic_store_n(&retirementStopped_, 1u, __ATOMIC_RELEASE);
	IOLockUnlock(lock_);
	events_.stop();
	resources_.stop();
	releaseMappings();
#if TGM_STATS
	tgm_readback_closed(this);
#endif
	IOLockLock(lock_);
	releaseCpuLeases();
	releasePromises(true);
	hangHolder_ = current_thread();
	for (auto &chan : chans_)
		tgm_chan_del(&chan);
	destroySets();
	for (auto &channel : eventChannels_)
		channel.reset();
	for (auto &admission : admissions_)
		admission.end();
	bzero(admissionCookies_, sizeof(admissionCookies_));
	hangHolder_ = nullptr;
	IOLockUnlock(lock_);
	// Channel destruction drains the callbacks before their preallocated work is cancelled.
	if (retireCall_)
		thread_call_cancel_wait(retireCall_);
	if (wakeCall_)
		thread_call_cancel_wait(wakeCall_);
	IOLockLock(lock_);
	while (nbinds_)
		unbindOverlaps(binds_[0].va, binds_[0].range, nullptr);
	surfaces_.clear();
	boIndex_.clear();
	while (bos_) {
		auto *bo = bos_;
		bos_ = bo->next;
		queueZombie(bo);
	}
	reap(true);
	IOLockUnlock(lock_);
	scheduleOrphanRelease();
}

IOReturn AGXDeviceUserClient::boFree(const TegraGM20BAbiBoFree *in)
{
	const uint64_t start = nowNs();
	Bo **p = &bos_;
	while (*p && (*p)->handle != in->handle)
		p = &(*p)->next;
	if (!*p)
		return kIOReturnBadArgument;
	Bo *bo = *p;
	*p = bo->next;
	switch_gpu_backing_member_freed(bo->backing);
	boIndex_.erase(bo->handle);
	surfaces_.remove(bo->handle);
	for (uint32_t i = 0; i < nbinds_;) {
		if (binds_[i].bo == bo) {
			unbindOverlaps(binds_[i].va, binds_[i].range, bo);
		} else {
			i++;
		}
	}
	queueZombie(bo);
	scheduleRetirement();
	tgm_stats_bo_free(nowNs() - start);
	return kIOReturnSuccess;
}

// Whole binds of `only` (BoFree) or every bind (close) in the range retire.
void AGXDeviceUserClient::unbindOverlaps(uint64_t va, uint64_t range, Bo *only)
{
	for (uint32_t i = 0; i < nbinds_;) {
		Binding &b = binds_[i];
		if (b.va < va + range && va < b.va + b.range && (!only || b.bo == only)) {
			if (only)
				noteBind(TGM_BIND_FREED, b.va, b.range, b.bo, b.bo_offset,
					 b.retirement->kind);
			retire(b.retirement);
			--nbinds_;
			memmove(&binds_[i], &binds_[i + 1], sizeof(Binding) * (nbinds_ - i));
		} else {
			i++;
		}
	}
	scheduleRetirement();
}

// lock_ held. A channel killed for an MMU fault gets one line naming the binding over its fault address, after the kill's
// own line (that runs with interrupts off and cannot take lock_ to read the binds); debug kernels add what was around it.
void AGXDeviceUserClient::logFaultBindings()
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		if (!chans_[slot] || faultLogged_[slot] || !tgm_chan_error(chans_[slot]))
			continue;
		faultLogged_[slot] = true;
		const uint64_t fault = tgm_chan_fault_va(chans_[slot]);
		if (!fault)
			continue;
		const Binding *binding = switch_gpu_binding_find(binds_, nbinds_, fault, 1);
		char pages[160] = "";
		if (binding)
			switch_gpu_mapping_describe(binding->retirement->mapping, pages,
						    sizeof(pages));
		if (binding)
			IOLog("tegra-gm20b: channel %u fault at %#llx: binding %#llx+%#llx (handle "
			      "%llu, pages %s)\n",
			      slot + 1, fault, binding->va, binding->range, binding->bo->handle,
			      pages);
		else
			IOLog("tegra-gm20b: channel %u fault at %#llx: binding none\n", slot + 1,
			      fault);
		describeFault(slot, fault);
	}
}

#if TGM_STATS
static_assert(kMaxChannels == TGM_BIND_CHANNELS, "a note keeps each slot's submitted seqno");

void AGXDeviceUserClient::noteBind(uint8_t op, uint64_t va, uint64_t range, const Bo *bo,
				   uint64_t boOffset, uint8_t kind)
{
	tgm_bind_history *&history = bindHistory_[va >= TGM_KERNEL_VA / 2];
	if (!history) {
		history = static_cast<tgm_bind_history *>(IOMallocZero(sizeof(tgm_bind_history)));
		if (!history)
			return;
	}
	tgm_bind_note n = {};
	n.va = va;
	n.range = range;
	n.bo_offset = boOffset;
	n.ns = nowNs();
	n.op = op;
	n.kind = kind;
	if (bo) {
		n.handle = bo->handle;
		n.bo_bytes = bo->bytes;
		n.origin = bo->wired     ? TGM_BIND_IMPORT
			   : bo->chunked ? TGM_BIND_COMPRESSIBLE
			   : bo->pool_va ? TGM_BIND_POOL
					 : TGM_BIND_ALLOC;
		n.surface = bo->coh ? tgm_coh_surface_id(bo->coh) : 0;
	}
	for (uint32_t i = 0; i < kMaxChannels; i++)
		n.submitted[i] = chans_[i] ? tgm_chan_submitted(chans_[i]) : 0;
	tgm_bind_note_add(history, &n);
}

// The bindings beside the fault VA, and the maps, unmaps and PTE removals near it among the last TGM_BIND_NOTES of its
// half of the address space: newest few before the kill, each with whether the failed job (the kill record's
// killed_first) was already submitted then, and the first two after it.
void AGXDeviceUserClient::describeFault(uint32_t slot, uint64_t va)
{
	const volatile auto *fence = reinterpret_cast<const volatile TegraGM20BAbiFence *>(
		fence_va_ + static_cast<size_t>(slot) * 16);
	const uint32_t failed = fence->killed_first, last = fence->killed_last;
	const uint32_t after = switch_gpu_binding_upper(binds_, nbinds_, va);
	char sides[2][160] = {"none", "none"};
	for (uint32_t side = 0; side < 2; side++) {
		const Binding *b = side == 0 ? (after ? &binds_[after - 1] : nullptr)
					     : (after < nbinds_ ? &binds_[after] : nullptr);
		if (b && !(side == 0 && va < b->va + b->range))
			snprintf(sides[side], sizeof(sides[side]),
				 "%#llx+%#llx of BO %llu (%llu bytes at %#llx), kind %#x", b->va,
				 b->range, b->bo->handle, b->bo->bytes, b->bo_offset,
				 b->retirement->kind);
	}
	IOLog("tegra-gm20b: %s channel %u fault VA %#llx (jobs %u-%u failed): below: %s; above: "
	      "%s\n",
	      owner_, slot + 1, va, failed, last, sides[0], sides[1]);
	const tgm_bind_history *history = bindHistory_[va >= TGM_KERNEL_VA / 2];
	uint32_t near[16];
	const uint32_t n =
		history ? tgm_bind_history_near(history, va, TGM_BIND_SLACK, near, 16) : 0;
	const uint64_t now = nowNs(), killed = tgm_chan_killed_ns(chans_[slot]);
	uint32_t shownBefore = 0, shownAfter = 0;
	for (uint32_t i = 0; i < n; i++) {
		const tgm_bind_note &note = history->notes[near[i]];
		const bool late = killed && note.ns > killed;
		uint32_t &shown = late ? shownAfter : shownBefore;
		if (shown == (late ? 2u : 6u))
			continue;
		shown++;
		char line[256];
		tgm_bind_note_format(line, sizeof(line), &note, now, killed, slot, failed);
		IOLog("tegra-gm20b: fault VA %#llx: %s\n", va, line);
	}
	const uint32_t kept = !history                            ? 0u
			      : history->written < TGM_BIND_NOTES ? history->written
								  : TGM_BIND_NOTES;
	IOLog("tegra-gm20b: fault VA %#llx: %u maps and unmaps near it among the last %u of its "
	      "half of the address "
	      "space, which go back %llu ms\n",
	      va, n, kept, history ? tgm_bind_history_span_ns(history, now) / 1000000 : 0);
}
#else
void AGXDeviceUserClient::noteBind(uint8_t, uint64_t, uint64_t, const Bo *, uint64_t, uint8_t)
{
}

void AGXDeviceUserClient::describeFault(uint32_t, uint64_t)
{
}
#endif
