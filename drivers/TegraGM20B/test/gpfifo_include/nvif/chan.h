// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
extern unsigned switch_gpu_test_delays, switch_gpu_test_warnings;
#define WARN_ON(x) ((x) ? (++switch_gpu_test_warnings, 1) : 0)

static inline void udelay(unsigned value)
{
	(void)value;
	++switch_gpu_test_delays;
}

struct nvif_map {
	void *ptr;
};

struct nvif_push {
	int (*wait)(struct nvif_push *, u32);
	void (*kick)(struct nvif_push *);

	struct {
		struct {
			struct nvif_map map;
		} object;
	} mem;

	u64 addr;

	struct {
		u32 get, max;
	} hw;

	u32 *bgn, *cur, *seg, *end;
};
struct nvif_chan;

struct nvif_chan_func {
	struct {
		u32 (*read_get)(struct nvif_chan *);
	} push;

	struct {
		u32 (*read_get)(struct nvif_chan *);
		void (*push)(struct nvif_chan *, bool, u64, u32, bool);
		void (*kick)(struct nvif_chan *);
		int (*post)(struct nvif_chan *, u32, u32);
		u32 post_size;
	} gpfifo;
};

struct nvif_chan {
	const struct nvif_chan_func *func;

	struct {
		struct nvif_map map;
	} userd;

	struct {
		struct nvif_map map;
		u32 cur, max;
		int free;
	} gpfifo;
	struct nvif_push push;
	bool defer_kick;
};

static inline int PUSH_KICK(struct nvif_push *p)
{
	if (p->cur != p->bgn) {
		p->kick(p);
		p->bgn = p->cur;
	}
	return 0;
}
#ifdef __cplusplus
extern "C" {
#endif
int nvif_chan_dma_wait(struct nvif_chan *, u32);
void nvif_chan_gpfifo_ctor(const struct nvif_chan_func *, void *, void *, u32, void *, u64, u32,
			   struct nvif_chan *);
int nvif_chan_gpfifo_wait(struct nvif_chan *, u32, u32);
void nvif_chan_gpfifo_push(struct nvif_chan *, u64, u32, bool);
int nvif_chan_gpfifo_post(struct nvif_chan *);

#ifdef __cplusplus
}
#endif
