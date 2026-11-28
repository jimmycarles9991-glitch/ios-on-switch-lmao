// SPDX-License-Identifier: MIT
// Why the kernel refused a GPU job call: SubmitJob (TegraGM20BJobABI.h method 33), ResourceSetCreate (method 31), or
// a call that never reached its method. The IOReturn goes back unchanged (the ABI has no room for a reason); the site
// and its values go to one error line, which the call's single exit writes outside every lock. Two answers are flow
// control the driver takes as normal and stay silent: kIOReturnNotReady for a set whose generation ended, and
// kIOReturnCannotLock for an admission or backing another admission holds. A client can fire a site per job, so a
// connection's refusals at one site are logged for the first four, then every 256th.
#pragma once
#include <IOKit/IOReturn.h>
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

enum class SwitchGpuRefusal : uint8_t {
	None,
	CallForm,
	CallSize,
	CallMemory,
	CallClosed,
	RecordSize,
	GpuDown,
	JobRecord,
	JobHeader,
	JobScratch,
	JobParse,
	AdmissionCancel,
	EventPrepare,
	Closing,
	NoChannel,
	NoSet,
	StaleSet,
	StaleUses,
	AdmissionFlags,
	Push,
	Fences,
	UseAccess,
	UseNoBo,
	UseNoBinding,
	UseNoBacking,
	UseSetMember,
	UseRange,
	UseReadOnly,
	NativeRead,
	AdmissionCover,
	EventSequence,
	Promises,
	Refresh,
	AdmissionBegin,
	Backings,
	WriteRanges,
	Dependencies,
	EventAppend,
	EventInherit,
	PromiseDependencies,
	EventBind,
	DependencyRoom,
	Exec,
	Room,
	Busy,
	PackImage,
	SetRecord,
	SetMemory,
	SetCopyIn,
	SetClosing,
	SetMember,
	SetSlot,
	SetFull,
	SetMake,
	SetAttach,
	SetCopyOut,
	Unnamed,
	Count
};

enum class SwitchGpuRefusalValue : uint8_t {
	None,
	Count,
	Hex,
	Errno
};

struct SwitchGpuRefusalSite {
	const char *what;
	const char *names[3];
	SwitchGpuRefusalValue kinds[3];
	bool silent;
};

