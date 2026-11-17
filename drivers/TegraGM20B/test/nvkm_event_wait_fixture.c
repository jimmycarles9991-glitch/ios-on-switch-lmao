/* SPDX-License-Identifier: MIT */
#include <core/event.h>
#include <core/client.h>
#include <nvif/if000e.h>
#include "nvkm_event_wait_fixture.h"

struct event_wait_member {
	struct nvkm_event_ntfy notify;
	unsigned calls;
	int resume;
};

struct event_wait_fixture {
	struct nvkm_event event;
	struct event_wait_member members[64];
	struct nvkm_object parent;
	struct nvkm_object *uevent;
	pthread_mutex_t work_lock;
	pthread_cond_t work_changed;
	struct switch_event_work_call *call;
	bool fail_allocation, queued, running, draining;
	unsigned allocations, frees, dispatching, inside_dispatch, callback_members, drain_members;
	void (*hook)(void *);
	void *hook_context;
};

struct switch_event_work_call {
	struct event_wait_fixture *fixture;
	struct work_struct *work;
};

static _Thread_local struct event_wait_fixture *switch_event_creating;

void switch_event_work_init(struct work_struct *work, void (*func)(struct work_struct *))
{
	struct event_wait_fixture *f = switch_event_creating;
	assert(f && !f->call);
	work->func = func;
	work->thread_call = NULL;
	if (f->fail_allocation)
		return;
	f->call = calloc(1, sizeof(*f->call));
	assert(f->call);
	f->call->fixture = f;
	f->call->work = work;
	f->allocations++;
	work->thread_call = f->call;
}

bool schedule_work(struct work_struct *work)
{
	struct switch_event_work_call *call = work->thread_call;
	struct event_wait_fixture *f = call->fixture;
	pthread_mutex_lock(&f->work_lock);
	const bool queued = f->queued;
	f->queued = true;
	pthread_cond_broadcast(&f->work_changed);
	pthread_mutex_unlock(&f->work_lock);
	return !queued;
}

static void switch_event_work_run_locked(struct event_wait_fixture *f)
{
	struct work_struct *work = f->call->work;
	assert(f->queued && !f->running);
	f->queued = false;
	f->running = true;
	pthread_mutex_unlock(&f->work_lock);
	work->func(work);
	pthread_mutex_lock(&f->work_lock);
	f->running = false;
	pthread_cond_broadcast(&f->work_changed);
}

bool flush_work(struct work_struct *work)
{
	struct switch_event_work_call *call = work->thread_call;
	struct event_wait_fixture *f = call->fixture;
	const unsigned members = event_wait_members(f);
	pthread_mutex_lock(&f->work_lock);
	const bool pending = f->queued || f->running;
	f->drain_members += members;
	f->draining = true;
	pthread_cond_broadcast(&f->work_changed);
	while (f->queued || f->running) {
		if (f->running)
			pthread_cond_wait(&f->work_changed, &f->work_lock);
		else
			switch_event_work_run_locked(f);
	}
	pthread_mutex_unlock(&f->work_lock);
	return pending;
}

void switch_linux_call_free(void *opaque)
{
	struct switch_event_work_call *call = opaque;
	if (!call)
		return;
	struct event_wait_fixture *f = call->fixture;
	assert(event_wait_members(f) == 0);
	pthread_mutex_lock(&f->work_lock);
	assert(!f->queued && !f->running);
	f->frees++;
	f->call = NULL;
	pthread_mutex_unlock(&f->work_lock);
	free(call);
}

static int callback(struct nvkm_event_ntfy *notify, u32 bits)
{
	struct event_wait_member *member = container_of(notify, struct event_wait_member, notify);
	(void)bits;
	member->calls++;
	return NVKM_EVENT_KEEP;
}

struct event_wait_fixture *event_wait_create(void)
{
	static const struct nvkm_event_func ops = {0};
	struct event_wait_fixture *f = calloc(1, sizeof(*f));
	assert(f && !nvkm_event_init(&ops, NULL, 1, 1, &f->event));
	pthread_mutex_init(&f->work_lock, NULL);
	pthread_cond_init(&f->work_changed, NULL);
	return f;
}

void event_wait_destroy(struct event_wait_fixture *f)
{
	switch_event_deferred_remove(f);
	for (unsigned i = 0; i < 64; i++)
		nvkm_event_ntfy_del(&f->members[i].notify);
	nvkm_event_fini(&f->event);
	pthread_rwlock_destroy(&f->event.list_lock);
	pthread_mutex_destroy(&f->event.refs_lock);
	pthread_cond_destroy(&f->work_changed);
	pthread_mutex_destroy(&f->work_lock);
	free(f);
}

void event_wait_add(struct event_wait_fixture *f, unsigned i, int wait)
{
	assert(i < 64);
	nvkm_event_ntfy_add(&f->event, 0, 1, wait, callback, &f->members[i].notify);
}

void event_wait_allow(struct event_wait_fixture *f, unsigned i)
{
	nvkm_event_ntfy_allow(&f->members[i].notify);
}

void event_wait_dispatch(struct event_wait_fixture *f)
{
	__atomic_store_n(&f->dispatching, 1, __ATOMIC_RELEASE);
	nvkm_event_ntfy(&f->event, 0, 1);
	__atomic_store_n(&f->dispatching, 0, __ATOMIC_RELEASE);
}

