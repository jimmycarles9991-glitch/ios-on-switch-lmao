// SPDX-License-Identifier: MIT
// VM binds, surface layouts, channels, and the hung submission's interlocks (the push scan: TegraGM20BPushScanRun.cpp).
#include "TegraGM20BUserClientInternal.h"
#include "TegraGM20BLayoutAsk.h"
#include "TegraGM20BRecord.h"

// lock_ held. A Map into VA no binding holds publishes its binding with the submit gate open: no job in flight can have
// resolved that VA. A cut (an Unmap, or a Map over a live binding) closes the gate first: its retirement snapshot and the
// bind epoch it publishes must have no kick between them (tgm_vm_bind_wait), which a job inside the gate could make.
IOReturn AGXDeviceUserClient::vmBind(const TegraGM20BAbiVmBind *in, uint32_t count)
{
	const bool cuts = bindCuts(in, count);
	if (cuts && !submitGate_.beginMutation(lock_))
		return kIOReturnNotOpen;
	BindingTable::Snapshot *snapshot = BindingTable::prepare(nbinds_ + 2 * count);
	IOReturn ret = snapshot ? kIOReturnSuccess : kIOReturnNoMemory;
	bool changed = false;
	for (uint32_t i = 0; !ret && i < count; i++)
		ret = bindOp(in->ops[i], changed);
	if (changed)
		publishBindings(snapshot);
	else
		BindingTable::discard(snapshot);
	if (cuts)
		submitGate_.endMutation(lock_);
	return ret;
}

// lock_ held: whether an op would take VA from a live binding (one that is the same binding again changes nothing).
bool AGXDeviceUserClient::bindCuts(const TegraGM20BAbiVmBind *in, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiBindOp &op = in->ops[i];
		if (!op.range || op.va > UINT64_MAX - op.range)
			continue;
		const uint32_t after =
			switch_gpu_binding_upper(binds_, nbinds_, op.va + op.range - 1);
		const Binding *b = after ? &binds_[after - 1] : nullptr;
		if (!b || b->va + b->range <= op.va)
			continue;
		const bool same = op.op != TegraGM20BBindUnmap && b->va == op.va &&
				  b->range == op.range && b->bo == findBo(op.handle) &&
				  b->bo_offset == op.bo_offset && b->retirement->kind == op.kind &&
				  b->retirement->vol == (op.op == TegraGM20BBindMapVolatile);
		if (!same)
			return true;
	}
	return false;
}

IOReturn AGXDeviceUserClient::bindOp(const TegraGM20BAbiBindOp &op, bool &changed)
{
	if (((op.va | op.range | op.bo_offset) & 0xfff) || !op.range || op.va < TGM_USER_VA_START ||
	    op.range > TGM_KERNEL_VA || op.va > TGM_KERNEL_VA - op.range || op.kind > 0xff)
		return kIOReturnBadArgument;
	if (op.op == TegraGM20BBindMapSparse)
		return kIOReturnUnsupported;  // InfoSparse is not advertised in v1
	const uint64_t start = statsNowNs();
	const bool unmap = op.op == TegraGM20BBindUnmap;
	if (!unmap && op.op != TegraGM20BBindMap && op.op != TegraGM20BBindMapVolatile)
		return kIOReturnBadArgument;
	Bo *bo = unmap ? nullptr : findBo(op.handle);
	if (!unmap && (!bo || op.bo_offset > bo->bytes || op.range > bo->bytes - op.bo_offset)) {
		noteBind(TGM_BIND_REFUSED, op.va, op.range, bo, op.bo_offset, uint8_t(op.kind));
		return kIOReturnBadArgument;
	}
	const Binding *same = switch_gpu_binding_find(binds_, nbinds_, op.va, op.range);
	if (!unmap && same && same->va == op.va && same->range == op.range && same->bo == bo &&
	    same->bo_offset == op.bo_offset && same->retirement->kind == op.kind &&
	    same->retirement->vol == (op.op == TegraGM20BBindMapVolatile))
		return kIOReturnSuccess;
	// Everything a cut and the new binding need, before any binding changes.
	if (nbinds_ + 2 > cap_binds_) {
		uint32_t cap = cap_binds_ ? cap_binds_ * 2 : 64;
		while (cap < nbinds_ + 2)
			cap *= 2;
		Binding *grown = IONew(Binding, cap);
		if (!grown)
			return kIOReturnNoMemory;
		if (binds_) {
			memcpy(grown, binds_, sizeof(Binding) * nbinds_);
			IODelete(binds_, Binding, cap_binds_);
		}
		binds_ = grown;
		cap_binds_ = cap;
	}
	RetiredBinding *spare[2] = {IONew(RetiredBinding, 1), IONew(RetiredBinding, 1)};
	auto *record = unmap ? nullptr : IONew(RetiredBinding, 1);
	const auto freeSpare = [&] {
		for (auto *each : spare)
			if (each)
				IODelete(each, RetiredBinding, 1);
	};
	IOReturn cut = !spare[0] || !spare[1] || (!unmap && !record) ? kIOReturnNoMemory
								     : kIOReturnSuccess;
	// Work queued now may still use what the range held: its PTEs stay until that work completes, and a Map's
	// new PTEs wait for retirement behind the bind epoch. With none queued, both happen here.
	const bool queued = workQueued(op.va, op.range);
	const uint32_t after = switch_gpu_binding_upper(binds_, nbinds_, op.va + op.range - 1);
	changed = changed ||
		  (!cut && after && binds_[after - 1].va + binds_[after - 1].range > op.va);
	if (!cut)
		cut = cutOverlaps(op.va, op.range, !queued, !unmap, spare);
	freeSpare();
	if (unmap || cut) {
		if (record)
			IODelete(record, RetiredBinding, 1);
		if (unmap && !cut)
			tgm_stats_bind(0, op.range >> 12, statsNowNs() - start);
		return cut;
	}
	if (const IOReturn mapped = mapRange(op, bo, record, queued))
		return mapped;
	changed = true;
	switch_gpu_path_taken(SWITCH_GPU_PATH_VOLATILE_BIND,
			      op.op == TegraGM20BBindMapVolatile ? 1 : 0);
	tgm_stats_bind(1, op.range >> 12, statsNowNs() - start);
	return kIOReturnSuccess;
}