namespace switch_gpu_refusal
{
using V = SwitchGpuRefusalValue;
constexpr V kN = V::None, kC = V::Count, kH = V::Hex, kE = V::Errno;

constexpr SwitchGpuRefusalSite kSites[] = {
	{"no refusal", {}, {kN, kN, kN}, true},
	{"the call is not one inline record", {"selector"}, {kC, kN, kN}, false},
	{"the call's record is larger than any method's", {"method", "bytes"}, {kC, kC, kN}, false},
	{"no kernel memory for the call's record", {"method", "bytes"}, {kC, kC, kN}, false},
	{"the connection closed before the call ran", {"selector"}, {kC, kN, kN}, false},
	{"the record's size is not the method's", {"in", "out"}, {kC, kC, kN}, false},
	{"the GPU is not up", {"stage"}, {kC, kN, kN}, false},
	{"the job record's word is malformed",
	 {"reserved", "bytes", "address"},
	 {kH, kC, kH},
	 false},
	{"the job header is unreadable or names no channel", {"address"}, {kH, kN, kN}, false},
	{"no kernel memory for the channel's job scratch", {}, {kN, kN, kN}, false},
	{"the job record does not parse or names another channel",
	 {"bytes", "header_channel"},
	 {kC, kC, kN},
	 false},
	{"an admission cancel names another admission", {"cookie", "current"}, {kH, kH, kN}, false},
	{"the job's shared events were refused", {"error"}, {kE, kN, kN}, false},
	{"the connection's submit gate is closed", {}, {kN, kN, kN}, false},
	{"the channel slot holds no channel", {}, {kN, kN, kN}, false},
	{"the job names a resource set slot with no set", {"set"}, {kC, kN, kN}, false},
	{"the set's generation ended", {"set", "generation"}, {kC, kC, kN}, true},
	{"the channel's cached uses are stale", {"bind_gen", "bo_gen"}, {kC, kC, kN}, true},
	{"the job's admission flags do not fit the channel's admission",
	 {"flags", "cookie", "current"},
	 {kH, kH, kH},
	 false},
	{"a push entry is invalid", {"push", "va", "dwords"}, {kC, kH, kC}, false},
	{"the job's fence pushes are invalid", {"pushes"}, {kC, kN, kN}, false},
	{"a shared use's access word is invalid",
	 {"use", "access", "reserved"},
	 {kC, kH, kH},
	 false},
	{"a shared use names no BO of this connection", {"use", "handle"}, {kC, kH, kN}, false},
	{"a shared use's GPU address has no binding",
	 {"use", "address", "bytes"},
	 {kC, kH, kC},
	 false},
	{"a shared use's BO has no GPU backing", {"use", "handle", "bytes"}, {kC, kH, kC}, false},
	{"a shared use names a member of a resource set", {"use", "handle"}, {kC, kH, kN}, false},
	{"a shared use's range is not inside its binding",
	 {"use", "address", "bytes"},
	 {kC, kH, kC},
	 false},
	{"a shared use writes a read-only BO", {"use", "handle", "access"}, {kC, kH, kH}, false},
	{"a native view's read was refused", {"use", "error"}, {kC, kE, kN}, false},
	{"the admission does not cover the job's uses", {"uses"}, {kC, kN, kN}, false},
	{"the job's events could not be sequenced", {"error"}, {kE, kN, kN}, false},
	{"the job's writer promises are invalid", {"promises", "pushes"}, {kC, kC, kN}, false},
	{"the job's IOSurfaces could not be refreshed", {"error", "surfaces"}, {kE, kC, kN}, false},
	{"the job's admission could not begin", {"error", "uses"}, {kE, kC, kN}, false},
	{"the job's backings could not be taken", {"error", "uses"}, {kE, kC, kN}, false},
	{"the job's write ranges were refused", {"error", "writes"}, {kE, kC, kN}, false},
	{"the job's backing dependencies were refused", {"error"}, {kE, kN, kN}, false},
	{"the job's event dependencies were refused",
	 {"error", "dependencies"},
	 {kE, kC, kN},
	 false},
	{"the job's inherited event waits were refused", {"error"}, {kE, kN, kN}, false},
	{"another connection's writer promises were refused",
	 {"error", "dependencies"},
	 {kE, kC, kN},
	 false},
	{"the job's events could not be bound to its completion", {"error"}, {kE, kN, kN}, false},
	{"the job waits on more than a job may", {"waits", "max"}, {kC, kC, kN}, false},
	{"the channel refused the job", {"error", "pushes", "dependencies"}, {kE, kC, kC}, false},
	{"no GPFIFO room came for the job", {"error", "pushes"}, {kE, kC, kN}, false},
	{"another admission holds what the job uses", {}, {kN, kN, kN}, true},
	{"the pack image the job's programs are in stopped streaming", {}, {kN, kN, kN}, false},
	{"the set record is malformed", {"count", "set", "address"}, {kC, kC, kH}, false},
	{"no kernel memory for the set's members", {"count"}, {kC, kN, kN}, false},
	{"the set's uses are unreadable", {"address", "count"}, {kH, kC, kN}, false},
	{"the connection's submit gate is closed to set changes", {}, {kN, kN, kN}, false},
	{"a set member is refused", {"member", "handle", "reason"}, {kC, kH, kC}, false},
	{"the set record names a slot with no set", {"set"}, {kC, kN, kN}, false},
	{"every resource set slot is taken", {"slots"}, {kC, kN, kN}, false},
	{"the set could not be made", {"error"}, {kE, kN, kN}, false},
	{"the set's members could not be attached", {"error", "members"}, {kE, kC, kN}, false},
	{"the set's reply could not be written back", {"address"}, {kH, kN, kN}, false},
	{"a refusal no site named", {}, {kN, kN, kN}, false},
};
static_assert(sizeof(kSites) / sizeof(kSites[0]) == size_t(SwitchGpuRefusal::Count),
	      "a site per refusal");

constexpr uint32_t kLoggedFirst = 4, kLoggedEvery = 256;

// Whether the `nth` refusal (from 0) at one site of one connection gets its line
constexpr bool logged(uint32_t nth)
{
	return nth < kLoggedFirst || nth % kLoggedEvery == 0;
}

constexpr const SwitchGpuRefusalSite &site(SwitchGpuRefusal r)
{
	return kSites[r < SwitchGpuRefusal::Count ? size_t(r) : size_t(SwitchGpuRefusal::Unnamed)];
}
}  // namespace switch_gpu_refusal

