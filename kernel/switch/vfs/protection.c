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
#include <kern/locks.h>

#include <switch/protection.h>
#include <switch/protection_rules.h>

_Static_assert(SWITCH_PROTECTION_DEFAULT == PROTECTION_CLASS_DEFAULT, "class values");
_Static_assert(SWITCH_PROTECTION_DIR_NONE == PROTECTION_CLASS_DIR_NONE, "class values");
_Static_assert(SWITCH_PROTECTION_C == PROTECTION_CLASS_C, "class values");
_Static_assert(SWITCH_PROTECTION_D == PROTECTION_CLASS_D, "class values");
_Static_assert(PROTECTION_CLASS_MAX <= 7, "class bits");

#define SWITCH_PROTECTION_EPOCH        "/private/var/Switch/protection-epoch"
#define SWITCH_PROTECTION_CACHE_SIZE   2048u
#define SWITCH_PROTECTION_CACHE_SHARDS 16u
#define SWITCH_PROT_CLASS_MASK         0x07u
#define SWITCH_PROT_CLASS_KNOWN        0x20u
#define SWITCH_PROT_LABELLED           0x40u
#define SWITCH_PROT_LABEL_KNOWN        0x80u

struct switch_protection_slot {
    uint32_t seq;
    uint32_t vid;
    vnode_t vp;
    uint8_t state;
};

struct switch_protection_shard {
    lck_spin_t lock;
    uint32_t relabel_gen;
    uint32_t relabel_active;
};

static LCK_GRP_DECLARE(switch_protection_lck_grp, "switch-protection");
static struct switch_protection_slot switch_protection_cache[SWITCH_PROTECTION_CACHE_SIZE];
static struct switch_protection_shard switch_protection_shards[SWITCH_PROTECTION_CACHE_SHARDS];

static struct {
    mount_t data;
    uint64_t epoch;
} switch_protection;

static inline uint32_t switch_protection_slot_index(vnode_t vp)
{
    const uintptr_t bits = (uintptr_t)vp;
    return (uint32_t)(((bits >> 8) ^ (bits >> 19)) & (SWITCH_PROTECTION_CACHE_SIZE - 1u));
}

static inline struct switch_protection_shard *switch_protection_shard_for(uint32_t idx)
{
    return &switch_protection_shards[idx & (SWITCH_PROTECTION_CACHE_SHARDS - 1u)];
}

static bool switch_protection_cache_get(vnode_t vp, uint8_t *state_out)
{
    const uint32_t vid = vnode_vid(vp);
    const struct switch_protection_slot *slot = &switch_protection_cache[switch_protection_slot_index(vp)];
    const uint32_t seq1 = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if ((seq1 & 1u) != 0u) {
        return false;
    }
    const vnode_t cached_vp = __atomic_load_n(&slot->vp, __ATOMIC_RELAXED);
    const uint32_t cached_vid = __atomic_load_n(&slot->vid, __ATOMIC_RELAXED);
    const uint8_t state = __atomic_load_n(&slot->state, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot->seq, __ATOMIC_RELAXED) != seq1 || cached_vp != vp || cached_vid != vid ||
        (state & SWITCH_PROT_LABEL_KNOWN) == 0u) {
        return false;
    }
    *state_out = state;
    return true;
}

static void switch_protection_slot_write_locked(struct switch_protection_slot *slot, vnode_t vp, uint32_t vid,
                                                uint8_t state)
{
    const uint32_t seq = slot->seq;
    __atomic_store_n(&slot->seq, seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot->vp, vp, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->vid, vid, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->state, state, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->seq, seq + 2u, __ATOMIC_RELEASE);
}

static void switch_protection_cache_fill(vnode_t vp, uint32_t gen, uint8_t state)
{
    const uint32_t idx = switch_protection_slot_index(vp);
    struct switch_protection_shard *shard = switch_protection_shard_for(idx);
    const uint32_t vid = vnode_vid(vp);
    lck_spin_lock(&shard->lock);
    if (shard->relabel_active == 0u && shard->relabel_gen == gen) {
        switch_protection_slot_write_locked(&switch_protection_cache[idx], vp, vid, state);
    }
    lck_spin_unlock(&shard->lock);
}