// lock_ held on entry and exit, but not across the decision: it may tile the surface on the copy engine and sleep on its
// fence, and the connection's other calls go on meanwhile. The surface and its coherency entry are held for it, so a
// BoFree meanwhile frees nothing it uses.
IOReturn AGXDeviceUserClient::surfaceLayout(const TegraGM20BAbiSurfaceLayout *in,
					    TegraGM20BAbiSurfaceLayoutReply *out)
{
	TegraGM20BAbiSurfaceLayoutInfo info = {};
	const IOReturn result = surfaceLayoutInfo(in, &info, false);
	*out = {info.layout, 0};
	return result;
}

IOReturn AGXDeviceUserClient::surfaceLayoutInfo(const TegraGM20BAbiSurfaceLayout *in,
						TegraGM20BAbiSurfaceLayoutInfo *out, bool canonical)
{
	Bo *bo = findBo(in->handle);
	if (!bo || !bo->wired || !bo->coh)
		return kIOReturnBadArgument;
	const bool firstAsk = !bo->layoutAsked;
	bo->layoutAsked = true;
	SwitchGpuSurfaceGeometry surface = {};
	if (switch_gpu_surface_geometry(tgm_coh_surface_id(bo->coh), task_, bo->pas, bo->npages,
					&surface))
		return kIOReturnBadArgument;
	tgm_coh *coh = bo->coh;
	OSObject *object = bo->surface;
	const uint64_t first = bo->pas[0];
	tgm_coh_hold_asker(coh);
	if (object)
		object->retain();
	IOLockUnlock(lock_);
	const bool packed = switch_gpu_surface_packed(surface);
	const bool same = switch_gpu_surface_shape_matches(surface, in->width, in->height, in->bpp,
							   in->bytes_per_row);
	const bool exclusive = switch_gpu_surface_layout_exclusive(object, true) == 0;
	const int writes = tgm_layout_ask_flags(
		exclusive, packed, surface.client_memory, canonical || same,
		(in->flags & TegraGM20BLayoutWrites) != 0, tgm_surface_unwritten(object), firstAsk);
	const uint32_t width = uint32_t(surface.width), height = uint32_t(surface.height),
		       pitch = uint32_t(surface.pitch);
	const int layout =
		!packed     ? tgm_coherency_layout(coh, 0, 0, 0, 0, 0)
		: canonical ? tgm_coherency_layout_fitted(coh, width, height, surface.bpp, pitch,
							  writes)
			    : tgm_coherency_layout(coh, width, height, surface.bpp, pitch, writes);
	if (exclusive)
		switch_gpu_surface_layout_exclusive(object, false);
	tgm_layout tiled = {};
	const bool described = layout > 0 && tegra_gm20b_surface_layout(first, &tiled);
	tgm_coh_put_asker(coh);
	if (object)
		object->release();
	IOLockLock(lock_);
	*out = {};
	if (surface.width <= UINT32_MAX && surface.height <= UINT32_MAX &&
	    surface.pitch <= UINT32_MAX) {
		out->width = uint32_t(surface.width);
		out->height = uint32_t(surface.height);
		out->bpp = surface.bpp;
		out->pitch = uint32_t(surface.pitch);
		out->rows = uint32_t(surface.height);
		out->bytes = surface.bytes;
		out->align = 4096;
	}
	if (layout > 0) {
		if (!described)
			return kIOReturnNotReady;
		// method 16's askers lay the shape out themselves, at NIL's block height
		const bool readable = canonical || (same && tgm_layout_nil_height(&tiled));
		*out = {readable ? uint32_t(TegraGM20BLayoutTiled)
				 : uint32_t(TegraGM20BLayoutOtherShape),
			tiled.width,
			tiled.height,
			tiled.bpp,
			tiled.row_bytes,
			tiled.y_log2,
			tiled.rows,
			0,
			tiled.align,
			tiled.bytes};
	} else if (layout < 0) {
		out->layout = TegraGM20BLayoutOtherShape;
	}
	return kIOReturnSuccess;
}

