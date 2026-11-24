// SPDX-License-Identifier: GPL-2.0-only
/*
 * A shared cache map that bsd/vm/vm_unix.c refuses as "not on process' root volume nor preboot volume" after the
 * Cryptexes/OS exemption did not apply (diag/cryptex_map.c, debug kernels): one "SwitchCryptex: map refused" line with
 * the process, the cache's volume, and the result of each lookup the exemption makes.
 */
#ifndef SWITCH_CRYPTEX_MAP_H
#define SWITCH_CRYPTEX_MAP_H

#include <sys/cdefs.h>

__BEGIN_DECLS

struct proc;
struct vnode;

#define SWITCH_CRYPTEX_MAP_LINES   16u
#define SWITCH_CRYPTEX_PREBOOT_DIR "/private/preboot/Cryptexes"

#if DEVELOPMENT || DEBUG
void switch_cryptex_map_refused(struct proc *p, struct vnode *cache, struct vnode *preboot, int preboot_error);

#define SWITCH_CRYPTEX_MAP_REFUSED(p, cache, preboot, preboot_error) \
    switch_cryptex_map_refused((p), (cache), (preboot), (preboot_error))
#else
#define SWITCH_CRYPTEX_MAP_REFUSED(p, cache, preboot, preboot_error) \
    do {                                                             \
    } while (0)
#endif

__END_DECLS

#endif /* SWITCH_CRYPTEX_MAP_H */