static void switch_protection_cache_seed_new(vnode_t vp, uint8_t state)
{
    const uint32_t idx = switch_protection_slot_index(vp);
    struct switch_protection_shard *shard = switch_protection_shard_for(idx);
    const uint32_t vid = vnode_vid(vp);
    lck_spin_lock(&shard->lock);
    if (shard->relabel_active == 0u) {
        switch_protection_slot_write_locked(&switch_protection_cache[idx], vp, vid, state);
    }
    lck_spin_unlock(&shard->lock);
}

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
        for (uint32_t i = 0; i < SWITCH_PROTECTION_CACHE_SHARDS; ++i) {
            lck_spin_init(&switch_protection_shards[i].lock, &switch_protection_lck_grp, LCK_ATTR_NULL);
        }
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

static int switch_protection_label_read(vnode_t vp, int32_t *pclass, vfs_context_t ctx)
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

static int switch_protection_label(vnode_t vp, int32_t *pclass, vfs_context_t ctx)
{
    uint8_t state = 0;
    if (switch_protection_cache_get(vp, &state)) {
        if (state & SWITCH_PROT_LABELLED) {
            *pclass = (int32_t)(state & SWITCH_PROT_CLASS_MASK);
            return 0;
        }
        return ENOATTR;
    }
    const struct switch_protection_shard *shard = switch_protection_shard_for(switch_protection_slot_index(vp));
    const uint32_t gen = __atomic_load_n(&shard->relabel_gen, __ATOMIC_ACQUIRE);
    const int error = switch_protection_label_read(vp, pclass, ctx);
    if (error == 0) {
        switch_protection_cache_fill(vp, gen,
                                     (uint8_t)(SWITCH_PROT_LABEL_KNOWN | SWITCH_PROT_LABELLED |
                                               SWITCH_PROT_CLASS_KNOWN | ((uint32_t)*pclass & SWITCH_PROT_CLASS_MASK)));
    } else if (error == ENOATTR) {
        switch_protection_cache_fill(vp, gen, SWITCH_PROT_LABEL_KNOWN);
    }
    return error;
}

static int32_t switch_protection_class_with_va(vnode_t vp, const struct vnode_attr *known_va, vfs_context_t ctx)
{
    uint8_t state = 0;
    bool unlabelled_known = false;
    if (switch_protection_cache_get(vp, &state)) {
        if (state & SWITCH_PROT_CLASS_KNOWN) {
            return (int32_t)(state & SWITCH_PROT_CLASS_MASK);
        }
        unlabelled_known = true;
    }
    const struct switch_protection_shard *shard = switch_protection_shard_for(switch_protection_slot_index(vp));
    const uint32_t gen = __atomic_load_n(&shard->relabel_gen, __ATOMIC_ACQUIRE);
    int label_error = ENOATTR;
    if (!unlabelled_known) {
        int32_t pclass = 0;
        label_error = switch_protection_label_read(vp, &pclass, ctx);
        if (label_error == 0) {
            switch_protection_cache_fill(vp, gen,
                                         (uint8_t)(SWITCH_PROT_LABEL_KNOWN | SWITCH_PROT_LABELLED |
                                                   SWITCH_PROT_CLASS_KNOWN |
                                                   ((uint32_t)pclass & SWITCH_PROT_CLASS_MASK)));
            return pclass;
        }
    }
    bool known = false;
    uint64_t fileid = 0;
    if (known_va != NULL && VATTR_IS_SUPPORTED(known_va, va_fileid)) {
        known = true;
        fileid = known_va->va_fileid;
    } else {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        known = vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_fileid);
        fileid = known ? va.va_fileid : 0;
    }
    const int32_t pclass = switch_protection_unlabelled(vnode_isdir(vp), known, fileid, switch_protection.epoch);
    if (label_error == ENOATTR && known) {
        switch_protection_cache_fill(vp, gen,
                                     (uint8_t)(SWITCH_PROT_LABEL_KNOWN | SWITCH_PROT_CLASS_KNOWN |
                                               ((uint32_t)pclass & SWITCH_PROT_CLASS_MASK)));
    }
    return pclass;
}

static int32_t switch_protection_class(vnode_t vp, vfs_context_t ctx)
{
    return switch_protection_class_with_va(vp, NULL, ctx);
}