// lock_ held. The connection's level from its priority and GPU denial (TegraGM20BSchedPolicy.h), for every channel it has.
void AGXDeviceUserClient::applyLevel(bool allChannels)
{
	level_ = tgm_priority_level(priority_, backgroundPriority_,
				    static_cast<int>(backgroundRole_));
	critical_ = level_ == TGM_LEVEL_HIGH;
	if (allChannels)
		for (uint32_t i = 0; i < kMaxChannels; ++i)
			if (chans_[i]) {
				tgm_chan_set_level(chans_[i], level_);
				channelLevels_[i] = level_;
			}
}

IOReturn AGXDeviceUserClient::channelCreate(const TegraGM20BAbiChannelCreate *in,
					    TegraGM20BAbiChannelCreateReply *out)
{
	const int copyOnly = switch_gpu_channel_copy_only(in->engines);
	if (in->reserved || copyOnly < 0)
		return kIOReturnBadArgument;
	uint32_t slot = 0;
	while (slot < kMaxChannels && chans_[slot])
		slot++;
	if (slot == kMaxChannels)
		return kIOReturnNoResources;
	// not reset: a slot's seqno only moves forward (tgm_chan_new continues it)
	volatile uint32_t *fence = fence_va_ + static_cast<size_t>(slot) * 16;
	tgm_fence_kill_clear(fence);
	const auto create = copyOnly ? tgm_chan_new_copy : tgm_chan_new;
	const IOReturn ret =
		ioreturn(create(vm_, this, fence, fence_gpu_ + static_cast<uint64_t>(slot) * 64,
				owner_, static_cast<int>(critical_), &chans_[slot]));
	if (ret)
		return ret;
	switch_gpu_chan_visibility(chans_[slot], fence_va_ + 128 + slot,
				   fence_gpu_ + 512 + uint64_t(slot) * 4);
	auto *semantic = reinterpret_cast<volatile TegraGM20BAbiFenceSemantic *>(fence + 4);
	switch_gpu_chan_semantic_visibility(chans_[slot], semantic);
	auto *identity = reinterpret_cast<volatile TegraGM20BAbiFenceIdentity *>(fence + 8);
	__atomic_store_n(&identity->channel_uid, switch_gpu_chan_identity(chans_[slot]),
			 __ATOMIC_RELEASE);
	tgm_chan_set_group(chans_[slot], task_);  // Order relates the channels of one task
	if (level_ != TGM_LEVEL_FOREGROUND && !critical_)
		tgm_chan_set_level(chans_[slot], level_);
	channelLevels_[slot] = level_;
	leanChannels_[slot] = copyOnly != 0;
	faultLogged_[slot] = false;
	tgm_chan_on_fence(chans_[slot], retirementFence, this);
	tgm_chan_on_wake(chans_[slot], wakeAsked, this, slot);
	out->channel = slot + 1;
	out->fence_token = kFenceToken;
	out->fence_offset = slot * 64;
	out->reserved = 0;
	return kIOReturnSuccess;
}

