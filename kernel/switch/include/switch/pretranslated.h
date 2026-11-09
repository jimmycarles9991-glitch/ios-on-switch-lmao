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
#include "../../../../kits/SwitchPretranslated/switch_pretranslated_originals.h"
#include <sys/ioccom.h>

struct fileglob;
struct mount;
struct nameidata;
struct vnode;
struct vnode_attr;

__BEGIN_DECLS

#if CONFIG_A64X

extern bool switch_pretranslated_on;

void switch_pretranslated_capture(void);
void switch_pretranslated_redirect(struct nameidata *ndp);
bool switch_pretranslated_authentic(struct vnode *vp);
int switch_pretranslated_mmap_check(struct fileglob *fg, int flags, int prot, int maxprot);
/* The tree's translation of an image mapped from an installed copy of a staged system app (vp holds Apple's bytes and
 * their signature at slice_offset): that file with an iocount and its own signature (signature_offset and
 * signature_size from the image's LC_CODE_SIGNATURE, which translation keeps) loaded, once it is authentic; else
 * NULLVP. */
struct vnode *switch_pretranslated_translation(struct vnode *vp, int64_t slice_offset, int64_t signature_offset,
                                               uint32_t signature_size);
int switch_pretranslated_refuse(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                struct vnode *v3, uint32_t intent);
void switch_pretranslated_identity(struct vnode *vp, struct vnode_attr *va);
struct mount *switch_pretranslated_system_volume(struct vnode *vp);

static inline void switch_pretranslated_lookup(struct nameidata *ndp)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        switch_pretranslated_redirect(ndp);
    }
}

/* stat and getattrlist: a Data copy reports the System file it stands in for (volume, file id), as a sealed System
 * volume's file would. */
static inline void switch_pretranslated_getattr(struct vnode *vp, struct vnode_attr *va)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        switch_pretranslated_identity(vp, va);
    }
}

/* statfs and fstatfs (identity.c): the System volume for a Data copy, else NULL. */
static inline struct mount *switch_pretranslated_volume(struct vnode *vp)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        return switch_pretranslated_system_volume(vp);
    }
    return NULL;
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

static inline void switch_pretranslated_getattr(struct vnode *vp, struct vnode_attr *va)
{
    (void)vp, (void)va;
}

static inline struct mount *switch_pretranslated_volume(struct vnode *vp)
{
    (void)vp;
    return NULL;
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
