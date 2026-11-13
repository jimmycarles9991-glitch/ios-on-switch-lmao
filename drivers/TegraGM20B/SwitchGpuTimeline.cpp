// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuTimeline.h"
#include "SwitchGpuDependencyTrace.h"
#include "SwitchGpuBridge.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BABI.h"
#include "SwitchGpuSharedEvent.h"

static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr));

struct switch_gpu_timeline_semantic {
	switch_gpu_timeline_semantic *next;
	switch_gpu_timeline *timeline;
	switch_gpu_event_waits *authority;
	uint32_t seqno;
};

// The newest jobs whose own event waits failed. A failure belongs to that job alone: later jobs on the channel do not
// depend on its wait. An older failure pushed out by newer ones reads as its physical status.
constexpr uint32_t kFailedKept = 8;

struct switch_gpu_timeline {
	switch_gpu_timeline *next;
	uint64_t uid;
	const volatile uint32_t *fence;
	const volatile uint32_t *allocation;
	const volatile uint32_t *visible;
	uint32_t terminal_visible, promoted;
	uint64_t terminal;
	uint32_t refs;
	switch_gpu_timeline_semantic *semantics;
	volatile TegraGM20BAbiFenceSemantic *semantic_public;
	uint32_t failed[kFailedKept];
	uint32_t failed_count;
};

namespace
{
IOLock *lock;
switch_gpu_timeline *timelines;
uint32_t semantic_count;

bool failed_locked(const switch_gpu_timeline *timeline, uint32_t value)
{
	const uint32_t kept =
		timeline->failed_count < kFailedKept ? timeline->failed_count : kFailedKept;
	for (uint32_t i = 0; i < kept; ++i)
		if (timeline->failed[i] == value)
			return true;
	return false;
}

void fail_locked(switch_gpu_timeline *timeline, uint32_t value)
{
	if (failed_locked(timeline, value))
		return;
	if (!timeline->failed_count)
		__atomic_add_fetch(&semantic_count, 1u, __ATOMIC_RELEASE);
	timeline->failed[timeline->failed_count % kFailedKept] = value;
	if (++timeline->failed_count == 2 * kFailedKept)
		timeline->failed_count = kFailedKept;
}

uint32_t newest_failure(const switch_gpu_timeline *timeline)
{
	return timeline->failed[(timeline->failed_count + kFailedKept - 1) % kFailedKept];
}

bool initialize()
{
	if (__atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return true;
	IOLock *made = IOLockAlloc();
	if (!made)
		return false;
	IOLock *expected = nullptr;
	if (!__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE))
		IOLockFree(made);
	return true;
}

switch_gpu_timeline *find(const switch_gpu_backing_token *token)
{
	if (!token || token->engine != SWITCH_GPU_ENGINE_GM20B || !token->reset_generation ||
	    token->reset_generation > UINT32_MAX || token->value > UINT32_MAX)
		return nullptr;
	const uint64_t uid = ((token->reset_generation - 1) << 32) | token->queue;
	for (auto *at = timelines; at; at = at->next)
		if (at->uid == uid)
			return at;
	return nullptr;
}

// Every load atomic: the present's interrupt filter reads a retained token's timeline with no lock
// (switch_gpu_timeline_view_visible). The fence and visible words are in a fence page the timeline holds.
int physical_now(const switch_gpu_timeline *timeline, uint32_t value, bool external)
{
	uint64_t terminal = __atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE);
	uint32_t completed = uint32_t(terminal),
		 visible = __atomic_load_n(&timeline->terminal_visible, __ATOMIC_ACQUIRE);
	const volatile uint32_t *fence = __atomic_load_n(&timeline->fence, __ATOMIC_ACQUIRE);
	if (!terminal && fence) {
		completed = __atomic_load_n(fence, __ATOMIC_ACQUIRE);
		visible = __atomic_load_n(__atomic_load_n(&timeline->visible, __ATOMIC_ACQUIRE),
					  __ATOMIC_ACQUIRE);
		// Recovery publishes failure before advancing abandoned hardware acquire fences.
		terminal = __atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE);
		if (terminal) {
			completed = uint32_t(terminal);
			visible = __atomic_load_n(&timeline->terminal_visible, __ATOMIC_ACQUIRE);
		}
	}
	if (int32_t(completed - value) < 0)
		return terminal || !fence ? -5 : 0;
	return !external || int32_t(visible - value) >= 0 ||
			       int32_t(__atomic_load_n(&timeline->promoted, __ATOMIC_ACQUIRE) -
				       value) >= 0
		       ? 1
		       : 0;
}

