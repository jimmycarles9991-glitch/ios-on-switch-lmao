/*
 * The util/macros.h names (Mesa, MIT) that nv_push.h and the generated nv_push_cl*.h use, for units built without Mesa's
 * util/ (the kernel has no stdio). Spelled exactly as Mesa spells them: a host unit that also includes util/macros.h
 * sees the same definitions twice, which C and C++ allow.
 */
#ifndef SWITCH_NV_PUSH_COMPAT_H
#define SWITCH_NV_PUSH_COMPAT_H
#pragma clang system_header

#ifdef KERNEL
#include <kern/assert.h>
#else
#include <assert.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef UTIL_MACROS_H
#define UNUSED __attribute__((unused))
#ifdef NDEBUG
#define ASSERTED UNUSED
#else
#define ASSERTED
#endif
#define MIN2( A, B )   ( (A)<(B) ? (A) : (B) )
#define MAX2( A, B )   ( (A)>(B) ? (A) : (B) )
#define BITFIELD_BIT(b)      (1u << (b))
#endif

#endif
