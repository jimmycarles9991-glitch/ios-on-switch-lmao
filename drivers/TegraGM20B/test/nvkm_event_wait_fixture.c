/* SPDX-License-Identifier: MIT */
#include <core/event.h>
#include "nvkm_event_wait_fixture.h"

struct event_wait_member {
	struct nvkm_event_ntfy notify;
	unsigned calls;
	int resume;
};

struct event_wait_fixture {
	struct nvkm_event event;
	struct event_wait_member members[64];
};

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
	return f;
}

void event_wait_destroy(struct event_wait_fixture *f)
{
	for (unsigned i = 0; i < 64; i++)
		nvkm_event_ntfy_del(&f->members[i].notify);
	nvkm_event_fini(&f->event);
	pthread_rwlock_destroy(&f->event.list_lock);
	pthread_mutex_destroy(&f->event.refs_lock);
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
	nvkm_event_ntfy(&f->event, 0, 1);
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
	return f->members[i].calls;
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
