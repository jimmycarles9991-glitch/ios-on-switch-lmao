// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef AVD_EMITTER_HARNESS_H
#define AVD_EMITTER_HARNESS_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../../AVD/AVDEmitter.h"

typedef struct {
    uint32_t lockAcquisitions;
    uint32_t broadcasts;
    uint32_t outputHeapAllocs;
    uint32_t emittedCount;
    uint32_t waitCalls;
    OSStatus lastEmittedStatus;
    void *lastEmittedFrame;
} AVDEmitterHarnessStats;

void avd_emitter_harness_reset(void);
AVDEmitterHarnessStats avd_emitter_harness_stats(void);
void avd_emitter_harness_set_emit_hook(void (*hook)(void *frame, OSStatus status, void *ctx), void *ctx);
void avd_emitter_harness_set_wait_hook(int (*hook)(uint32_t fence, void *ctx), void *ctx);
void avd_emitter_harness_set_wait_status(int ret, uint32_t error_status, uint32_t mbs_correct);
void avd_emitter_harness_set_status_buffer(uint32_t handle, void *va);

typedef struct AVDGateHarness AVDGateHarness;
AVDGateHarness *avd_gate_harness_create(void);
void avd_gate_harness_destroy(AVDGateHarness *g);
void avd_gate_harness_enter(AVDGateHarness *g);
void avd_gate_harness_leave(AVDGateHarness *g);
uint32_t avd_gate_harness_slow_locks(const AVDGateHarness *g);

#ifdef __cplusplus
}
#endif

#endif
