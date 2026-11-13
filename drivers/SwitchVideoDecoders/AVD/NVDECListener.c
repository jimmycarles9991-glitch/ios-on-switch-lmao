// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVDECListener.h.
#include "NVDECListener.h"
#include <IOKit/OSMessageNotification.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

_Static_assert(sizeof(nvdec_status_s) == NVDEC_STATUS_BYTES, "NVDEC's status record");

struct NVDECListener {
    atomic_int references;
    atomic_bool stopped;
    NVDECDone done;
    void *context;
    dispatch_queue_t queue;
    dispatch_source_t source;  // NULL: the Wait route
    mach_port_t port;
    io_connect_t connection;
    uint32_t selector;
};

// IOUserClient::sendAsyncResult64's message to a 64-bit task (OSNotificationHeader64, IOAsyncCompletionContent, then
// the arguments, packed).
typedef struct __attribute__((packed)) {
    mach_msg_header_t header;
    mach_msg_size_t size;
    natural_t type;
    io_user_reference_t reference[kOSAsyncRef64Count];
    IOReturn result;
    io_user_reference_t arguments[SWITCH_NVDEC_COMPLETION_ARGS];
} CompletionMessage;

typedef struct {
    NVDECListener *listener;
    uint32_t fence;
    TegraNVDECRange range;
    const void *status;
} PendingWait;

static const char kOnQueue = 0;

static void retain(NVDECListener *l)
{
    atomic_fetch_add_explicit(&l->references, 1, memory_order_relaxed);
}

static void release(NVDECListener *l)
{
    if (atomic_fetch_sub_explicit(&l->references, 1, memory_order_acq_rel) != 1)
        return;
    if (l->port != MACH_PORT_NULL)
        mach_port_mod_refs(mach_task_self(), l->port, MACH_PORT_RIGHT_RECEIVE, -1);
    dispatch_release(l->queue);
    free(l);
}

static bool stopped(NVDECListener *l)
{
    return atomic_load_explicit(&l->stopped, memory_order_acquire);
}

static void deliver(NVDECListener *l, const uint8_t *bytes)
{
    CompletionMessage m;
    memcpy(&m, bytes, sizeof(m));
    const uint32_t count = (m.size - (uint32_t)sizeof(IOReturn)) / sizeof(io_user_reference_t);
    if (m.header.msgh_size < sizeof(m) || m.header.msgh_id != kOSNotificationMessageID ||
        m.type != kIOAsyncCompletionNotificationType || count < SWITCH_NVDEC_COMPLETION_ARGS) {
        switch_refusal("AVD", "NVDEC completion message not understood",
                       "size %u id %d type %u arguments %u: its picture is never answered", m.header.msgh_size,
                       m.header.msgh_id, m.type, count);
        return;
    }
    l->done(l->context, (uint32_t)m.arguments[0], m.result ? -5 : 0, (uint32_t)m.arguments[1],
            (uint32_t)m.arguments[2]);
}

static void received(void *context)
{
    NVDECListener *l = context;

    union {
        mach_msg_header_t header;
        uint8_t bytes[sizeof(CompletionMessage) + 128 + sizeof(mach_msg_max_trailer_t)];
    } m;

    while (!stopped(l)) {
        const mach_msg_return_t kr =
            mach_msg(&m.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(m), l->port, 0, MACH_PORT_NULL);
        if (kr == MACH_RCV_TIMED_OUT)
            return;
        if (kr != MACH_MSG_SUCCESS) {
            switch_refusal("AVD", "NVDEC completion message lost",
                           "mach_msg %#x: a completion was dropped and its picture is never answered", kr);
            return;
        }
        deliver(l, m.bytes);
    }
}

static void cancelled(void *context)
{
    release(context);
}

static void waited(void *context)
{
    PendingWait *p = context;
    NVDECListener *l = p->listener;
    if (!stopped(l)) {
        TegraNVDECWait wait = {.fence = p->fence, .invalidate_count = 1};
        wait.invalidate[0] = p->range;
        const kern_return_t kr =
            nvdec_tunnel(l->connection, l->selector, TegraNVDECMethodWait, &wait, sizeof(wait), NULL, 0);
        nvdec_status_s status = {0};
        if (!kr && p->status)
            memcpy(&status, p->status, sizeof(status));
        if (!stopped(l))
            l->done(l->context, p->fence, kr ? -5 : 0, status.error_status, status.mbs_correctly_decoded);
    }
    release(l);
    free(p);
}

static void nothing(void *context)
{
    (void)context;
}

NVDECListener *nvdec_listener_start(io_connect_t connection, uint32_t selector, dispatch_queue_t queue, NVDECDone done,
                                    void *context)
{
    NVDECListener *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    atomic_init(&l->references, 1);
    l->done = done;
    l->context = context;
    l->queue = queue;
    dispatch_retain(queue);
    l->connection = connection;
    l->selector = selector;
    dispatch_queue_set_specific(queue, &kOnQueue, l, NULL);
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr == KERN_SUCCESS)
        kr = IOConnectSetNotificationPort(connection, SWITCH_NVDEC_COMPLETION_PORT, port, 0);
    dispatch_source_t source =
        kr == KERN_SUCCESS ? dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, port, 0, queue) : NULL;
    if (port != MACH_PORT_NULL)
        l->port = port;
    if (source) {
        l->source = source;
        retain(l);
        dispatch_set_context(source, l);
        dispatch_source_set_event_handler_f(source, received);
        dispatch_source_set_cancel_handler_f(source, cancelled);
        dispatch_resume(source);
    } else {
        switch_refusal("AVD", "TegraNVDEC's completion port refused",
                       "error %#x: each NVDEC job's Wait runs on the session's queue instead", kr);
    }
    return l;
}

void nvdec_listener_submitted(NVDECListener *l, uint32_t fence, TegraNVDECRange range, const void *status)
{
    if (!l || l->source)
        return;
    PendingWait *p = malloc(sizeof(*p));
    if (!p)
        return;
    *p = (PendingWait){l, fence, range, status};
    retain(l);
    dispatch_async_f(l->queue, p, waited);
}

bool nvdec_listener_on_port(const NVDECListener *l)
{
    return l && l->source;
}

void nvdec_listener_stop(NVDECListener *l)
{
    if (!l)
        return;
    atomic_store_explicit(&l->stopped, true, memory_order_release);
    if (l->source) {
        dispatch_source_cancel(l->source);
        dispatch_release(l->source);
    }
    if (dispatch_get_specific(&kOnQueue) != l)
        dispatch_sync_f(l->queue, NULL, nothing);
    release(l);
}