void publish_semantic_locked(switch_gpu_timeline *timeline)
{
	auto *state = timeline->semantic_public;
	if (!state)
		return;
	uint32_t pending = 0, flags = timeline->failed_count ? TegraGM20BFenceSemanticFailed : 0;
	for (auto *node = timeline->semantics; node; node = node->next) {
		if (!(flags & TegraGM20BFenceSemanticPending) || int32_t(node->seqno - pending) < 0)
			pending = node->seqno;
		flags |= TegraGM20BFenceSemanticPending;
	}
	const uint32_t epoch = __atomic_load_n(&state->epoch, __ATOMIC_RELAXED);
	__atomic_store_n(&state->epoch, epoch + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&state->pending_first, pending, __ATOMIC_RELAXED);
	__atomic_store_n(&state->failed_first,
			 timeline->failed_count ? newest_failure(timeline) : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&state->flags, flags, __ATOMIC_RELAXED);
	__atomic_store_n(&state->epoch, epoch + 2, __ATOMIC_RELEASE);
}

int status_locked(const switch_gpu_timeline *timeline, uint32_t value, bool external)
{
	const int physical = physical_now(timeline, value, external);
	if (physical <= 0)
		return physical;
	if (failed_locked(timeline, value))
		return -5;
	for (auto *node = timeline->semantics; node; node = node->next) {
		if (int32_t(value - node->seqno) < 0)
			continue;
		const int semantic = switch_gpu_event_waits_cached_status(node->authority);
		if (semantic == 0)
			return 0;
		if (semantic < 0 && node->seqno == value)
			return -5;
	}
	return physical;
}

int query(const switch_gpu_backing_token *token, bool external)
{
	IOLockLock(lock);
	const auto *timeline = find(token);
	const int result =
		timeline ? status_locked(timeline, uint32_t(token->value), external) : -19;
	IOLockUnlock(lock);
	return result;
}

int status(const switch_gpu_backing_token *token)
{
	return query(token, false);
}

int external_status(const switch_gpu_backing_token *token)
{
	return query(token, true);
}

int wait(const switch_gpu_backing_token *token);

int external_wait(const switch_gpu_backing_token *token)
{
	SwitchGpuDependencyTrace dependency;
	const int executed = wait(token);
	if (executed <= 0)
		return executed;
	const int visible = external_status(token);
	return visible ? visible : switch_gpu_bridge_wait_external(token);
}

int wait(const switch_gpu_backing_token *token)
{
	SwitchGpuDependencyTrace dependency;
	int result = status(token);
	if (!result) {
		tgm_writer_wait(token->queue, uint32_t(token->value));
		result = status(token);
	}
	if (!result) {
		switch_gpu_event_waits *authority = nullptr;
		const int semantic = switch_gpu_timeline_semantic_snapshot(token, &authority);
		if (semantic < 0)
			return semantic;
		if (authority) {
			result = switch_gpu_event_waits_wait(authority, 0);
			switch_gpu_event_waits_release(&authority);
		}
	}
	return result;
}

int retain(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	if (timeline)
		++timeline->refs;
	IOLockUnlock(lock);
	return timeline ? 0 : -19;
}

void put_locked(switch_gpu_timeline *timeline)
{
	if (--timeline->refs)
		return;
	auto **at = &timelines;
	while (*at && *at != timeline)
		at = &(*at)->next;
	if (*at)
		*at = timeline->next;
	if (timeline->failed_count)
		__atomic_sub_fetch(&semantic_count, 1u, __ATOMIC_RELEASE);
	switch_gpu_fence_put(timeline->allocation);
	IODelete(timeline, switch_gpu_timeline, 1);
}

void release(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	if (auto *timeline = find(token))
		put_locked(timeline);
	IOLockUnlock(lock);
}
}  // namespace

