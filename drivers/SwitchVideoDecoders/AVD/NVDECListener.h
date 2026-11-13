// SPDX-License-Identifier: BSD-2-Clause-Patent
// Where a TegraNVDEC connection's job completions arrive (TegraNVDECABI.h, SWITCH_NVDEC_COMPLETION_PORT): the kernel's
// messages on the caller's serial queue, or in a process whose sandbox refuses the port (one refusal line), a Wait for
// each job on that queue reading the job's status itself. Either way `done` runs once per job on the queue, in the
// order the messages or the jobs' retirements come.
#ifndef NVDEC_LISTENER_H
#define NVDEC_LISTENER_H
#include <IOKit/IOKitLib.h>
#include "NVDECClient.h"

typedef struct NVDECListener NVDECListener;

NVDECListener *nvdec_listener_start(io_connect_t connection, uint32_t selector, dispatch_queue_t queue, NVDECDone done,
                                    void *context);
// A job just submitted, its status record at `status` in this task: the Wait route queues its Wait.
void nvdec_listener_submitted(NVDECListener *listener, uint32_t fence, TegraNVDECRange range, const void *status);
void nvdec_listener_stop(NVDECListener *listener);
// The kernel's messages bring the completions (false: the Wait route).
bool nvdec_listener_on_port(const NVDECListener *listener);

// One call through the selector the sandbox admits (NVDECClient.c).
kern_return_t nvdec_tunnel(io_connect_t connection, uint32_t selector, uint32_t method, const void *in, size_t inSize,
                           void *out, size_t outSize);

#endif
