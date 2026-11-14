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
#ifdef __cplusplus
}
#endif
