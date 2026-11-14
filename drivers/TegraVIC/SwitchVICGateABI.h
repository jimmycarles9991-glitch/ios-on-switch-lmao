// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test's display.media_completion_flip_gated as it calls TegraVIC (debug kernels): the kernel-test selector, requests
// with this magic. ARM queues a VIC copy of the caller's IOSurface `source` into its IOSurface `destination` (the same
// format and size, both as VIC addresses them) behind a syncpoint only the CPU moves, so a video set showing the
// destination waits for its producer; RELEASE moves that syncpoint and the copy runs; QUERY reports. One gate at a time;
// a connection that closes with its gate held releases it. A release kernel answers kIOReturnNotPermitted. C.
#pragma once
#include <stdint.h>

#define SWITCH_VIC_GATE_MAGIC 0x56474154u  // 'VGAT'

enum {
    SWITCH_VIC_GATE_ARM = 1,
    SWITCH_VIC_GATE_RELEASE = 2,
    SWITCH_VIC_GATE_QUERY = 3,
};

enum {
    SWITCH_VIC_GATE_IDLE = 0,
    SWITCH_VIC_GATE_HELD = 1,      // the copy is queued behind the gate
    SWITCH_VIC_GATE_RELEASED = 2,  // the gate moved, the copy has not completed
    SWITCH_VIC_GATE_DONE = 3,      // the copy completed with `status`
};

typedef struct {
    uint32_t magic;
    uint32_t op;
    uint32_t source, destination;  // ARM: IOSurface IDs in the caller's task
} SwitchVICGateRequest;

typedef struct {
    uint32_t magic;
    uint32_t state;
    int32_t error;   // why the op was refused (an errno), 0 when it was done
    int32_t status;  // the copy's completion, once DONE
    uint32_t syncpt, threshold;
} SwitchVICGateReply;
