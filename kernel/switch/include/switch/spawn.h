// SPDX-License-Identifier: GPL-2.0-only
/*
 * posix_spawn(2)'s attribute block, read in the layout of the libsystem_kernel that sent it.
 *
 * Included by bsd/kern/kern_exec.c, after its own includes: struct _posix_spawnattr (sys/spawn_internal.h), copyin,
 * printf, EINVAL and user_addr_t come from there.
 */
#ifndef SWITCH_SPAWN_H
#define SWITCH_SPAWN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * iOS 27's libsystem_kernel (xnu-13432) sends a 0x100-byte struct _posix_spawnattr; this kernel's own is 0xf8. The two
 * agree up to psa_launch_type (0xa8). After it, xnu-13432 keeps psa_sec_flags as a uint32_t at 0xac, puts a sanitizer
 * flag byte at 0xb0, and moves psa_crash_count through psa_conclave_mem_limit 8 bytes down (0xb4-0xc4), so the
 * extension pointers start at 0xc8 (24B119's posix_spawnattr setters). Read with this kernel's offsets, the crash count
 * and throttle timeout that launchd and xpcproxy set on a relaunched job become its kqworkloop limits, and
 * psa_sec_flags comes from two bytes posix_spawnattr_init never writes.
 */
#define SWITCH_SPAWN_DARWIN27_ATTR_SIZE 0x100
#define SWITCH_SPAWN_DARWIN27_TAIL      0xa8
#define SWITCH_SPAWN_DARWIN27_PORTS     0xc8

struct switch_spawn_darwin27_tail {
    uint8_t launch_type;
    uint8_t unset[3];
    uint32_t sec_flags;
    uint8_t use_sanitizers;
    uint8_t padding[3];
    uint32_t crash_count;
    uint32_t throttle_timeout;
    uint32_t kqworkloop_soft_limit;
    uint32_t kqworkloop_hard_limit;
    uint32_t conclave_mem_limit;
};

_Static_assert(SWITCH_SPAWN_DARWIN27_TAIL + sizeof(struct switch_spawn_darwin27_tail) == SWITCH_SPAWN_DARWIN27_PORTS,
               "the Darwin 27 tail ends where its extension pointers start");
_Static_assert(offsetof(struct _posix_spawnattr, psa_launch_type) == SWITCH_SPAWN_DARWIN27_TAIL,
               "both layouts share every field before psa_launch_type");

/* use_sanitizers and the sec_flags bits above this kernel's 16 have no counterpart here. */
static inline void switch_spawn_attr_from_darwin27(struct _posix_spawnattr *psa, const uint8_t *user)
{
    struct switch_spawn_darwin27_tail tail;

    memcpy(psa, user, SWITCH_SPAWN_DARWIN27_TAIL);
    memcpy(&tail, user + SWITCH_SPAWN_DARWIN27_TAIL, sizeof(tail));
    psa->psa_launch_type = tail.launch_type;
    psa->psa_sec_flags = (uint16_t)tail.sec_flags;
    psa->psa_crash_count = tail.crash_count;
    psa->psa_throttle_timeout = tail.throttle_timeout;
    psa->psa_kqworkloop_soft_limit = tail.kqworkloop_soft_limit;
    psa->psa_kqworkloop_hard_limit = tail.kqworkloop_hard_limit;
    psa->psa_conclave_mem_limit = tail.conclave_mem_limit;
}

/*
 * Fills psa up to its extension pointers (size bytes in this kernel's layout), as copyin(uaddr, psa, size) did. A
 * block of any other size is refused: read with the wrong offsets it would fail only later, in whatever its misread
 * fields control.
 */
static inline int switch_spawn_copyin_attr(user_addr_t uaddr, uint64_t attr_size, struct _posix_spawnattr *psa,
                                           size_t size)
{
    uint8_t darwin27[SWITCH_SPAWN_DARWIN27_PORTS];
    int error;

    if (attr_size == sizeof(struct _posix_spawnattr)) {
        return copyin(uaddr, psa, size);
    }
    if (attr_size != SWITCH_SPAWN_DARWIN27_ATTR_SIZE) {
        printf("posix_spawn: a %llu-byte attribute block, a layout this kernel does not know\n",
               (unsigned long long)attr_size);
        return EINVAL;
    }
    error = copyin(uaddr, darwin27, sizeof(darwin27));
    if (error == 0) {
        switch_spawn_attr_from_darwin27(psa, darwin27);
    }
    return error;
}

#endif
