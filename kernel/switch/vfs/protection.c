// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real iPads format /private/var for content protection, and Apple code relies on it. MobileKeyBag's
 * MKBDeviceFormattedForContentProtection and libsqlite's SQLITE_OPEN_FILEPROTECTION_* both test
 * statfs(...).f_flags & MNT_CPROTECT (locationd's CLDataProtectionManager skips its keybag setup without it), and a
 * class set on a file or given when it is made reads back: spotlightknowledged's updater requires a new file in
 * DocumentProcessing to read as class C, PosterBoard its store as class D (NSFileProtectionNone).
 *
 * Our Data volume is APFS without per-file keys (MNT_CPROTECT stays clear inside the kernel, so APFS and the cprotect
 * code never look for keys) and the keybag never locks (drivers/NativeKeyStoreEvents), so a class is a label:
 *  - statfs shows the Data volume with MNT_CPROTECT;
 *  - a class set, given at create or inherited from a labelled directory is kept in a protected xattr
 *    (<switch/protection_rules.h>), only after the caller was authorized as for the same change on a protected volume;
 *    a write that fails fails the call;
 *  - an unlabelled vnode reads as class D if it predates labelling (the epoch is the file id of a file made the first
 *    time a kernel labels), as every file did before, else as a new one does on an iPad: C for a file, no class for a
 *    directory.
 * getattrlistbulk's own packing runs inside the filesystem's directory walk, where no label is read, so a request for
 * classes on the Data volume packs each entry through getattrlist instead.
 */
#include <sys/attr.h>
#include <sys/content_protection.h>
#include <sys/cprotect.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/uio_internal.h>
#include <sys/vnode_internal.h>
#include <sys/xattr.h>
#include <kern/kalloc.h>

#include <switch/protection.h>
#include <switch/protection_rules.h>

_Static_assert(SWITCH_PROTECTION_DEFAULT == PROTECTION_CLASS_DEFAULT, "class values");
_Static_assert(SWITCH_PROTECTION_DIR_NONE == PROTECTION_CLASS_DIR_NONE, "class values");
_Static_assert(SWITCH_PROTECTION_C == PROTECTION_CLASS_C, "class values");
_Static_assert(SWITCH_PROTECTION_D == PROTECTION_CLASS_D, "class values");

#define SWITCH_PROTECTION_EPOCH "/private/var/Switch/protection-epoch"

static struct {
    mount_t data;
    uint64_t epoch;
} switch_protection;

void switch_protection_capture(void)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t root = NULLVP, epoch = NULLVP;
    struct vnode_attr va;
    int error = vnode_lookup("/private/var", 0, &root, ctx);
    if (!error && (!vnode_isvroot(root) || (vfs_flags(vnode_mount(root)) & (MNT_RDONLY | MNT_CPROTECT)) ||
                   !(vfs_flags(vnode_mount(root)) & MNT_LOCAL))) {
        error = EINVAL;
    }
    if (!error) {
        error = vnode_open(SWITCH_PROTECTION_EPOCH, O_CREAT | FREAD, S_IRUSR, VNODE_LOOKUP_NOFOLLOW, &epoch, ctx);
    }
    if (!error) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        error = vnode_getattr(epoch, &va, ctx);
        if (!error && !VATTR_IS_SUPPORTED(&va, va_fileid)) {
            error = ENOTSUP;
        }
        vnode_close(epoch, FREAD, ctx);
    }
    if (!error) {
        switch_protection.epoch = va.va_fileid;
        switch_protection.data = vnode_mount(root);
        printf("SwitchProtection: on, files before id %llu read as class D\n", switch_protection.epoch);
    } else {
        printf("SwitchProtection: off (%d): /private/var shows no content protection\n", error);
    }
    if (root) {
        vnode_put(root);
    }
}

uint32_t switch_protection_mount_flags(mount_t mp)
{
    return mp != NULL && mp == switch_protection.data ? MNT_CPROTECT : 0;
}

bool switch_protection_volume(vnode_t vp)
{
    return vp != NULLVP && switch_protection.data != NULL && vnode_mount(vp) == switch_protection.data &&
           (vnode_isreg(vp) || vnode_isdir(vp));
}

static bool switch_protection_unprotected(vnode_t vp)
{
    const uint64_t flags = vfs_flags(vnode_mount(vp));
    return !(flags & (MNT_CPROTECT | MNT_RDONLY)) && (flags & MNT_LOCAL);
}

static int switch_protection_label(vnode_t vp, int32_t *pclass, vfs_context_t ctx)
{
    uint32_t label = 0;
    size_t size = sizeof(label);
    UIO_STACKBUF(uio_buf, 1);
    uio_t uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
    uio_addiov(uio, CAST_USER_ADDR_T(&label), sizeof(label));
    int error = vn_getxattr(vp, SWITCH_PROTECTION_XATTR, uio, &size, XATTR_NOSECURITY, ctx);
    if (!error && (uio_resid(uio) != 0 || !cp_is_valid_class(vnode_isdir(vp), (int32_t)label))) {
        error = EINVAL;
    }
    *pclass = (int32_t)label;
    return error;
}

static int32_t switch_protection_class(vnode_t vp, vfs_context_t ctx)
{
    int32_t pclass;
    if (switch_protection_label(vp, &pclass, ctx) == 0) {
        return pclass;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    const bool known = vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_fileid);
    return switch_protection_unlabelled(vnode_isdir(vp), known, known ? va.va_fileid : 0, switch_protection.epoch);
}

