// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <string.h>
#include "TegraGM20BExecSurfaces.h"
#include "TegraGM20BFences.h"

namespace tgm_fences
{
namespace
{
constexpr uint32_t kFirstCapacity = 64;
constexpr int kNoSpace = -28;  // gpu.c's errno values are Linux's: ENOSPC

void unqueued(int error)
{
	static bool logged;
	if (__atomic_exchange_n(&logged, true, __ATOMIC_RELAXED))
		return;
	IOLog("tegra-gm20b: ExecSurfaces: a listed surface's writer is not waited for (%d: %s)\n",
	      error,
	      error == kNoSpace ? "the channel has its most acquires queued" : "no fence view");
}
}  // namespace

bool ExecSurfaces::reserve()
{
	if (nrefs_ < cap_)
		return true;
	const uint32_t cap = cap_ ? cap_ * 2 : kFirstCapacity;
	Ref *grown = IONew(Ref, cap);
	if (!grown)
		return false;
	if (refs_) {
		memcpy(grown, refs_, sizeof(Ref) * nrefs_);
		IODelete(refs_, Ref, cap_);
	}
	refs_ = grown;
	cap_ = cap;
	return true;
}

void ExecSurfaces::add(uint64_t handle, tgm_coh *coh, bool writable)
{
	if (nrefs_ < cap_)
		refs_[nrefs_++] = {handle, coh, writable};
}

const ExecSurfaces::Ref *ExecSurfaces::find(uint64_t handle) const
{
	uint32_t lo = 0;
	uint32_t hi = nrefs_;
	while (lo < hi) {
		const uint32_t mid = lo + (hi - lo) / 2;
		if (refs_[mid].handle < handle) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < nrefs_ && refs_[lo].handle == handle ? &refs_[lo] : nullptr;
}

void ExecSurfaces::remove(uint64_t handle)
{
	const Ref *r = find(handle);
	if (!r)
		return;
	const uint32_t at = static_cast<uint32_t>(r - refs_);
	memmove(&refs_[at], &refs_[at + 1], sizeof(Ref) * (nrefs_ - at - 1));
	nrefs_--;
}

void ExecSurfaces::clear()
{
	if (refs_)
		IODelete(refs_, Ref, cap_);
	refs_ = nullptr;
	nrefs_ = cap_ = 0;
	for (List *&l : lists_) {
		if (l)
			IODelete(l, List, 1);
		l = nullptr;
	}
	taken_ = nullptr;
}

void ExecSurfaces::forget(uint32_t slot)
{
	if (slot < kChannels && lists_[slot])
		lists_[slot]->given = false;
}

bool ExecSurfaces::valid(const TegraGM20BAbiExecSurfaces *in)
{
	if (in->count > TegraGM20BMaxExecSurfaces ||
	    (in->flags & ~static_cast<uint32_t>(TegraGM20BExecSurfacesAll)) || in->reserved)
		return false;
	for (uint32_t i = 0; i < in->count; i++) {
		const TegraGM20BAbiSurfaceUse &u = in->uses[i];
		if (!u.access ||
		    (u.access &
		     ~static_cast<uint32_t>(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite)) ||
		    u.reserved)
			return false;
	}
	return true;
}

bool ExecSurfaces::set(uint32_t slot, const TegraGM20BAbiExecSurfaces *in)
{
	if (slot >= kChannels)
		return false;
	List *&l = lists_[slot];
	if (!l) {
		l = IONew(List, 1);
		if (!l)
			return false;
	}
	l->given = true;
	l->count = in->count;
	l->flags = in->flags;
	l->writes = 0;
	for (uint32_t i = 0; i < in->count; i++) {
		l->handles[i] = in->uses[i].handle;
		if (in->uses[i].access & TegraGM20BSurfaceWrite)
			l->writes |= static_cast<uint64_t>(1) << i;
	}
	return true;
}

// The writers of other channels, merged per channel into at most TGM_ORDER_MAX at a time (as many acquires as a submission
// can queue), then handed to gpu.c under one hold of its lock.
int ExecSurfaces::follow(tgm_chan *chan, const List &list)
{
	const uint32_t self = tgm_chan_uid(chan);
	uint64_t acc[TGM_ORDER_MAX];
	uint32_t n = 0;
	int ret = 0;
	for (uint32_t i = 0; i < list.count && !ret; i++) {
		const Ref *r = find(list.handles[i]);
		if (!r)
			continue;
		uint64_t writers[TGM_COH_WRITERS];
		uint64_t pa = 0;
		uint32_t lost = 0;
		uint32_t pending = 0;
		const uint32_t k = tgm_coherency_writers(r->coh, writers, &lost, &pa);
		if (lost) {
			ret = tgm_chan_follow_importers(chan, pa, &pending);
			if (!ret && !pending)
				tgm_coherency_lost_seen(r->coh, lost);
		}
		for (uint32_t j = 0; j < k && !ret; j++) {
			if (!tgm_fence_merge(acc, &n, TGM_ORDER_MAX, writers[j], self))
				continue;
			ret = tgm_chan_follow(chan, acc, n);
			n = 0;
			tgm_fence_merge(acc, &n, TGM_ORDER_MAX, writers[j], self);
		}
	}
	if (!ret && n)
		ret = tgm_chan_follow(chan, acc, n);
	return ret;
}

bool ExecSurfaces::before(uint32_t slot, tgm_chan *chan, uint32_t pushes)
{
	List *l = slot < kChannels ? lists_[slot] : nullptr;
	const bool given = (l != nullptr) && l->given;
	if (l)
		l->given = false;
	taken_ = nullptr;
	if (!tgm_fence_listed(static_cast<int>(given), given ? l->flags : 0, pushes))
		return false;
	taken_ = l;
	if (const int ret = follow(chan, *l))
		unqueued(ret);
	return true;
}

void ExecSurfaces::after(bool listed, bool importsWritten, uint32_t uid, uint32_t seqno)
{
	if (!listed) {
		const int always = tgm_fence_always(0, static_cast<int>(importsWritten));
		for (uint32_t i = 0; i < nrefs_; i++) {
			if (tgm_fence_marks(0, 0, static_cast<int>(refs_[i].writable)))
				tgm_coherency_wrote(refs_[i].coh, always, uid, seqno);
		}
		return;
	}
	const List *l = taken_;
	taken_ = nullptr;
	for (uint32_t i = 0; l && i < l->count; i++) {
		const uint32_t access =
			(l->writes >> i) & 1 ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead;
		const Ref *r = find(l->handles[i]);
		if (r && tgm_fence_marks(1, access, static_cast<int>(r->writable)))
			tgm_coherency_wrote_listed(r->coh, uid, seqno);
	}
}
}  // namespace tgm_fences
