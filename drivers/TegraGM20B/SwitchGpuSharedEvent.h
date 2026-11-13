// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif

struct switch_gpu_event;
struct switch_gpu_event_signal;
struct switch_gpu_event_watch;
struct switch_gpu_event_waits;
struct switch_gpu_event_use;

/* Borrowed during admission. Accepted publication consumes/nulls signal entries; failure leaves them for abort. */
struct switch_gpu_event_submission {
	const struct switch_gpu_backing_token *dependencies;
	uint32_t dependency_count;
	struct switch_gpu_event_waits *waits;
	struct switch_gpu_event_signal **signals;
	uint32_t signal_count;
};

enum {
	SWITCH_GPU_EVENT_MAX_JOB_USES = 32,
	SWITCH_GPU_EVENT_MAX_DEPENDENCIES = 1024,
	SWITCH_GPU_EVENT_MAX_SOURCES = SWITCH_GPU_EVENT_MAX_DEPENDENCIES +
		SWITCH_GPU_EVENT_MAX_JOB_USES
};

int switch_gpu_event_acquire(void *task, uint32_t port, struct switch_gpu_event **event);
void switch_gpu_event_retain(struct switch_gpu_event *event);
void switch_gpu_event_release(struct switch_gpu_event **event);
uint64_t switch_gpu_event_value(struct switch_gpu_event *event);

/* 1: already signaled; 2: retained exact producer in token; 0: no published producer yet. */
int switch_gpu_event_resolve(struct switch_gpu_event *event, uint64_t value,
			     struct switch_gpu_backing_token *token);

/* The physical token is borrowed from use until use_release; authority preserves transitive producer errors. */
int switch_gpu_event_resolve_use(struct switch_gpu_event *event, uint64_t value,
				 struct switch_gpu_backing_token *token,
				 struct switch_gpu_event_use **use);
void switch_gpu_event_use_release(struct switch_gpu_event_use **use);

/* Preparation allocates before engine publication. Bind the actual reserved completion token before kick; commit
 * only once that kick is accepted. Commit consumes the reservation; abort consumes any uncommitted reservation. */
int switch_gpu_event_signal_prepare(struct switch_gpu_event *event, uint64_t value,
				    struct switch_gpu_event_signal **signal);
int switch_gpu_event_signal_bind(struct switch_gpu_event_signal *signal,
				 const struct switch_gpu_backing_token *token);
int switch_gpu_event_signal_inherit(struct switch_gpu_event_signal *signal,
				    struct switch_gpu_event_waits *waits);
void switch_gpu_event_signal_commit(struct switch_gpu_event_signal **signal);
void switch_gpu_event_signal_abort(struct switch_gpu_event_signal **signal);

/* One callback owns the transferred context reference. Readiness and cancellation race to consume it; cancel does
 * not send a second callback if readiness already won. The watch handle remains caller-owned until cancel/release. */
int switch_gpu_event_observe(struct switch_gpu_event *event, uint64_t value, uint64_t cookie,
			     void (*completed)(void *, uint64_t, int), void *context,
			     struct switch_gpu_event_watch **watch);
/* Terminal observers retain the first selected producer authority through success/failure. */
int switch_gpu_event_terminal(struct switch_gpu_event *event, uint64_t value);
int switch_gpu_event_observe_terminal(struct switch_gpu_event *event, uint64_t value,
				      uint64_t cookie, void (*completed)(void *, uint64_t, int),
				      void *context, struct switch_gpu_event_watch **watch);
void switch_gpu_event_observe_cancel(struct switch_gpu_event_watch **watch);

/* Producer semaphore storage remains pinned until the consumer has retired, including event-only jobs. */
int switch_gpu_event_waits_prepare(struct switch_gpu_event_use *const *uses, uint32_t count,
				   struct switch_gpu_event_waits **waits);
int switch_gpu_event_waits_prepare_tokens(const struct switch_gpu_backing_token *tokens,
					  uint32_t count, struct switch_gpu_event_waits **out);
int switch_gpu_event_waits_covers_token(struct switch_gpu_event_waits *waits,
					const struct switch_gpu_backing_token *token);
int switch_gpu_event_waits_prepare_dependencies(const struct switch_gpu_backing_token *tokens,
						uint32_t count,
						struct switch_gpu_event_waits **waits);
/* Submission owns both uncommitted batches; retry may extend an already-bound destination before kick. */
int switch_gpu_event_waits_merge_uncommitted(struct switch_gpu_event_waits *destination,
					     struct switch_gpu_event_waits *source);
/* 1: current semantic authority is covered or absent; 0: capture and merge before retry; negative: failed. */
int switch_gpu_event_waits_covers(struct switch_gpu_event_waits *waits,
				  const struct switch_gpu_backing_token *token);
int switch_gpu_event_waits_bind(struct switch_gpu_event_waits *waits,
				const struct switch_gpu_backing_token *consumer);
int switch_gpu_event_waits_completion(struct switch_gpu_event_waits *waits,
				      void (*completed)(void *, int), void *context);
int switch_gpu_event_waits_inherit(struct switch_gpu_event_waits *waits,
				   struct switch_gpu_event_waits *previous);
void switch_gpu_event_waits_retain(struct switch_gpu_event_waits *waits);
void switch_gpu_event_waits_release(struct switch_gpu_event_waits **waits);
int switch_gpu_event_waits_status(struct switch_gpu_event_waits *waits);
/* Retained batches expose terminal truth before their deferred completion callback releases engine nodes. */
int switch_gpu_event_waits_cached_status(const struct switch_gpu_event_waits *waits);
/* Any context, no lock (a host1x filter waiter's read): 1 when the batch ends in success once its consumer is
 * physically complete with success: it already has, or it has no waits of its own and inherits none still out or
 * failed. 0 otherwise. */
int switch_gpu_event_waits_settled_status(const struct switch_gpu_event_waits *waits);
/* deadline_ns uses mach_absolute_time converted to nanoseconds; zero waits without a deadline. */
int switch_gpu_event_waits_wait(struct switch_gpu_event_waits *waits, uint64_t deadline_ns);
void switch_gpu_event_waits_commit(struct switch_gpu_event_waits **waits);
void switch_gpu_event_waits_abort(struct switch_gpu_event_waits **waits);

/* Engine completion publication and native CPU-event notification schedule retirement, never poll producers. */
void switch_gpu_events_completed(void);
#ifdef __cplusplus
}
#endif
