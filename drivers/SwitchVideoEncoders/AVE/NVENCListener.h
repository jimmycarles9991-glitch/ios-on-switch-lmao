// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVENC job completion delivery (TegraNVENCABI.h, SWITCH_NVENC_COMPLETION_PORT): Mach notification messages on
// the caller's serial queue, or if the sandbox refuses the port, one single Wait per job on that queue.
#ifndef NVENC_LISTENER_H
#define NVENC_LISTENER_H
#include <IOKit/IOKitLib.h>
#include "NVENCClient.h"

typedef struct NVENCListener NVENCListener;

NVENCListener *nvenc_listener_start(io_connect_t connection, uint32_t selector, dispatch_queue_t queue, NVENCDone done,
                                    void *context);
void nvenc_listener_submitted(NVENCListener *listener, uint32_t fence, TegraNVENCRange range, const void *status);
void nvenc_listener_stop(NVENCListener *listener);
bool nvenc_listener_on_port(const NVENCListener *listener);

kern_return_t nvenc_tunnel(io_connect_t connection, uint32_t selector, uint32_t method, const void *in, size_t inSize,
                           void *out, size_t outSize);

#endif