extern "C" int switch_gpu_timeline_init(void)
{
	if (!initialize())
		return -12;
	const switch_gpu_backing_engine operations = {status, wait, retain, release, 32};
	const int result = switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B, &operations);
	if (result && result != -16)
		return result;
	const switch_gpu_backing_visibility visibility = {external_status, external_wait};
	const int visible_result =
		switch_gpu_backing_register_visibility(SWITCH_GPU_ENGINE_GM20B, &visibility);
	if (visible_result && visible_result != -16)
		return visible_result;
	const switch_gpu_backing_semantics semantics = {switch_gpu_timeline_semantic_snapshot,
							switch_gpu_timeline_physical_status,
							switch_gpu_timeline_has_semantics};
	const int semantic_result =
		switch_gpu_backing_register_semantics(SWITCH_GPU_ENGINE_GM20B, &semantics);
	return semantic_result == -16 ? 0 : semantic_result;
}

extern "C" switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid,
							   const volatile uint32_t *fence)
{
	if (!fence || !lock)
		return nullptr;
	auto *timeline = IONew(switch_gpu_timeline, 1);
	if (!timeline)
		return nullptr;
	IOLockLock(lock);
	switch_gpu_fence_hold(fence);
	const uint32_t completed = __atomic_load_n(fence, __ATOMIC_ACQUIRE);
	*timeline = {timelines, uid, fence,   fence,   fence, completed, completed,
		     0,         1,   nullptr, nullptr, {},    0};
	timelines = timeline;
	IOLockUnlock(lock);
	return timeline;
}

