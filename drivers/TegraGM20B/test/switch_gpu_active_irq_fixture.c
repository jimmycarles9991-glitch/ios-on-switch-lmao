/* SPDX-License-Identifier: MIT */
#include <core/event.h>
#include "switch_gpu_active_irq_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuActiveIrq.h"

struct switch_irq_member {
	struct nvkm_event_ntfy notify;
	uint64_t state;
	uint32_t completed;
	unsigned calls;
	int resume;
};

struct switch_irq_fixture {
	struct nvkm_event event;
	struct switch_irq_member members[64];
	switch_irq_hook dropping;
	void *context;
};

static int callback(struct nvkm_event_ntfy *notify, u32 bits)
{
	struct switch_irq_member *member = container_of(notify, struct switch_irq_member, notify);
	struct switch_irq_fixture *f =
		container_of(notify->event, struct switch_irq_fixture, event);
	(void)bits;
	__atomic_add_fetch(&member->calls, 1, __ATOMIC_RELAXED);
	if (!switch_gpu_irq_complete(&member->state,
				     __atomic_load_n(&member->completed, __ATOMIC_ACQUIRE)))
		return NVKM_EVENT_KEEP;
	if (f->dropping)
		f->dropping(f->context);
	return NVKM_EVENT_DROP;
}

struct switch_irq_fixture *switch_irq_create(void)
{
	static const struct nvkm_event_func ops = {0};
	struct switch_irq_fixture *f = calloc(1, sizeof(*f));
	assert(f && !nvkm_event_init(&ops, NULL, 1, 1, &f->event));
	return f;
}

void switch_irq_destroy(struct switch_irq_fixture *f)
{
	for (unsigned i = 0; i < 64; i++)
		nvkm_event_ntfy_del(&f->members[i].notify);
	nvkm_event_fini(&f->event);
	pthread_rwlock_destroy(&f->event.list_lock);
	pthread_mutex_destroy(&f->event.refs_lock);
	free(f);
}

void switch_irq_add(struct switch_irq_fixture *f, unsigned i, int active)
{
	assert(i < 64);
	nvkm_event_ntfy_add(&f->event, 0, 1, active, callback, &f->members[i].notify);
}

void switch_irq_arm(struct switch_irq_fixture *f, unsigned i, uint32_t sequence)
{
	if (switch_gpu_irq_arm(&f->members[i].state, sequence))
		nvkm_event_ntfy_allow(&f->members[i].notify);
}

void switch_irq_done(struct switch_irq_fixture *f, unsigned i, uint32_t sequence)
{
	__atomic_store_n(&f->members[i].completed, sequence, __ATOMIC_RELEASE);
}

void switch_irq_dispatch(struct switch_irq_fixture *f, int active)
{
	if (active)
		nvkm_event_ntfy_active(&f->event, 0, 1);
	else
		nvkm_event_ntfy(&f->event, 0, 1);
}

unsigned switch_irq_members(struct switch_irq_fixture *f)
{
	unsigned count = 0;
	pthread_rwlock_rdlock(&f->event.list_lock);
	for (struct list_head *p = f->event.ntfy.next; p != &f->event.ntfy; p = p->next)
		count++;
	pthread_rwlock_unlock(&f->event.list_lock);
	return count;
}

unsigned switch_irq_calls(struct switch_irq_fixture *f, unsigned i)
{
	return __atomic_load_n(&f->members[i].calls, __ATOMIC_ACQUIRE);
}

unsigned switch_irq_refcount(struct switch_irq_fixture *f)
{
	return f->event.refs[0];
}

void switch_irq_hook_drop(struct switch_irq_fixture *f, switch_irq_hook hook, void *context)
{
	f->dropping = hook;
	f->context = context;
}

int switch_irq_suspend(struct switch_irq_fixture *f, unsigned i)
{
	return f->members[i].resume = nvkm_event_ntfy_suspend(&f->members[i].notify);
}

void switch_irq_resume(struct switch_irq_fixture *f, unsigned i)
{
	if (f->members[i].resume)
		nvkm_event_ntfy_allow(&f->members[i].notify);
}

void switch_irq_remove(struct switch_irq_fixture *f, unsigned i)
{
	nvkm_event_ntfy_del(&f->members[i].notify);
}
