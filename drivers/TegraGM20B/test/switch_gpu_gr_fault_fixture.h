/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef int atomic_t;

struct nvkm_runl {
	atomic_t rc_pending;
};

struct nvkm_cgrp {
	struct nvkm_runl *runl;
	atomic_t rc;
};

struct nvkm_chan {
	struct nvkm_cgrp *cgrp;
	atomic_t errored;
};

#ifdef __cplusplus
extern "C" {
#endif
void nvkm_chan_error(struct nvkm_chan *, bool);
void nvkm_runl_rc_cgrp(struct nvkm_cgrp *);
#ifdef __cplusplus
}
#endif
