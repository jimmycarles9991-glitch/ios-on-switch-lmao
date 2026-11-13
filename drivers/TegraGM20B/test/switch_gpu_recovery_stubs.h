// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef int atomic_t;

static inline int atomic_read(const atomic_t *p)
{
	return *p;
}

static inline int atomic_xchg(atomic_t *p, int value)
{
	int old = *p;
	*p = value;
	return old;
}

static inline int atomic_cmpxchg(atomic_t *p, int old, int value)
{
	int found = *p;
	if (found == old)
		*p = value;
	return found;
}
struct nvkm_runl;
struct nvkm_cgrp;
struct nvkm_chan;
struct nvkm_engn;
struct nvkm_runq;

struct nvkm_engine {
	struct {
		void *device;
	} subdev;
};

struct nvkm_fifo {
	struct nvkm_engine engine;
};

struct nvkm_event {
	int unused;
};

struct nvkm_chid {
	int nr;
	struct nvkm_event event;
	struct nvkm_chan *data[8];
};

struct nvkm_chan {
	struct nvkm_cgrp *cgrp;
	struct nvkm_chan *next;
	int id;
	atomic_t errored;
};

struct nvkm_cgrp {
	struct nvkm_runl *runl;
	struct nvkm_cgrp *next;
	struct nvkm_chan *chans;
	atomic_t rc;
};

struct nvkm_engn_func {
	int (*cxid)(struct nvkm_engn *, bool *);
};

struct nvkm_engn {
	const struct nvkm_engn_func *func;
	struct nvkm_runl *runl;
	struct nvkm_engine *engine;
	struct nvkm_engn *next;
	int channel;
};

struct nvkm_runq_func {
	bool (*idle)(struct nvkm_runq *);
};

struct nvkm_runq {
	const struct nvkm_runq_func *func;
};

struct nvkm_runl_func {
	void (*preempt)(struct nvkm_runl *);
	int (*update)(struct nvkm_runl *);
	void (*fault_clear)(struct nvkm_runl *);
	int (*wait)(struct nvkm_runl *);
	bool (*preempt_pending)(struct nvkm_runl *);
};

struct nvkm_runl {
	const struct nvkm_runl_func *func;
	struct nvkm_fifo *fifo;
	struct nvkm_chid *chid;
	struct nvkm_cgrp *cgrps;
	struct nvkm_engn *engns;
	struct nvkm_runq *runq[2];
	int runq_nr;
	bool mutex;
	atomic_t rc_pending;
};

#define NVKM_CGRP_RC_NONE 0
#define NVKM_CGRP_RC_PENDING 1
#define NVKM_CGRP_RC_RUNNING 2
#define NVKM_CHAN_EVENT_ERRORED 1
#define mutex_is_locked(p) (*(p))
void switch_gpu_recovery_abort(const char *);
#define BUG_ON(x)                                      \
	do {                                           \
		if (x)                                 \
			switch_gpu_recovery_abort(#x); \
	} while (0)
#define WARN_ON(x) ((void)(x))
#define ENGN_DEBUG(...) ((void)0)
#define nvkm_msec(device, ms, ...)               \
	({                                       \
		(void)(device);                  \
		(void)(ms);                      \
		int result = 0;                  \
		do {                             \
			__VA_ARGS__ result = -1; \
		} while (0);                     \
		result;                          \
	})
#define nvkm_runl_foreach_cgrp_safe(c, tmp, r) \
	for ((c) = (r)->cgrps; (c) && (((tmp) = (c)->next), 1); (c) = (tmp))
#define nvkm_cgrp_foreach_chan_safe(c, tmp, g) \
	for ((c) = (g)->chans; (c) && (((tmp) = (c)->next), 1); (c) = (tmp))
#define nvkm_runl_foreach_engn_cond(e, r, cond)      \
	for ((e) = (r)->engns; (e); (e) = (e)->next) \
		if (cond)
struct nvkm_chan *nvkm_runl_chan_get_chid(struct nvkm_runl *, int, unsigned long *);
struct nvkm_cgrp *nvkm_runl_cgrp_get_cgid(struct nvkm_runl *, int, unsigned long *);
void nvkm_chan_put(struct nvkm_chan **, unsigned long);
void nvkm_cgrp_put(struct nvkm_cgrp **, unsigned long);
void nvkm_chan_error(struct nvkm_chan *, bool);
void nvkm_chan_remove_locked(struct nvkm_chan *);
int nvkm_engine_reset(struct nvkm_engine *);
void tgm_recover_reset_hold(struct nvkm_engine *);
void tgm_recover_reset_release(struct nvkm_engine *);
void tgm_recover_preempt_unblock(struct nvkm_runl *);
void nvkm_runl_allow(struct nvkm_runl *);
void nvkm_event_ntfy(struct nvkm_event *, int, int);
void nvkm_runl_rc(struct nvkm_runl *);
#ifdef __cplusplus
}
#endif