unsigned event_wait_members(struct event_wait_fixture *f)
{
	unsigned count = 0;
	pthread_rwlock_rdlock(&f->event.list_lock);
	for (struct list_head *p = f->event.ntfy.next; p != &f->event.ntfy; p = p->next)
		count++;
	pthread_rwlock_unlock(&f->event.list_lock);
	return count;
}

unsigned event_wait_calls(struct event_wait_fixture *f, unsigned i)
{
	return __atomic_load_n(&f->members[i].calls, __ATOMIC_ACQUIRE);
}

unsigned event_wait_refcount(struct event_wait_fixture *f)
{
	return f->event.refs[0];
}

int event_wait_suspend(struct event_wait_fixture *f, unsigned i)
{
	return f->members[i].resume = nvkm_event_ntfy_suspend(&f->members[i].notify);
}

void event_wait_resume(struct event_wait_fixture *f, unsigned i)
{
	if (f->members[i].resume)
		nvkm_event_ntfy_allow(&f->members[i].notify);
}

void event_wait_remove(struct event_wait_fixture *f, unsigned i)
{
	nvkm_event_ntfy_del(&f->members[i].notify);
}

static int switch_event_deferred_callback(struct nvkm_object *parent, u64 token, u32 bits)
{
	struct event_wait_fixture *f = container_of(parent, struct event_wait_fixture, parent);
	(void)token;
	(void)bits;
	__atomic_fetch_add(&f->members[0].calls, 1, __ATOMIC_RELEASE);
	f->inside_dispatch += __atomic_load_n(&f->dispatching, __ATOMIC_ACQUIRE);
	f->callback_members = event_wait_members(f);
	if (f->hook)
		f->hook(f->hook_context);
	return NVKM_EVENT_KEEP;
}

static int switch_event_deferred_parent(struct nvkm_object *parent, void *args, u32 argc,
					struct nvkm_uevent *uevent)
{
	struct event_wait_fixture *f = container_of(parent, struct event_wait_fixture, parent);
	(void)args;
	(void)argc;
	return nvkm_uevent_add_deferred(uevent, &f->event, 0, 1, switch_event_deferred_callback);
}

int switch_event_deferred_add(struct event_wait_fixture *f, int wait, int fail_allocation)
{
	static const struct nvkm_object_func funcs = {.uevent = switch_event_deferred_parent};
	const struct nvkm_oclass oclass = {.parent = &f->parent};
	union nvif_event_args args = {.v0.wait = wait};
	assert(!f->uevent);
	f->parent.func = &funcs;
	f->fail_allocation = fail_allocation;
	switch_event_creating = f;
	const int ret = nvkm_uevent_new(&oclass, &args, sizeof(args.v0), &f->uevent);
	switch_event_creating = NULL;
	if (ret)
		switch_event_deferred_remove(f);
	return ret;
}

void switch_event_deferred_allow(struct event_wait_fixture *f)
{
	assert(!f->uevent->func->mthd(f->uevent, NVIF_EVENT_V0_ALLOW, NULL, 0));
}

void switch_event_deferred_block(struct event_wait_fixture *f)
{
	assert(!f->uevent->func->mthd(f->uevent, NVIF_EVENT_V0_BLOCK, NULL, 0));
}

void switch_event_deferred_fini(struct event_wait_fixture *f)
{
	assert(!f->uevent->func->fini(f->uevent, NVKM_SUSPEND));
}

void switch_event_deferred_init(struct event_wait_fixture *f)
{
	assert(!f->uevent->func->init(f->uevent));
}

void switch_event_deferred_remove(struct event_wait_fixture *f)
{
	if (f->uevent) {
		free(f->uevent->func->dtor(f->uevent));
		f->uevent = NULL;
	}
}

void switch_event_deferred_run(struct event_wait_fixture *f)
{
	pthread_mutex_lock(&f->work_lock);
	if (f->queued)
		switch_event_work_run_locked(f);
	pthread_mutex_unlock(&f->work_lock);
}

void switch_event_deferred_hook(struct event_wait_fixture *f, void (*hook)(void *), void *context)
{
	f->hook = hook;
	f->hook_context = context;
}

void switch_event_deferred_wait_draining(struct event_wait_fixture *f)
{
	pthread_mutex_lock(&f->work_lock);
	while (!f->draining)
		pthread_cond_wait(&f->work_changed, &f->work_lock);
	pthread_mutex_unlock(&f->work_lock);
}

unsigned switch_event_deferred_allocations(struct event_wait_fixture *f)
{
	return f->allocations;
}

unsigned switch_event_deferred_frees(struct event_wait_fixture *f)
{
	pthread_mutex_lock(&f->work_lock);
	const unsigned frees = f->frees;
	pthread_mutex_unlock(&f->work_lock);
	return frees;
}

unsigned switch_event_deferred_queued(struct event_wait_fixture *f)
{
	pthread_mutex_lock(&f->work_lock);
	const bool queued = f->queued;
	pthread_mutex_unlock(&f->work_lock);
	return queued;
}

unsigned switch_event_deferred_inside_dispatch(struct event_wait_fixture *f)
{
	return f->inside_dispatch;
}

unsigned switch_event_deferred_callback_members(struct event_wait_fixture *f)
{
	return f->callback_members;
}

unsigned switch_event_deferred_drain_members(struct event_wait_fixture *f)
{
	return f->drain_members;
}
