// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <string.h>
#include "TegraGM20BExecSurfaces.h"
#include "TegraGM20BFences.h"
#include "TegraGM20BJobABI.h"

namespace tgm_fences
{
namespace
{
constexpr uint32_t kFirstCapacity = 64;
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

bool ExecSurfaces::validSubmit(const TegraGM20BAbiSubmit *in, uint32_t bytes, bool addressed)
{
	if (bytes < sizeof(*in) || (!addressed && bytes > TegraGM20BMaxSubmitBytes) ||
	    in->push_count > uint32_t(addressed ? TegraGM20BMaxJobPushes : TegraGM20BMaxPushes) ||
	    in->surface_count >
		    uint32_t(addressed ? TegraGM20BMaxJobUses : TegraGM20BMaxExecSurfaces) ||
	    (in->surface_flags &
	     ~static_cast<uint32_t>(TegraGM20BExecSurfacesAll |
				    (addressed ? uint32_t(TegraGM20BSubmitVisibilityExplicit |
							  TegraGM20BSubmitExternalVisibility |
							  TegraGM20BSubmitHasEvents |
							  TegraGM20BSubmitAdmissionBegin |
							  TegraGM20BSubmitAdmissionContinue |
							  TegraGM20BSubmitAdmissionEnd |
							  TegraGM20BSubmitAdmissionCancel |
							  TegraGM20BSubmitEventReleases)
					       : 0u))) ||
	    ((in->surface_flags & TegraGM20BSubmitEventReleases) &&
	     !(in->surface_flags & TegraGM20BSubmitHasEvents)) ||
	    ((in->surface_flags & TegraGM20BSubmitExternalVisibility) &&
	     !(in->surface_flags & TegraGM20BSubmitVisibilityExplicit)) ||
	    ((in->surface_flags & TegraGM20BSubmitVisibilityExplicit) &&
	     (in->surface_flags & TegraGM20BExecSurfacesAll)))
		return false;
	auto expected = sizeof(*in) + in->push_count * sizeof(TegraGM20BAbiPush) +
			in->surface_count * sizeof(TegraGM20BAbiSurfaceUse);

	constexpr uint32_t admissionMask =
		TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionContinue |
		TegraGM20BSubmitAdmissionEnd | TegraGM20BSubmitAdmissionCancel;
	const uint32_t admissionFlags = in->surface_flags & admissionMask;
	if (admissionFlags) {
		const bool begin = admissionFlags & TegraGM20BSubmitAdmissionBegin;
		const bool next = admissionFlags & TegraGM20BSubmitAdmissionContinue;
		const bool cancel = admissionFlags & TegraGM20BSubmitAdmissionCancel;
		if ((cancel &&
		     (admissionFlags != TegraGM20BSubmitAdmissionCancel || in->push_count ||
		      in->surface_count || (in->surface_flags & TegraGM20BSubmitHasEvents))) ||
		    (!cancel && begin == next) ||
		    bytes < expected + sizeof(TegraGM20BAbiSubmitAdmission))
			return false;
		const auto *admission = reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(
			reinterpret_cast<const uint8_t *>(in) + expected);
		if (!admission->cookie || admission->reserved)
			return false;
		expected += sizeof(*admission);
	}
	if (in->surface_flags & TegraGM20BSubmitHasEvents) {
		if (bytes < expected + sizeof(TegraGM20BAbiSubmitEvents))
			return false;
		const auto *events = reinterpret_cast<const TegraGM20BAbiSubmitEvents *>(
			reinterpret_cast<const uint8_t *>(in) + expected);
		if (events->reserved || events->wait_count > TegraGM20BMaxJobEvents ||
		    events->signal_count > TegraGM20BMaxJobEvents - events->wait_count)
			return false;
		expected += sizeof(*events) + (events->wait_count + events->signal_count) *
						      sizeof(TegraGM20BAbiEventUse);
		if (in->surface_flags & TegraGM20BSubmitEventReleases)
			expected += events->signal_count * sizeof(TegraGM20BAbiSignalRelease);
		if (bytes != expected)
			return false;
		const auto *records = reinterpret_cast<const TegraGM20BAbiEventUse *>(events + 1);
		for (uint32_t i = 0; i < events->wait_count + events->signal_count; ++i)
			if (!records[i].handle)
				return false;
	}
	if (bytes != expected)
		return false;
	const auto *pushes = reinterpret_cast<const TegraGM20BAbiPush *>(in + 1);
	const auto *uses =
		reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(pushes + in->push_count);
	for (uint32_t i = 0; i < in->surface_count; i++) {
		const uint32_t allowed =
			TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite |
			(addressed ? uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange)
				   : 0u) |
			((in->surface_flags & TegraGM20BSubmitVisibilityExplicit)
				 ? uint32_t(TegraGM20BSurfaceHostWritten)
				 : 0u);
		const bool ranged = uses[i].access & TegraGM20BSurfaceRange;
		if (!(uses[i].access & (TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite)) ||
		    (ranged ? !(uses[i].access & TegraGM20BSurfaceGpuAddress) || !uses[i].reserved
			    : uses[i].reserved != 0) ||
		    (uses[i].access & ~allowed))
			return false;
	}
	return true;
}

bool ExecSurfaces::set(uint32_t slot, const TegraGM20BAbiExecSurfaces *in)
{
	return set(slot, in->uses, in->count, in->flags);
}

bool ExecSurfaces::set(uint32_t slot, const TegraGM20BAbiSurfaceUse *uses, uint32_t count,
		       uint32_t flags)
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
	l->count = count;
	l->flags = flags;
	l->writes = 0;
	l->reads = 0;
	for (uint32_t i = 0; i < count; i++) {
		l->handles[i] = uses[i].handle;
		if (uses[i].access & TegraGM20BSurfaceWrite)
			l->writes |= static_cast<uint64_t>(1) << i;
		if (uses[i].access & TegraGM20BSurfaceRead)
			l->reads |= static_cast<uint64_t>(1) << i;
	}
	return true;
}

uint32_t ExecSurfaces::listedAccess(uint64_t handle) const
{
	uint32_t access = 0;
	for (uint32_t i = 0; taken_ && i < taken_->count; i++) {
		if (taken_->handles[i] != handle)
			continue;
		if ((taken_->writes >> i) & 1)
			access |= TegraGM20BSurfaceWrite;
		if ((taken_->reads >> i) & 1)
			access |= TegraGM20BSurfaceRead;
	}
	return access;
}

// The writers of other channels, merged per channel into at most TGM_ORDER_MAX at a time (as many acquires as a submission
// can queue), then handed to gpu_sync.c under one hold of its lock.
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

int ExecSurfaces::before(uint32_t slot, tgm_chan *chan, uint32_t pushes, bool dependencies)
{
	List *l = slot < kChannels ? lists_[slot] : nullptr;
	const bool given = (l != nullptr) && l->given;
	if (l)
		l->given = false;
	taken_ = nullptr;
	if (!tgm_fence_listed(static_cast<int>(given), given ? l->flags : 0, pushes))
		return false;
	taken_ = l;
	if (const int ret = dependencies ? follow(chan, *l) : 0) {
		taken_ = nullptr;
		return ret;
	}
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