struct SwitchGpuRefusalNote {
	SwitchGpuRefusal site = SwitchGpuRefusal::None;
	uint64_t values[3] = {};
};

// A SubmitJob refusal the compositor's (a critical client's) frame does not survive, which panics at the refusal: any
// but the silent sites the runtime resubmits, Aborted (a -5 while the job is admitted, the one source found a native
// surface not readied for the GPU, SwitchGpuSurfaceRefresh) and NotOpen (the connection closing: the process is going).
inline bool switch_gpu_refusal_loses_frame(bool critical, SwitchGpuRefusal site, int32_t ret)
{
	const SwitchGpuRefusal named =
		site == SwitchGpuRefusal::None ? SwitchGpuRefusal::Unnamed : site;
	return critical && !switch_gpu_refusal::site(named).silent &&
	       ret != int32_t(kIOReturnAborted) && ret != int32_t(kIOReturnNotOpen);
}

// A negative errno as a refusal's value (SwitchGpuRefusalValue::Errno prints it signed)
constexpr uint64_t switch_gpu_refusal_errno(int error)
{
	return static_cast<uint64_t>(static_cast<int64_t>(error));
}

// A refusal at `site` with its values: notes it for the call's exit and answers `ret`
inline int32_t switch_gpu_refuse(SwitchGpuRefusalNote &note, SwitchGpuRefusal site, int32_t ret,
				 uint64_t a = 0, uint64_t b = 0, uint64_t c = 0)
{
	note = {site, {a, b, c}};
	return ret;
}

// The line for `call`'s refusal answered `ret` on `channel` (0: none named), the `nth` at its site from this connection,
// written to `line`; false (and nothing written) when the site is silent or the rate bound holds it back
inline bool switch_gpu_refusal_line(char *line, size_t size, const char *owner, int pid,
				    const char *call, uint32_t channel, int32_t ret,
				    const SwitchGpuRefusalNote &note, uint32_t nth)
{
	const SwitchGpuRefusalSite &s = switch_gpu_refusal::site(
		note.site == SwitchGpuRefusal::None ? SwitchGpuRefusal::Unnamed : note.site);
	if (s.silent || !switch_gpu_refusal::logged(nth) || !size)
		return false;
	size_t used = 0;
	const auto add = [&](int wrote) {
		if (wrote > 0)
			used += size_t(wrote) < size - used ? size_t(wrote) : size - used - 1;
	};
	add(snprintf(line, size, "tegra-gm20b: %s[%d]: %s", owner, pid, call));
	if (channel)
		add(snprintf(line + used, size - used, " on channel %u", channel));
	add(snprintf(line + used, size - used, " refused, %#x: %s", uint32_t(ret), s.what));
	for (int i = 0; i < 3; ++i) {
		const uint64_t v = note.values[i];
		switch (s.kinds[i]) {
		case SwitchGpuRefusalValue::Count:
			add(snprintf(line + used, size - used, " %s=%llu", s.names[i],
				     static_cast<unsigned long long>(v)));
			break;
		case SwitchGpuRefusalValue::Hex:
			add(snprintf(line + used, size - used, " %s=%#llx", s.names[i],
				     static_cast<unsigned long long>(v)));
			break;
		case SwitchGpuRefusalValue::Errno:
			add(snprintf(line + used, size - used, " %s=%lld", s.names[i],
				     static_cast<long long>(v)));
			break;
		case SwitchGpuRefusalValue::None:
			break;
		}
	}
	if (nth + 1 == switch_gpu_refusal::kLoggedFirst)
		add(snprintf(line + used, size - used, " (from here every %uth)",
			     switch_gpu_refusal::kLoggedEvery));
	else if (nth >= switch_gpu_refusal::kLoggedFirst)
		add(snprintf(line + used, size - used, " (refusal %u here)", nth + 1));
	return true;
}
