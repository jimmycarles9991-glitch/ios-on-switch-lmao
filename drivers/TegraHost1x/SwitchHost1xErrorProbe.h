// SPDX-License-Identifier: BSD-2-Clause-Patent
// The illegal-opcode probe, debug kernels only (TegraVIC's selector 6, SwitchVICKernelTest.h, magic 'HIOP'): whether
// host1x's general interrupt delivers, and whether a channel stopped on an illegal opcode comes back. On spare channel
// 13, which no queue uses:
//   1. opcode 15, which no host1x version decodes (Linux hw/debug_hw.c knows 0 to 14), with the channel's error
//      expected (SwitchHost1xErrors.cpp): the interrupt wakes the probe instead of failing jobs. Latched and not
//      delivered in 100 ms is a panic; neither latched nor delivered means opcode 15 is not illegal on this unit;
//   2. the channel torn down and restarted, as every queue's cancellation does;
//   3. a stream that only increments a syncpoint, which must run.
// CMDPROC_STAT (14.9.11) is read before, after the opcode and after the restart.
#pragma once
#include <stdint.h>

#define SWITCH_HOST1X_ERROR_PROBE_MAGIC   0x48494f50u  // 'HIOP'
#define SWITCH_HOST1X_ERROR_PROBE_CHANNEL 13u
#define SWITCH_HOST1X_ERROR_PROBE_OPCODE  15u

// The general interrupt's registers (TRM 14.9.2, 14.9.5, 14.9.7, 14.9.8).
typedef struct {
    uint32_t intmask, hintstatus, hintstatusExt, hintmaskExt;
} SwitchHost1xErrorLine;

typedef struct {
    uint32_t magic;
    int32_t status;     // 0, or the error of the step that could not run
    uint32_t attached;  // the general interrupt is attached and on
    uint32_t channel, opcode;
    uint32_t taken;  // illegal opcodes the interrupt delivered for the channel
    uint32_t statBefore, statStopped, statRestarted;
    uint32_t recovered;  // the increment-only stream ran after the restart
    // While the channel is stopped, its bit masked: whether it latched again (HINTSTATUS_EXT) and whether HINTSTATUS
    // bit 31 summarises it anyway. After the run: the master enable still on.
    SwitchHost1xErrorLine stopped, after;
} SwitchHost1xErrorProbeReply;

#ifdef __cplusplus
bool switch_host1x_errors_attached();
void switch_host1x_errors_expect(uint32_t channel);
// The expected channel's errors delivered within `ms`; a latched one not delivered panics.
uint32_t switch_host1x_errors_expect_end(uint32_t ms);
uint32_t switch_host1x_errors_stat();
SwitchHost1xErrorLine switch_host1x_errors_line();
extern "C" int switch_host1x_error_probe_run(void *reply);
#endif
