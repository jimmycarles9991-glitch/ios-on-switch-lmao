// SPDX-License-Identifier: BSD-2-Clause-Patent
// A present armed by the media engines' syncpoint interrupts: the CPU enqueues the flip, and the interrupt of the last
// engine that writes the surface (NVDEC, or VIC after it) runs it. The shape of switch_gpu_bridge_fence, without the
// GPU job that one puts between the producers and the callback.
#ifndef SWITCH_HOST1X_TOKEN_FENCE_H
#define SWITCH_HOST1X_TOKEN_FENCE_H
#include <stdint.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_HOST1X_TOKEN_FENCE_MAX = 4
};

// tokens: producers of the surface (a view's producer), each a VIC or NVDEC job's. Returns 0 when completed(context,
// status) will run once, on host1x's interrupt work loop, after every token's job has finished on its engine: status 0,
// or a failure one of those jobs had reported by then: its cancellation, its hardware's failure or a producer's it ran
// after, or a semantic failure already retired (one retired later is not seen). It never runs inside this
// call, even when every job is already done. Otherwise nothing runs: -22 for a token that is not a VIC or NVDEC job's
// (the GPU's go through switch_gpu_bridge_fence), -16 when every flip slot is in use, SWITCH_GPU_BACKING_STALE for a
// token from before a reset, a job's failure when it already failed, -19 while host1x is down.
int switch_host1x_token_fence(const struct switch_gpu_backing_token *tokens, uint32_t count,
                              void (*completed)(void *context, int status), void *context);

#ifdef __cplusplus
}
#endif
#endif
