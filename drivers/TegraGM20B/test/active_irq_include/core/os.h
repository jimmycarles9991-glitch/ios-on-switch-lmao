/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct {
	int value;
} atomic_t;

#define atomic_set(p, v) __atomic_store_n(&(p)->value, (v), __ATOMIC_RELEASE)
#define atomic_read(p) __atomic_load_n(&(p)->value, __ATOMIC_ACQUIRE)
#define atomic_xchg(p, v) __atomic_exchange_n(&(p)->value, (v), __ATOMIC_ACQ_REL)
typedef pthread_mutex_t spinlock_t;
typedef pthread_rwlock_t rwlock_t;
#define spin_lock_init(p) pthread_mutex_init(p, NULL)
#define rwlock_init(p) pthread_rwlock_init(p, NULL)
#define spin_lock_irqsave(p, f) ((f) = 0, pthread_mutex_lock(p))
#define spin_unlock_irqrestore(p, f) ((void)(f), pthread_mutex_unlock(p))
#define read_lock_irqsave(p, f) ((f) = 0, pthread_rwlock_rdlock(p))
#define read_unlock_irqrestore(p, f) ((void)(f), pthread_rwlock_unlock(p))
#define write_lock_irqsave(p, f) ((f) = 0, pthread_rwlock_wrlock(p))
#define write_unlock_irqrestore(p, f) ((void)(f), pthread_rwlock_unlock(p))
#define write_lock_irq(p) pthread_rwlock_wrlock(p)
#define write_unlock_irq(p) pthread_rwlock_unlock(p)
#define assert_spin_locked(p) ((void)(p))
#define WARN_ON(v) (v)
#define __ffs(v) __builtin_ctz(v)
#define array3_size(a, b, c) ((size_t)(a) * (b) * (c))
#define kzalloc(n, f) calloc(1, n)
#define kfree(p) free(p)
#define GFP_KERNEL 0
#define __always_inline inline __attribute__((always_inline))

struct list_head {
	struct list_head *next, *prev;
};

#define INIT_LIST_HEAD(p) ((p)->next = (p), (p)->prev = (p))

static inline void list_add_tail(struct list_head *n, struct list_head *h)
{
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

static inline void list_del_init(struct list_head *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	INIT_LIST_HEAD(n);
}

#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each_entry_safe(p, n, h, m)               \
	for (p = list_entry((h)->next, __typeof__(*p), m), \
	    n = list_entry(p->m.next, __typeof__(*p), m);  \
	     &p->m != (h); p = n, n = list_entry(n->m.next, __typeof__(*n), m))
