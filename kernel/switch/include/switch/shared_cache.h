// SPDX-License-Identifier: GPL-2.0-only
/*
 * The shared region's checks on a dyld shared cache file, bsd/vm/vm_unix.c shared_region_map_and_slide_setup
 * (kernel/switch/vm/shared_cache.c), for every XNU.
 */
#ifndef SWITCH_SHARED_CACHE_H
#define SWITCH_SHARED_CACHE_H

#include <stdbool.h>
#include <sys/cdefs.h>

struct mount;
struct proc;
struct vnode;

__BEGIN_DECLS

/* Whether the cache file vp is on the OS cryptex's mount, itself or as the file a boot union shows at a root path. */
bool switch_shared_cache_on_cryptex(struct proc *p, struct vnode *vp, struct mount *cryptex);

#if CONFIG_A64X
/* A DriverKit cache not translated offline, which the load-time translator would rewrite with the main cache's plan. */
bool switch_shared_cache_unusable(struct proc *p, struct vnode *vp, bool driverkit);
#endif

__END_DECLS

#endif /* SWITCH_SHARED_CACHE_H */
