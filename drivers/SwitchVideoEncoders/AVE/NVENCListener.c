// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVENCListener.h.
#include "NVENCListener.h"
#include <IOKit/OSMessageNotification.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

_Static_assert(sizeof(nvenc_pic_stat_s) == NVENC_STATUS_BYTES, "NVENC's status record");

struct NVENCListener {
    atomic_int references;
    atomic_bool stopped;
    NVENCDone done;
    void *context;
    dispatch_queue_t queue;
    dispatch_source_t source;  // NULL: Wait fallback route
    mach_port_t port;
    io_connect_t connection;
    uint32_t selector;
};

typedef struct __attribute__((packed)) {
    mach_msg_header_t header;
    mach_msg_size_t size;
    natural_t type;
    io_user_reference_t reference[kOSAsyncRef64Count];
    IOReturn result;
    io_user_reference_t arguments[SWITCH_NVENC_COMPLETION_ARGS];
} CompletionMessage;

typedef struct {
    NVENCListener *listener;
    uint32_t fence;
    TegraNVENCRange range;
    const void *status;
} PendingWait;

static const char kOnQueue = 0;

static void retain(NVENCListener *l)
{
    atomic_fetch_add_explicit(&l->references, 1, memory_order_relaxed);
}

static void release(NVENCListener *l)
{
    if (atomic_fetch_sub_explicit(&l->references, 1, memory_order_acq_rel) != 1)
        return;
    if (l->port != MACH_PORT_NULL)
        mach_port_mod_refs(mach_task_self(), l->port, MACH_PORT_RIGHT_RECEIVE, -1);
    dispatch_release(l->queue);
    free(l);
}

static bool stopped(NVENCListener *l)
{
    return atomic_load_explicit(&l->stopped, memory_order_acquire);
}

static void deliver(NVENCListener *l, const uint8_t *bytes)
{
    CompletionMessage m;
    memcpy(&m, bytes, sizeof(m));
    const uint32_t count = (m.size - (uint32_t)sizeof(IOReturn)) / sizeof(io_user_reference_t);
    if (m.header.msgh_size < sizeof(m) || m.header.msgh_id != kOSNotificationMessageID ||
        m.type != kIOAsyncCompletionNotificationType || count < SWITCH_NVENC_COMPLETION_ARGS) {
        switch_refusal("AVE", "NVENC completion message not understood",
                       "size %u id %d type %u arguments %u: its picture is never answered", m.header.msgh_size,
                       m.header.msgh_id, m.type, count);
        return;
    }
    l->done(l->context, (uint32_t)m.arguments[0], m.result ? -5 : 0, (uint32_t)m.arguments[1], (uint32_t)m.arguments[2],
            (uint32_t)m.arguments[3]);
}

static void received(void *context)
{
    NVENCListener *l = context;

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
            switch_refusal("AVE", "NVENC completion message lost",
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
    NVENCListener *l = p->listener;
    if (!stopped(l)) {
        TegraNVENCWait wait = {.fence = p->fence, .timeout_us = 1000000, .invalidate_count = 1};
        wait.invalidate[0] = p->range;
        const kern_return_t kr =
            nvenc_tunnel(l->connection, l->selector, TegraNVENCMethodWait, &wait, sizeof(wait), NULL, 0);
        nvenc_pic_stat_s status = {0};
        if (!kr && p->status)
            memcpy(&status, p->status, sizeof(status));
        if (!stopped(l))
            l->done(l->context, p->fence, kr ? -5 : 0, status.error_status, status.ucode_error_status,
                    status.total_bit_count);
    }
    release(l);
    free(p);
}

static void nothing(void *context)
{
    (void)context;
}

NVENCListener *nvenc_listener_start(io_connect_t connection, uint32_t selector, dispatch_queue_t queue, NVENCDone done,
                                    void *context)
{
    NVENCListener *l = calloc(1, sizeof(*l));
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
        kr = IOConnectSetNotificationPort(connection, SWITCH_NVENC_COMPLETION_PORT, port, 0);
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
        switch_refusal("AVE", "TegraNVENC's completion port refused",
                       "error %#x: each NVENC job's Wait runs on the session's queue instead", kr);
    }
    return l;
}

void nvenc_listener_submitted(NVENCListener *l, uint32_t fence, TegraNVENCRange range, const void *status)
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

bool nvenc_listener_on_port(const NVENCListener *l)
{
    return l && l->source;
}

void nvenc_listener_stop(NVENCListener *l)
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