static int switch_protection_write_label(vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    const uint32_t idx = switch_protection_slot_index(vp);
    struct switch_protection_shard *shard = switch_protection_shard_for(idx);
    struct switch_protection_slot *slot = &switch_protection_cache[idx];
    const uint32_t vid = vnode_vid(vp);

    lck_spin_lock(&shard->lock);
    const uint32_t start_gen = shard->relabel_gen + 1u;
    __atomic_store_n(&shard->relabel_gen, start_gen, __ATOMIC_RELEASE);
    ++shard->relabel_active;
    if (slot->vp == vp && slot->vid == vid) {
        switch_protection_slot_write_locked(slot, NULLVP, 0, 0);
    }
    lck_spin_unlock(&shard->lock);

    uint32_t label = (uint32_t)pclass;
    UIO_STACKBUF(uio_buf, 1);
    uio_t uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_WRITE, uio_buf, sizeof(uio_buf));
    uio_addiov(uio, CAST_USER_ADDR_T(&label), sizeof(label));
    const int error = vn_setxattr(vp, SWITCH_PROTECTION_XATTR, uio, XATTR_NOSECURITY, ctx);

    lck_spin_lock(&shard->lock);
    --shard->relabel_active;
    const bool solo = shard->relabel_active == 0u && shard->relabel_gen == start_gen;
    __atomic_store_n(&shard->relabel_gen, shard->relabel_gen + 1u, __ATOMIC_RELEASE);
    if (error == 0 && solo) {
        switch_protection_slot_write_locked(
            slot, vp, vid,
            (uint8_t)(SWITCH_PROT_LABEL_KNOWN | SWITCH_PROT_LABELLED | SWITCH_PROT_CLASS_KNOWN |
                      ((uint32_t)pclass & SWITCH_PROT_CLASS_MASK)));
    }
    lck_spin_unlock(&shard->lock);
    return error;
}

static int switch_protection_relabel(vnode_t vp, int32_t pclass, vfs_context_t ctx)
{
    if (switch_protection_class(vp, ctx) == pclass) {
        return 0;
    }
    return switch_protection_write_label(vp, pclass, ctx);
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
    vap->va_dataprotect_class =
        switch_protection_volume(vp) ? switch_protection_class_with_va(vp, vap, ctx) : PROTECTION_CLASS_D;
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
    if (label == PROTECTION_CLASS_DEFAULT) {
        const int32_t def = switch_protection_unlabelled(vnode_isdir(vp), false, 0, 0);
        switch_protection_cache_seed_new(
            vp, (uint8_t)(SWITCH_PROT_LABEL_KNOWN | SWITCH_PROT_CLASS_KNOWN | ((uint32_t)def & SWITCH_PROT_CLASS_MASK)));
        return 0;
    }
    return switch_protection_write_label(vp, label, ctx);
}

int switch_protection_listxattr(vnode_t vp, uio_t uio, size_t *size, int options, vfs_context_t ctx)
{
    int32_t label;
    if (!switch_protection_volume(vp) || switch_protection_label(vp, &label, ctx) == ENOATTR) {
        return vn_listxattr(vp, uio, size, options, ctx);
    }
    char stack_names[256];
    UIO_STACKBUF(stack_uio_buf, 1);
    uio_t stack_uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, stack_uio_buf, sizeof(stack_uio_buf));
    uio_addiov(stack_uio, CAST_USER_ADDR_T(stack_names), sizeof(stack_names));
    size_t listed = 0;
    int error = vn_listxattr(vp, stack_uio, &listed, options, ctx);
    if (!error) {
        const size_t length =
            switch_protection_hide(stack_names, sizeof(stack_names) - (size_t)uio_resid(stack_uio),
                                   SWITCH_PROTECTION_XATTR);
        if (uio == NULL) {
            *size = length;
        } else if (length > (size_t)uio_resid(uio)) {
            error = ERANGE;
        } else if (length > 0) {
            error = uiomove(stack_names, (int)length, uio);
        }
        return error;
    }
    if (error != ERANGE) {
        return error;
    }
    size_t names_size = 0;
    error = vn_listxattr(vp, NULL, &names_size, options, ctx);
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
    listed = 0;
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
