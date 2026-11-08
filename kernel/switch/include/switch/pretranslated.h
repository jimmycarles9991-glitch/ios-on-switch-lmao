// SPDX-License-Identifier: GPL-2.0-only
/*
 * iOS 27's System files translated offline (kernel/switch/vfs/pretranslated.c), for every XNU component that calls it.
 * Without CONFIG_A64X the hooks are no-ops.
 */
#ifndef SWITCH_PRETRANSLATED_KERNEL_H
#define SWITCH_PRETRANSLATED_KERNEL_H

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../../../kits/SwitchPretranslated/switch_pretranslated.h"
#include <sys/ioccom.h>

struct fileglob;
struct nameidata;
struct vnode;

__BEGIN_DECLS

#if CONFIG_A64X

extern bool switch_pretranslated_on;

void switch_pretranslated_capture(void);
void switch_pretranslated_redirect(struct nameidata *ndp);
bool switch_pretranslated_authentic(struct vnode *vp);
int switch_pretranslated_mmap_check(struct fileglob *fg, int flags, int prot, int maxprot);
int switch_pretranslated_refuse(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                struct vnode *v3, uint32_t intent);

static inline void switch_pretranslated_lookup(struct nameidata *ndp)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        switch_pretranslated_redirect(ndp);
    }
}

/* EPERM when op would change the pretranslated tree (vnodes in the order switch_pt_denied names them), else 0. */
static inline int switch_pretranslated_check(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                             struct vnode *v3, uint32_t intent)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        return switch_pretranslated_refuse(op, v0, v1, v2, v3, intent);
    }
    return 0;
}

/* mmap: a shared mapping that can write a file of the tree is refused like any other write. */
static inline int switch_pretranslated_mmap(struct fileglob *fg, int flags, int prot, int maxprot)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        return switch_pretranslated_mmap_check(fg, flags, prot, maxprot);
    }
    return 0;
}

#else

static inline void switch_pretranslated_capture(void)
{}

static inline void switch_pretranslated_lookup(struct nameidata *ndp)
{
    (void)ndp;
}

static inline bool switch_pretranslated_authentic(struct vnode *vp)
{
    (void)vp;
    return false;
}

static inline int switch_pretranslated_check(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                             struct vnode *v3, uint32_t intent)
{
    (void)op, (void)v0, (void)v1, (void)v2, (void)v3, (void)intent;
    return 0;
}

static inline int switch_pretranslated_mmap(struct fileglob *fg, int flags, int prot, int maxprot)
{
    (void)fg, (void)flags, (void)prot, (void)maxprot;
    return 0;
}

#endif

/* fsctl: an ioctl that passes data in, or none out, counts as a write. */
static inline int switch_pretranslated_fsctl(struct vnode *vp, unsigned long cmd)
{
    return switch_pretranslated_check(SWITCH_PT_FSCTL, vp, NULL, NULL, NULL,
                                      ((cmd & IOC_IN) || !(cmd & IOC_OUT)) ? SWITCH_PT_WRITES : 0);
}

__END_DECLS

#endif /* SWITCH_PRETRANSLATED_KERNEL_H */
