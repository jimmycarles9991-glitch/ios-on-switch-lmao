// SPDX-License-Identifier: MIT
// sched.c and deadline.c on the host: nvkm's runlist, channel and register calls they make, each register read counted.
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int atomic_t;

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->next = head->prev = head;
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	entry->prev = head->prev;
	entry->next = head;
	head->prev->next = entry;
	head->prev = entry;
}

static inline void list_rotate_left(struct list_head *head)
{
	struct list_head *first = head->next;

	if (first == head || first->next == head)
		return;
	first->prev->next = first->next;
	first->next->prev = first->prev;
	list_add_tail(first, head);
}

#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define list_for_each_entry(pos, head, member)                               \
	for ((pos) = container_of((head)->next, __typeof__(*(pos)), member); \
	     &(pos)->member != (head);                                       \
	     (pos) = container_of((pos)->member.next, __typeof__(*(pos)), member))

struct mutex {
	int held;
};

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int mutex_trylock(struct mutex *m);

typedef struct {
	int held;
} spinlock_t;

#define spin_lock_init(l) ((l)->held = 0)
void spin_lock_irqsave_stub(spinlock_t *l);
void spin_unlock_irqrestore_stub(spinlock_t *l);
#define spin_lock_irqsave(l, f) ((void)(f), spin_lock_irqsave_stub(l))
#define spin_unlock_irqrestore(l, f) ((void)(f), spin_unlock_irqrestore_stub(l))

/* The shim's are single-copy-atomic volatile accesses (nvif/os.h); here relaxed atomics, which ThreadSanitizer reads alike */
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define atomic_set(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define atomic_read(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define scnprintf snprintf

static inline int test_bit(unsigned long nr, const unsigned long *addr)
{
	return (int)((addr[nr / 64] >> (nr % 64)) & 1);
}

enum {
	NVKM_ENGINE_GR = 1,
	NVKM_ENGINE_CE = 2
};

enum {
	NVKM_MEM_TARGET_INST = 1
};

struct nvkm_device;
struct nvkm_fifo;

struct nvkm_memory {
	int unused;
};

struct nvkm_timer {
	int unused;
};

struct nvkm_subdev {
	int type;
	struct nvkm_device *device;
};

struct nvkm_engine {
	struct nvkm_subdev subdev;
};

struct nvkm_chan;
struct nvkm_cgrp;
struct nvkm_runl;

struct nvkm_chan_func {
	void (*preempt)(struct nvkm_chan *);
	void (*stop)(struct nvkm_chan *);
	void (*start)(struct nvkm_chan *);
};

struct nvkm_fifo_func {
	int (*runl_ctor)(struct nvkm_fifo *);

	struct {
		const struct nvkm_chan_func *func;
	} chan;
};

struct nvkm_fifo {
	const struct nvkm_fifo_func *func;
	struct nvkm_engine engine;
	struct list_head runls;

	struct {
		unsigned chan_msec;
	} timeout;
};

struct nvkm_device {
	struct nvkm_fifo *fifo;
	struct nvkm_timer *timer;
};

struct nvkm_chid {
	int nr;
	unsigned long used[8];
};

struct nvkm_runl_func {
	u32 size;
	int (*update)(struct nvkm_runl *);
	bool (*pending)(struct nvkm_runl *);
	int (*wait)(struct nvkm_runl *);
	void (*insert_cgrp)(struct nvkm_cgrp *, struct nvkm_memory *, u64);
	void (*insert_chan)(struct nvkm_chan *, struct nvkm_memory *, u64);
	void (*commit)(struct nvkm_runl *, struct nvkm_memory *, u32, int);
};

struct nvkm_runq {
	int id;
};

struct nvkm_engn_func {
	int (*cxid)(void);
};

struct nvkm_engn {
	int id;
	struct nvkm_engine *engine;
	const struct nvkm_engn_func *func;
	struct list_head head;
};

struct nvkm_runl {
	int id;
	const struct nvkm_runl_func *func;
	struct nvkm_fifo *fifo;
	struct nvkm_chid *chid, *cgid;
	int cgrp_nr;
	struct list_head cgrps, engns, head;
	struct nvkm_runq *runq[2];
	int runq_nr;
	struct mutex mutex;
	atomic_t changed;
};

struct nvkm_cgrp {
	bool hw;
	int chan_nr;
	struct nvkm_runl *runl;
	struct list_head chans, head;
	atomic_t rc;
};

struct nvkm_chan {
	int id;
	struct nvkm_cgrp *cgrp;
	struct list_head head;
	const struct nvkm_chan_func *func;
	spinlock_t lock;
	atomic_t blocked, errored;
};

/* sched.c tgm_sched_ramfc's channel steps (TegraGM20BRamfcOrder.h): the unit's weak no-ops unless a test defines them */
void nvkm_chan_block(struct nvkm_chan *chan);
void nvkm_chan_allow(struct nvkm_chan *chan);
int nvkm_chan_preempt_locked(struct nvkm_chan *chan, bool wait);

#define nvkm_runl_foreach(runl, fifo) list_for_each_entry((runl), &(fifo)->runls, head)
#define nvkm_runl_foreach_engn(engn, runl) list_for_each_entry((engn), &(runl)->engns, head)
#define nvkm_runl_foreach_engn_cond(engn, runl, cond) nvkm_runl_foreach_engn(engn, runl) if (cond)
#define nvkm_runl_foreach_cgrp(cgrp, runl) list_for_each_entry((cgrp), &(runl)->cgrps, head)
#define nvkm_cgrp_foreach_chan(chan, cgrp) list_for_each_entry((chan), &(cgrp)->chans, head)

/* Every register read the test model counts (sched.c's PFIFO and PBDMA status, PTIMER) */
u32 nvkm_rd32(struct nvkm_device *device, u32 addr);
void nvkm_wr32(struct nvkm_device *device, u32 addr, u32 value);
u64 nvkm_timer_read(struct nvkm_timer *timer);
#define nvkm_wait_msec(device, ms, addr, mask, value) ((void)(device), (void)(ms), 0)
#define nvkm_kmap(mem) ((void)(mem))
#define nvkm_done(mem) ((void)(mem))
int nvkm_memory_new(struct nvkm_device *, int, u64, u32, bool, struct nvkm_memory **);
int nv50_runl_update(struct nvkm_runl *);
void nvkm_runl_update_locked(struct nvkm_runl *, bool);
void nvkm_runl_rc_cgrp(struct nvkm_cgrp *);
bool irqs_disabled(void);
void nvxnu_log(const char *, ...);
void nvxnu_panic(const char *, ...);
u64 nvxnu_nanotime(void);
u64 nvxnu_abstime_to_ns(u64);
/* The model's thread calls: each schedule is counted, and the test runs what it scheduled */
void *nvxnu_work_alloc(void (*fn)(void *), void *arg);
void nvxnu_work_schedule(void *work);
void nvxnu_work_schedule_after(void *work, u64 ns);
void nvxnu_work_cancel_wait(void *work);
void nvxnu_work_cancel(void *work);
/* The GPU interrupt work loop's gate (shim/xnu_glue.cpp) */
void nvxnu_pass_run(void (*fn)(void *), void *arg);
int nvxnu_pass_try(void (*fn)(void *), void *arg);
struct nvkm_chan *nvkm_runl_chan_get_chid(struct nvkm_runl *runl, int id, unsigned long *flags);
void nvkm_chan_put(struct nvkm_chan **chan, unsigned long flags);

#ifdef __cplusplus
}
#endif
