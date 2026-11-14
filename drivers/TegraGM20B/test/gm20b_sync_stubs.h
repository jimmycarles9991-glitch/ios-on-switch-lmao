// SPDX-License-Identifier: MIT
// gpu_sync.c on the host: the live channel list, its locks and the wake word it sleeps on, each lock a real mutex the
// test can see held.
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef unsigned int gfp_t;

#define GFP_KERNEL 0x01u
#define NSEC_PER_MSEC 1000000ULL
#define TGM_WAITERS 128
#define TGM_SYNC_MAX 512
#define TGM_FENCE_BYTES ((size_t)65 * 16 * 1024)
#define TGM_ISOLATE_NS (500 * NSEC_PER_MSEC)

#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define upper_32_bits(n) ((u32)((n) >> 32))
#define lower_32_bits(n) ((u32)(n))

struct list_head {
	struct list_head *next, *prev;
};

#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define list_for_each_entry(pos, head, member)                               \
	for ((pos) = container_of((head)->next, __typeof__(*(pos)), member); \
	     &(pos)->member != (head);                                       \
	     (pos) = container_of((pos)->member.next, __typeof__(*(pos)), member))

struct mutex {
	void *impl;
};

#define DEFINE_MUTEX(name) struct mutex name = {0}
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

void *kmalloc_array(size_t n, size_t size, gfp_t flags);
void kfree(const void *p);
void nvxnu_log(const char *format, ...);

struct nvif_push {
	u32 *cur;
};

void switch_sync_push(struct nvif_push *push);
#define PUSH_MTHD(push, ...) switch_sync_push(push)
#define PUSH_KICK(push) switch_sync_push(push)

struct tgm_vm {
	u64 fences_va, bind_va;
	u32 bind_target;
	const volatile u32 *bind_cpu;
};

struct tgm_order {
	u64 va;
	const volatile u32 *cpu;
	u64 id;
	u32 seqno;
	u32 chid;
};

struct tgm_chan {
	struct tgm_vm *vm;

	struct {
		struct nvif_push push;
	} chan;
	struct list_head live;
	u64 id, fault_va, killed_ns;
	volatile u32 *fence_cpu;
	u32 submitted, error, chid, norder;
	int kernel, high;
	void *group;
	struct tgm_order order[TGM_ORDER_MAX];
	struct tgm_order waited;
};

struct fences_state {
	void *va;
};

struct gpu_state {
	int stage;
	struct nvkm_device *device;
};

enum tgm_hang {
	TGM_HANG_STUCK,
	TGM_HANG_RECOVERING,
	TGM_HANG_PROGRESSING,
};

extern struct fences_state tgm_fence_pages;
extern struct gpu_state tgm_gpu;
extern struct list_head tgm_live_chans;
extern struct mutex tgm_live_lock;

u32 nvxnu_wake_seq(void);
u64 nvxnu_nanotime(void);
int nvxnu_idle(u32 seq, u64 deadline_ns);
void nvxnu_wake_due(u32 seq, const char *what, u32 seqno);
void nvxnu_dcache_clean_inval(const void *va, u64 size);
void nvxnu_dcache_clean_inval_pages(const u64 *pas, u32 count);
void tgm_chan_need_id(u64 id, u32 seqno);
int tgm_chan_announced_id(u64 id, u32 seqno);
int tgm_chan_announced_uid(u32 uid, u32 seqno);
void tgm_chan_wanted(u32 uid, u32 seqno);
enum tgm_hang tgm_hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns);
void tgm_hold_need(struct nvkm_device *device, unsigned int chid);
int vm_imports(struct tgm_vm *vm, u64 pa);

static inline struct tgm_chan *live_by(u64 id, int chid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (id ? chan->id == id : (int)chan->chid == chid)
			return chan;
	}
	return NULL;
}

static inline struct tgm_chan *live_by_uid(u32 uid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if ((u32)chan->id == uid)
			return chan;
	}
	return NULL;
}

u32 tgm_chan_completed(struct tgm_chan *chan);

static inline int sync_done(u64 id, u32 seqno)
{
	struct tgm_chan *chan = NULL;
	int done = 1;

	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (chan->id == id) {
			done = chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
			break;
		}
	}
	mutex_unlock(&tgm_live_lock);
	return done;
}

#ifdef __cplusplus
}
#endif
