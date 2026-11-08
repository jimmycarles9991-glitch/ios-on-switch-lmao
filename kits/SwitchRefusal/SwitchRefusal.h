// SPDX-License-Identifier: BSD-2-Clause-Patent
// One error line per (process, domain, reason) when our code refuses or drops work iPadOS sent it. One C ABI for the
// kernel and user space: os_log_error under SWITCH_REFUSAL_SUBSYSTEM, the domain as the category, kept in release.
#ifndef SWITCH_REFUSAL_H
#define SWITCH_REFUSAL_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_REFUSAL_SUBSYSTEM "com.brunostjohn.switch.refusal"

#ifdef __cplusplus
#define SWITCH_REFUSAL_INLINE static constexpr
#else
#define SWITCH_REFUSAL_INLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The key of (domain, reason_key): 64-bit FNV-1a (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over the
// domain's bytes, one zero byte, then reason_key's bytes. constexpr in C++, so a binding can fold it.
SWITCH_REFUSAL_INLINE uint64_t switch_refusal_key(const char *domain, const char *reason_key)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (; *domain != '\0'; domain++) {
        hash = (hash ^ (uint8_t)*domain) * 0x100000001b3ULL;
    }
    hash *= 0x100000001b3ULL;
    for (; *reason_key != '\0'; reason_key++) {
        hash = (hash ^ (uint8_t)*reason_key) * 0x100000001b3ULL;
    }
    return hash;
}

// The same FNV-1a continued over detail's 8 bytes, least significant first: when each runtime value (a pixel format,
// a selector) is a reason of its own.
SWITCH_REFUSAL_INLINE uint64_t switch_refusal_key_with(uint64_t key, uint64_t detail)
{
    for (unsigned byte = 0; byte < 8U; byte++) {
        key = (key ^ ((detail >> (byte * 8U)) & 0xffU)) * 0x100000001b3ULL;
    }
    return key;
}

// True the first time this process presents key, and from then on false for one relaxed atomic load. In the kernel the
// process is the calling thread's (work loops and interrupt threads are all kernel_task). Lock-free, allocation-free,
// any context. Once its probe window is full an unseen key is true on the 1st, 2nd, 4th, 8th... such overflow.
bool switch_refusal_first(uint64_t key);

// "<process>[<pid>] <reason_key>: <text>" as os_log_error, category domain. domain and reason_key must live as long as
// the image (literals). The kernel logs to the default log with the domain in front: creating a log handle can block,
// and a refusal may be logged with preemption off.
void switch_refusal_log(const char *domain, const char *reason_key, const char *text);

// switch_refusal_first() on switch_refusal_key(domain, reason_key), then the printf-style text, formatted only then.
void switch_refusal(const char *domain, const char *reason_key, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

// The same with a key the caller made (a folded constant, or switch_refusal_key_with() for a detail).
void switch_refusal_k(uint64_t key, const char *domain, const char *reason_key, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
}
#endif

#endif
