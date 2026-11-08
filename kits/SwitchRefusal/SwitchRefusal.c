// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchRefusal.h"

#include <os/log.h>
#include <stdarg.h>

#ifdef KERNEL
#include <libkern/libkern.h>
#include <sys/proc.h>
#else
#include <os/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#define SWITCH_REFUSAL_SLOTS  1024u
#define SWITCH_REFUSAL_PROBES 8u
#define SWITCH_REFUSAL_TEXT   224u

static uint64_t switch_refusal_seen[SWITCH_REFUSAL_SLOTS];
static uint32_t switch_refusal_overflows;

// The kernel's table holds every process's keys, so the pid joins the key there; user space's table is per image.
static uint64_t switch_refusal_tag(uint64_t key)
{
#ifdef KERNEL
    key ^= (uint64_t)(uint32_t)proc_selfpid() * 0x9e3779b97f4a7c15ULL;
#endif
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return key != 0 ? key : 1;
}

bool switch_refusal_first(uint64_t key)
{
    const uint64_t tag = switch_refusal_tag(key);
    for (uint32_t probe = 0; probe < SWITCH_REFUSAL_PROBES; probe++) {
        uint64_t *slot = &switch_refusal_seen[(tag + probe) & (SWITCH_REFUSAL_SLOTS - 1)];
        uint64_t seen = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (seen == tag) {
            return false;
        }
        // Every inserter of one tag walks the same probe sequence and claims the first empty slot, so racing first
        // hits meet at one compare-exchange and only one of them logs.
        if (seen == 0) {
            if (__atomic_compare_exchange_n(slot, &seen, tag, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                return true;
            }
            if (seen == tag) {
                return false;
            }
        }
    }
    const uint32_t overflows = __atomic_add_fetch(&switch_refusal_overflows, 1, __ATOMIC_RELAXED);
    return (overflows & (overflows - 1)) == 0;
}

#ifdef KERNEL
void switch_refusal_log(const char *domain, const char *reason_key, const char *text)
{
    char process[32];
    proc_selfname(process, sizeof(process));
    const int pid = proc_selfpid();
    // os_log_create can block, and a refusal may be logged with preemption off, so the kernel logs to the default log.
    // There os_log formats into the message buffer with the kernel's printf, which has no privacy specifiers ("%{public}s"
    // prints "{public}s" and drops the argument): plain "%s", public in the kernel's log either way.
    os_log_error(OS_LOG_DEFAULT, "%s: %s[%d] %s: %s", domain, process, pid, reason_key, text);
}
#else
void switch_refusal_log(const char *domain, const char *reason_key, const char *text)
{
    os_log_t log = os_log_create(SWITCH_REFUSAL_SUBSYSTEM, domain);
    os_log_error(log, "%{public}s[%d] %{public}s: %{public}s", getprogname(), getpid(), reason_key, text);
    os_release(log);
}
#endif

static void switch_refusal_logv(const char *domain, const char *reason_key, const char *format, va_list args)
{
    char text[SWITCH_REFUSAL_TEXT];
    vsnprintf(text, sizeof(text), format, args);
    switch_refusal_log(domain, reason_key, text);
}

void switch_refusal(const char *domain, const char *reason_key, const char *format, ...)
{
    if (!switch_refusal_first(switch_refusal_key(domain, reason_key))) {
        return;
    }
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    switch_refusal_logv(domain, reason_key, format, args);
    va_end(args);
}

void switch_refusal_k(uint64_t key, const char *domain, const char *reason_key, const char *format, ...)
{
    if (!switch_refusal_first(key)) {
        return;
    }
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    switch_refusal_logv(domain, reason_key, format, args);
    va_end(args);
}