static int switch_protection_relabel(vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    if (switch_protection_class(vp, ctx) == pclass) {
        return 0;
    }
    uint32_t label = (uint32_t)pclass;
    UIO_STACKBUF(uio_buf, 1);
    uio_t uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_WRITE, uio_buf, sizeof(uio_buf));
    uio_addiov(uio, CAST_USER_ADDR_T(&label), sizeof(label));
    return vn_setxattr(vp, SWITCH_PROTECTION_XATTR, uio, XATTR_NOSECURITY, ctx);
}

int switch_protection_get(vnode_t vp, int32_t *pclass, vfs_context_t ctx)
{
    *pclass = switch_protection_class(vp, ctx);
    return 0;
}

int switch_protection_set(vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    if (!cp_is_valid_class(vnode_isdir(vp), pclass)) {
        return EINVAL;
    }
    return switch_protection_volume(vp) ? switch_protection_relabel(vp, pclass, ctx) : 0;
}

int switch_protection_setattr_begin(vnode_t vp, struct vnode_attr *vap, int32_t *pclass, vfs_context_t ctx)
{
    *pclass = PROTECTION_CLASS_DEFAULT;
    if (!VATTR_IS_ACTIVE(vap, va_dataprotect_class) || !switch_protection_unprotected(vp)) {
        return 0;
    }
    if (!cp_is_valid_class(vnode_isdir(vp), (int32_t)vap->va_dataprotect_class)) {
        return EINVAL;
    }
    int error = vnode_authorize(vp, NULLVP, KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA, ctx);
    if (error) {
        return error;
    }
    if (switch_protection_volume(vp)) {
        *pclass = (int32_t)vap->va_dataprotect_class;
    }
    VATTR_CLEAR_ACTIVE(vap, va_dataprotect_class);
    return 0;
}

int switch_protection_setattr_end(vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    return pclass == PROTECTION_CLASS_DEFAULT ? 0 : switch_protection_relabel(vp, pclass, ctx);
}

void switch_protection_getattr(mount_t mp, vnode_t vp, uint32_t commonattr, struct vnode_attr *vap, vfs_context_t ctx)
{
    if (!(commonattr & ATTR_CMN_DATA_PROTECT_FLAGS) || !VATTR_IS_ACTIVE(vap, va_dataprotect_class) || mp == NULL) {
        return;
    }
    const uint64_t flags = vfs_flags(mp);
    if ((flags & (MNT_CPROTECT | MNT_RDONLY)) || !(flags & MNT_LOCAL)) {
        return;
    }
    vap->va_dataprotect_class = switch_protection_volume(vp) ? switch_protection_class(vp, ctx) : PROTECTION_CLASS_D;
    VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
}

bool switch_protection_bulk_by_entry(vnode_t dvp, uint32_t commonattr)
{
    return (commonattr & ATTR_CMN_DATA_PROTECT_FLAGS) && switch_protection_volume(dvp);
}

int switch_protection_create_begin(vnode_t dvp, struct vnode_attr *vap, int32_t *pclass)
{
    *pclass = PROTECTION_CLASS_DEFAULT;
    if (!switch_protection_volume(dvp) || !VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
        return 0;
    }
    if (!cp_is_valid_class(vap->va_type == VDIR, (int32_t)vap->va_dataprotect_class)) {
        return EINVAL;
    }
    *pclass = (int32_t)vap->va_dataprotect_class;
    VATTR_CLEAR_ACTIVE(vap, va_dataprotect_class);
    return 0;
}

int switch_protection_create_end(vnode_t dvp, vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    if (!switch_protection_volume(vp)) {
        return 0;
    }
    int32_t parent = PROTECTION_CLASS_DEFAULT;
    const bool labelled = pclass == PROTECTION_CLASS_DEFAULT && switch_protection_label(dvp, &parent, ctx) == 0;
    const int32_t label = switch_protection_created_label(vnode_isdir(vp), pclass, labelled, parent);
    return label == PROTECTION_CLASS_DEFAULT ? 0 : switch_protection_relabel(vp, label, ctx);
}

int switch_protection_listxattr(vnode_t vp, uio_t uio, size_t *size, int options, vfs_context_t ctx)
{
    int32_t label;
    if (!switch_protection_volume(vp) || switch_protection_label(vp, &label, ctx) == ENOATTR) {
        return vn_listxattr(vp, uio, size, options, ctx);
    }
    size_t names_size = 0;
    int error = vn_listxattr(vp, NULL, &names_size, options, ctx);
    if (error || names_size == 0) {
        return error ? error : vn_listxattr(vp, uio, size, options, ctx);
    }
    char *names = kalloc_data(names_size, Z_WAITOK);
    if (names == NULL) {
        return ENOMEM;
    }
    UIO_STACKBUF(uio_buf, 1);
    uio_t names_uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
    uio_addiov(names_uio, CAST_USER_ADDR_T(names), names_size);
    size_t listed = 0;
    error = vn_listxattr(vp, names_uio, &listed, options, ctx);
    if (!error) {
        const size_t length =
            switch_protection_hide(names, names_size - (size_t)uio_resid(names_uio), SWITCH_PROTECTION_XATTR);
        if (uio == NULL) {
            *size = length;
        } else if (length > (size_t)uio_resid(uio)) {
            error = ERANGE;
        } else if (length > 0) {
            error = uiomove(names, (int)length, uio);
        }
    }
    kfree_data(names, names_size);
    return error;
}
