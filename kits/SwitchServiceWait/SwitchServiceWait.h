// SPDX-License-Identifier: BSD-2-Clause-Patent
// User space: wait, bounded, until a kernel service is published or its driver says it never will be. A process that
// asks before the driver's bring-up has ended (VideoToolbox fills its codec registry once per process, at its first
// use) would otherwise decide for its whole life from a service that was about to appear. The driver publishes an
// IOResources resource (IOResourceMatch) when its bring-up ends without the service, so once either is there nothing
// waits; the bound only covers a driver that never ends its bring-up. Both arrive as IOKit matching notifications on a
// queue of the call's own: no polling.
#ifndef SWITCH_SERVICE_WAIT_H
#define SWITCH_SERVICE_WAIT_H
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool switch_service_wait_drain(io_iterator_t iterator)
{
    bool any = false;
    for (io_object_t object = IOIteratorNext(iterator); object; object = IOIteratorNext(iterator)) {
        IOObjectRelease(object);
        any = true;
    }
    return any;
}

static inline void switch_service_wait_matched(void *semaphore, io_iterator_t iterator)
{
    if (switch_service_wait_drain(iterator))
        dispatch_semaphore_signal((dispatch_semaphore_t)semaphore);
}

static inline CFMutableDictionaryRef switch_service_wait_resource(const char *resource)
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOResources");
    CFStringRef name = CFStringCreateWithCString(NULL, resource, kCFStringEncodingASCII);
    if (matching && name)
        CFDictionarySetValue(matching, CFSTR("IOResourceMatch"), name);
    if (name)
        CFRelease(name);
    return matching;
}

static inline bool switch_service_present(const char *service)
{
    const io_service_t found = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(service));
    if (found)
        IOObjectRelease(found);
    return found != 0;
}

typedef struct {
    IONotificationPortRef port;
    dispatch_semaphore_t semaphore;
    CFMutableDictionaryRef matching[2];
    io_iterator_t matches[2];
    bool armed, found;
} SwitchServiceWait;

// On the wait's queue, so the first drain of each iterator (what is already published) runs before any callback the
// drain arms.
static inline void switch_service_wait_arm(void *context)
{
    SwitchServiceWait *w = (SwitchServiceWait *)context;
    for (int i = 0; i < 2; i++) {
        if (!w->matching[i] ||
            IOServiceAddMatchingNotification(w->port, kIOFirstMatchNotification, w->matching[i],
                                             switch_service_wait_matched, w->semaphore, &w->matches[i]) != KERN_SUCCESS)
            w->armed = false;
        else if (switch_service_wait_drain(w->matches[i]))
            w->found = true;
    }
}

static inline void switch_service_wait_disarm(void *context)
{
    SwitchServiceWait *w = (SwitchServiceWait *)context;
    for (int i = 0; i < 2; i++)
        if (w->matches[i])
            IOObjectRelease(w->matches[i]);
    IONotificationPortDestroy(w->port);
}

// True once `service` (an IOServiceMatching class) is published; false when `absent` is, or after `timeoutMs`. A
// process its sandbox denies the notifications decides from what is published now.
static inline bool switch_service_wait(const char *service, const char *absent, uint32_t timeoutMs)
{
    SwitchServiceWait w;
    memset(&w, 0, sizeof(w));
    dispatch_queue_t queue = dispatch_queue_create("com.brunostjohn.switch.service-wait", DISPATCH_QUEUE_SERIAL);
    w.semaphore = dispatch_semaphore_create(0);
    w.port = IONotificationPortCreate(kIOMainPortDefault);
    if (w.port && queue && w.semaphore) {
        IONotificationPortSetDispatchQueue(w.port, queue);
        w.matching[0] = IOServiceMatching(service);
        w.matching[1] = switch_service_wait_resource(absent);
        w.armed = true;
        dispatch_sync_f(queue, &w, switch_service_wait_arm);
        if (w.armed && !w.found)
            (void)dispatch_semaphore_wait(w.semaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeoutMs * 1000000));
        dispatch_sync_f(queue, &w, switch_service_wait_disarm);
    } else if (w.port) {
        IONotificationPortDestroy(w.port);
    }
    if (queue)
        dispatch_release(queue);
    if (w.semaphore)
        dispatch_release(w.semaphore);
    return switch_service_present(service);
}

#endif