extern "C" void switch_gpu_timeline_failed(switch_gpu_timeline *timeline, uint32_t completed)
{
	if (!timeline)
		return;
	uint64_t expected = 0;
	const uint32_t visible = timeline->visible == timeline->fence
					 ? completed
					 : __atomic_load_n(timeline->visible, __ATOMIC_ACQUIRE);
	__atomic_store_n(&timeline->terminal_visible, visible, __ATOMIC_RELEASE);
	__atomic_compare_exchange_n(&timeline->terminal, &expected, (uint64_t(1) << 32) | completed,
				    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	switch_gpu_backing_completed();
}

extern "C" void switch_gpu_timeline_close(switch_gpu_timeline *timeline)
{
	if (!timeline)
		return;
	IOLockLock(lock);
	uint64_t expected = 0;
	const uint32_t completed = __atomic_load_n(timeline->fence, __ATOMIC_ACQUIRE);
	if (!__atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE))
		__atomic_store_n(&timeline->terminal_visible,
				 __atomic_load_n(timeline->visible, __ATOMIC_ACQUIRE),
				 __ATOMIC_RELEASE);
	__atomic_compare_exchange_n(&timeline->terminal, &expected, (uint64_t(1) << 32) | completed,
				    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	timeline->fence = nullptr;
	timeline->semantic_public = nullptr;
	put_locked(timeline);
	IOLockUnlock(lock);
	switch_gpu_backing_completed();
}

extern "C" void switch_gpu_timeline_token(switch_gpu_timeline *timeline, uint32_t value,
					  switch_gpu_backing_token *token)
{
	*token = {value, (timeline->uid >> 32) + 1, SWITCH_GPU_ENGINE_GM20B,
		  uint32_t(timeline->uid)};
}

extern "C" void switch_gpu_timeline_visibility(switch_gpu_timeline *timeline,
					       const volatile uint32_t *visible)
{
	IOLockLock(lock);
	timeline->visible = visible;
	timeline->promoted = __atomic_load_n(visible, __ATOMIC_ACQUIRE);
	IOLockUnlock(lock);
}

extern "C" int switch_gpu_timeline_promote(const switch_gpu_backing_token *token)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	const int result = timeline ? status_locked(timeline, uint32_t(token->value), false) : -19;
	if (result == 1 && int32_t(uint32_t(token->value) - timeline->promoted) > 0)
		timeline->promoted = uint32_t(token->value);
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_has_semantics(void)
{
	return __atomic_load_n(&semantic_count, __ATOMIC_ACQUIRE) != 0;
}

extern "C" int switch_gpu_timeline_physical_status(const switch_gpu_backing_token *token,
						   int external)
{
	IOLockLock(lock);
	const auto *timeline = find(token);
	const int result =
		timeline ? physical_now(timeline, uint32_t(token->value), external != 0) : -19;
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_view_open(const switch_gpu_backing_token *token,
					     switch_gpu_timeline_view *view)
{
	*view = {};
	if (!lock)
		return -19;
	IOLockLock(lock);
	const auto *timeline = find(token);
	const uint32_t value = timeline ? uint32_t(token->value) : 0;
	int result = !timeline ? -19 : failed_locked(timeline, value) ? -5 : 0;
	for (auto *node = result ? nullptr : timeline->semantics; node; node = node->next)
		if (int32_t(value - node->seqno) >= 0)
			result = -16;
	if (!result)
		*view = {timeline, value};
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_view_visible(const switch_gpu_timeline_view *view)
{
	return view->timeline ? physical_now(view->timeline, view->value, true) : -19;
}

extern "C" int switch_gpu_timeline_semantic_snapshot(const switch_gpu_backing_token *token,
						     switch_gpu_event_waits **authority)
{
	*authority = nullptr;
	if (!switch_gpu_timeline_has_semantics())
		return 0;
	IOLockLock(lock);
	const auto *timeline = find(token);
	int result = 0;
	if (!timeline) {
		result = -19;
	} else if (failed_locked(timeline, uint32_t(token->value))) {
		result = -5;
	} else {
		for (auto *node = timeline->semantics; node; node = node->next)
			if (int32_t(uint32_t(token->value) - node->seqno) >= 0) {
				const int semantic =
					switch_gpu_event_waits_cached_status(node->authority);
				if (semantic == 1 ||
				    (semantic < 0 && node->seqno != uint32_t(token->value)))
					continue;
				if (semantic < 0) {
					result = -5;
				} else {
					*authority = node->authority;
					switch_gpu_event_waits_retain(*authority);
					result = 1;
				}
				break;
			}
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_semantic_allocate(switch_gpu_timeline_semantic **out)
{
	if (!out)
		return -22;
	if (*out)
		return 0;
	*out = IONew(switch_gpu_timeline_semantic, 1);
	if (!*out)
		return -12;
	**out = {};
	return 0;
}

extern "C" int switch_gpu_timeline_semantic_reserve(const switch_gpu_backing_token *token,
						    switch_gpu_event_waits *authority,
						    switch_gpu_timeline_semantic **out)
{
	if (!authority || !token || !out)
		return -22;
	if (const int result = switch_gpu_timeline_semantic_allocate(out))
		return result;
	auto *node = *out;
	IOLockLock(lock);
	auto *timeline = find(token);
	int result = timeline ? 0 : -19;
	if (!result && node->timeline) {
		result = node->timeline == timeline && node->authority == authority &&
					 node->seqno == uint32_t(token->value)
				 ? 0
				 : -22;
	} else if (!result) {
		++timeline->refs;
		switch_gpu_event_waits_retain(authority);
		*node = {timeline->semantics, timeline, authority, uint32_t(token->value)};
		timeline->semantics = node;
		__atomic_add_fetch(&semantic_count, 1u, __ATOMIC_RELEASE);
		publish_semantic_locked(timeline);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" void switch_gpu_timeline_semantic_finish(switch_gpu_timeline_semantic **owned,
						    int result)
{
	if (!owned || !*owned)
		return;
	auto *node = *owned;
	*owned = nullptr;
	auto *timeline = node->timeline;
	if (!timeline) {
		IODelete(node, switch_gpu_timeline_semantic, 1);
		return;
	}
	IOLockLock(lock);
	auto **at = &timeline->semantics;
	while (*at && *at != node)
		at = &(*at)->next;
	if (*at)
		*at = node->next;
	__atomic_sub_fetch(&semantic_count, 1u, __ATOMIC_RELEASE);
	if (result < 0 && result != -125)
		fail_locked(timeline, node->seqno);
	publish_semantic_locked(timeline);
	put_locked(timeline);
	IOLockUnlock(lock);
	switch_gpu_event_waits_release(&node->authority);
	IODelete(node, switch_gpu_timeline_semantic, 1);
	switch_gpu_backing_completed();
}

extern "C" void switch_gpu_timeline_semantic_visibility(switch_gpu_timeline *timeline,
							volatile TegraGM20BAbiFenceSemantic *state)
{
	IOLockLock(lock);
	timeline->semantic_public = state;
	publish_semantic_locked(timeline);
	IOLockUnlock(lock);
}