// Method 14: GPU-side order between this channel and the task's other channels (gpu_sync.c tgm_chan_order); the peers are found by
// task in gpu_sync.c, only the caller's own channel is named here.
IOReturn AGXDeviceUserClient::order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out)
{
	static_assert(TegraGM20BOrderAfterTask == TGM_ORDER_AFTER &&
			      TegraGM20BOrderBeforeTask == TGM_ORDER_BEFORE &&
			      TegraGM20BMaxOrderWaits == TGM_ORDER_MAX,
		      "order flags");
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	uint32_t queued = 0;
	const IOReturn ret =
		ioreturn(tgm_chan_order(chans_[in->channel - 1], in->flags, in->seqno, &queued));
	out->waits = queued;
	out->reserved = 0;
	return ret;
}

// Method 17: what the channel's next Exec touches (TegraGM20BExecSurfaces.h).
IOReturn AGXDeviceUserClient::execSurfaces(const TegraGM20BAbiExecSurfaces *in)
{
	static_assert(tgm_fences::kChannels == kMaxChannels, "channel slots");
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	return surfaces_.set(in->channel - 1, in) ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn AGXDeviceUserClient::channelDestroy(const TegraGM20BAbiChannel *in)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	logFaultBindings();
	releasePromises(false);
	if (tgm_chan_error(chans_[in->channel - 1]))
		for (auto *set : sets_)
			if (set)
				switch_gpu_set_killed(set, in->channel - 1);
	hangHolder_ = current_thread();
	tgm_chan_del(&chans_[in->channel - 1]);
	eventChannels_[in->channel - 1].reset();
	admissions_[in->channel - 1].end();
	admissionCookies_[in->channel - 1] = 0;
	hangHolder_ = nullptr;
	surfaces_.forget(in->channel - 1);
	scheduleRetirement();
	return kIOReturnSuccess;
}

// The kernel's view of the push at [va, va + bytes) through the binding that holds it (`hit`: the last one found, tried
// first): pool BOs write-combined, cached and system BOs their kernel mapping. Null for a host import (no kernel mapping).
// lock_ held, or a channel's submit lock: it reads the published bindings.
const uint32_t *AGXDeviceUserClient::pushView(uint64_t va, uint64_t bytes,
					      const BindingTable::Entry *&hit) const
{
	if (!hit || !switch_gpu_binding_contains(*hit, va, bytes))
		hit = bindTable_.find(va, bytes);
	const void *base = nullptr;
	if (hit) {
		if (hit->bo->pool_va) {
			base = hit->bo->pool_va;
		} else {
			base = hit->bo->buffer.cpu;
		}
	}
	return base ? reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(base) +
							 hit->bo_offset + (va - hit->va))
		    : nullptr;
}

// A hang's line (gpu_hang.c): the hung submission's pushes read as the stats read them, for its interlocked
// states. From a GPU panic path on any thread: the binds are read under lock_, taken only if it is free (a thread that
// holds it while a GPU wait hangs would never let it go), unless this thread is the one holding it (hangHolder_).
int AGXDeviceUserClient::hungInterlocks(const uint64_t *va, const uint32_t *dwords, uint32_t n,
					uint64_t *interlocks, uint32_t *lastEngine)
{
	const bool held = hangHolder_ == current_thread();
	if (!held && !IOLockTryLock(lock_))
		return -1;
	PushCounts c = {};
	PushParse p = {};
	const BindingTable::Entry *hit = nullptr;
	int read = 0;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t *words = pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit);
		if (!words) {
			p = {};  // a method left open by the push not read ends with it
			continue;
		}
		tgm_scan::pushParse(c, p, words, dwords[i]);
		read++;
	}
	if (!held)
		IOLockUnlock(lock_);
	*interlocks = c.interlocks;
	*lastEngine = p.lastEngine;
	return read;
}

extern "C" int tgm_client_interlocks(const void *client, const uint64_t *va, const uint32_t *dwords,
				     uint32_t n, uint64_t *interlocks, uint32_t *last_engine)
{
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the C caller holds the client as const void *
	return static_cast<AGXDeviceUserClient *>(const_cast<void *>(client))
		->hungInterlocks(va, dwords, n, interlocks, last_engine);
}
