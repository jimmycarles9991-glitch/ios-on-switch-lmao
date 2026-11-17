/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef int atomic_t;

struct nvkm_device {
	u32 control;
};

struct nvkm_runl {
	atomic_t rc_pending;
};

struct nvkm_cgrp {
	struct nvkm_runl *runl;
	bool pending;
};

struct nvkm_chan {
	struct nvkm_cgrp *cgrp;
	bool stopped;
	atomic_t errored;
};

#ifdef __cplusplus
extern "C" {
#endif
u32 switch_gr_fault_mask(struct nvkm_device *, u32, u32, u32);
void switch_gr_fault_write(struct nvkm_device *, u32, u32);
void nvkm_chan_error(struct nvkm_chan *, bool);
void nvkm_runl_rc_cgrp(struct nvkm_cgrp *);

static inline int atomic_read(const atomic_t *value)
{
	return *value;
}

#define nvkm_mask switch_gr_fault_mask
#define nvkm_wr32 switch_gr_fault_write
#ifdef __cplusplus
}
#endif
