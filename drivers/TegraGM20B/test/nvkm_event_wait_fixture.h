/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct event_wait_fixture;
struct event_wait_fixture *event_wait_create(void);
void event_wait_destroy(struct event_wait_fixture *);
void event_wait_add(struct event_wait_fixture *, unsigned index, int wait);
void event_wait_allow(struct event_wait_fixture *, unsigned index);
void event_wait_dispatch(struct event_wait_fixture *);
unsigned event_wait_members(struct event_wait_fixture *);
unsigned event_wait_calls(struct event_wait_fixture *, unsigned index);
unsigned event_wait_refcount(struct event_wait_fixture *);
int event_wait_suspend(struct event_wait_fixture *, unsigned index);
void event_wait_resume(struct event_wait_fixture *, unsigned index);
void event_wait_remove(struct event_wait_fixture *, unsigned index);
int switch_event_deferred_add(struct event_wait_fixture *, int wait, int fail_allocation);
void switch_event_deferred_allow(struct event_wait_fixture *);
void switch_event_deferred_block(struct event_wait_fixture *);
void switch_event_deferred_fini(struct event_wait_fixture *);
void switch_event_deferred_init(struct event_wait_fixture *);
void switch_event_deferred_remove(struct event_wait_fixture *);
void switch_event_deferred_run(struct event_wait_fixture *);
void switch_event_deferred_hook(struct event_wait_fixture *, void (*)(void *), void *);
void switch_event_deferred_wait_draining(struct event_wait_fixture *);
unsigned switch_event_deferred_allocations(struct event_wait_fixture *);
unsigned switch_event_deferred_frees(struct event_wait_fixture *);
unsigned switch_event_deferred_queued(struct event_wait_fixture *);
unsigned switch_event_deferred_inside_dispatch(struct event_wait_fixture *);
unsigned switch_event_deferred_callback_members(struct event_wait_fixture *);
unsigned switch_event_deferred_drain_members(struct event_wait_fixture *);
#ifdef __cplusplus
}
#endif
