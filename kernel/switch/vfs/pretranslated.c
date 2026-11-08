// SPDX-License-Identifier: GPL-2.0-only
/*
 * iOS 27's System volume stays sealed as Apple shipped it. The image builder puts the plain arm64 translation
 * of each of its Mach-O files on Data, at /private/var/Switch/pretranslated/<System path>, and the tree's index
 * (kits/SwitchPretranslated) lists each file's System and Data file ids and the tree's directory ids. A lookup that
 * ends on a listed System file returns its Data copy, so exec and dyld map code that needs no translation at page-in.
 *
 * The tree is captured once, before launchd: its root vnode is kept and its index is read through it, so renaming or
 * replacing /private/var/Switch later changes nothing. The copy is found by its file id, never by a path, and only if
 * it is a regular file whose parent is one of the tree's directories. The trust module the boot package carries for
 * the tree's executables (its UUID in /chosen/switch-pretranslated-trust) must be loaded too. Anything wrong turns the
 * redirect off with one log line: the original files then run translated at load time, as without it.
 *
 * Nothing on the device changes the tree, root included: the MAC check wrappers (security/mac_vfs.c, mac_file.c) ask
 * switch_pretranslated_refuse() before any policy, and it refuses with EPERM (one rate-limited log line) whatever
 * switch_pt_denied() says touches a vnode of the tree, the tree's root and its index among them. Only a vnode on the
 * Data mount costs a file id lookup; the hot paths of every other volume compare one pointer.
 */
#if CONFIG_A64X
#include <sys/param.h>
#include <sys/file_internal.h>
#include <sys/mman.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/trust_caches.h>
#include <sys/ubc_internal.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <mach/machine.h>
#include <os/atomic_private.h>
#include <pexpert/device_tree.h>

#include <switch/auth_fs.h>
#include <switch/pretranslated.h>

#define SWITCH_PT_TREE   "Switch/pretranslated"
#define SWITCH_PT_INDEX  ".index"
#define SWITCH_PT_LOOKUP (VNODE_LOOKUP_NOFOLLOW_ANY | VNODE_LOOKUP_NOCROSSMOUNT)
/* The UUID of the trust module the boot package carries for this tree's executables (the os role's). */
#define SWITCH_PT_TRUST_PROPERTY "switch-pretranslated-trust"

bool switch_pretranslated_on = false;

static struct {
    mount_t system;
    mount_t data;
    vnode_t root;
    uint8_t *raw;
    size_t raw_size;
    void *storage;
    size_t storage_size;
    struct switch_pt_tables tables;
    uint8_t trust[kUUIDSize];
} switch_pt;

unsigned int hash_string(const char *cp, int len);

static uint32_t switch_pt_name_hash(const char *name, uint32_t length)
{
    return hash_string(name, (int)length);
}

static bool switch_pt_log_allowed(void)
{
    static uint64_t window;
    static uint32_t lines;
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    const uint64_t second = now / NSEC_PER_SEC;
    if (os_atomic_load(&window, relaxed) != second) {
        os_atomic_store(&window, second, relaxed);
        os_atomic_store(&lines, 0, relaxed);
    }
    return os_atomic_inc_orig(&lines, relaxed) < 4;
}

static int switch_pt_owned_by_root(vnode_t vp, enum vtype type, vfs_context_t ctx, uint64_t *id, off_t *size)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_data_size);
    int error = vnode_getattr(vp, &va, ctx);
    if (error) {
        return error;
    }
    if (vnode_vtype(vp) != type || !VATTR_IS_SUPPORTED(&va, va_fileid) || va.va_uid != 0 || va.va_gid != 0 ||
        (va.va_mode & (S_IWGRP | S_IWOTH))) {
        return EPERM;
    }
    *id = va.va_fileid;
    if (size) {
        *size = (off_t)va.va_data_size;
    }
    return 0;
}

static int switch_pt_read_index(vnode_t tree, vfs_context_t ctx, uint64_t *index_id)
{
    vnode_t index = NULLVP;
    off_t size = 0;
    int error = vnode_lookupat(SWITCH_PT_INDEX, SWITCH_PT_LOOKUP, &index, ctx, tree);
    if (!error && vnode_mount(index) != switch_pt.data) {
        error = EXDEV;
    }
    if (!error) {
        error = switch_pt_owned_by_root(index, VREG, ctx, index_id, &size);
    }
    if (!error && (size < SWITCH_PT_HEADER || (uint64_t)size > SWITCH_PT_MAX_INDEX)) {
        error = EFBIG;
    }
    if (!error) {
        int resid = 0;
        switch_pt.raw = kalloc_data((vm_size_t)size, Z_WAITOK);
        switch_pt.raw_size = (size_t)size;
        error = vn_rdwr(UIO_READ, index, (caddr_t)switch_pt.raw, (int)size, 0, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(ctx), &resid, current_proc());
        if (!error && resid) {
            error = EIO;
        }
    }
    if (index) {
        vnode_put(index);
    }
    return error;
}

static int switch_pt_read_trust(void)
{
    DTEntry chosen;
    const void *uuid = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, SWITCH_PT_TRUST_PROPERTY, &uuid, &size) != kSuccess) {
        return ENOENT;
    }
    if (size != sizeof(switch_pt.trust)) {
        return EINVAL;
    }
    memcpy(switch_pt.trust, uuid, sizeof(switch_pt.trust));
    return check_trust_cache_runtime_for_uuid(switch_pt.trust) == KERN_SUCCESS ? 0 : ESRCH;
}

static const char *switch_pt_build(vnode_t tree, vfs_context_t ctx, int *error)
{
    uint64_t tree_id = 0, index_id = 0;
    struct switch_pt_index index;
    if ((*error = switch_pt_owned_by_root(tree, VDIR, ctx, &tree_id, NULL))) {
        return "tree";
    }
    if ((*error = switch_pt_read_index(tree, ctx, &index_id))) {
        return "index";
    }
    enum switch_pt_error parsed = switch_pt_index_parse(switch_pt.raw, switch_pt.raw_size, &index);
    if (parsed != SWITCH_PT_OK) {
        *error = (int)parsed;
        return "index contents";
    }
    switch_pt.storage_size = switch_pt_tables_bytes(&index, 1);
    switch_pt.storage = kalloc_data(switch_pt.storage_size, Z_WAITOK | Z_ZERO);
    parsed = switch_pt_tables_build(&switch_pt.tables, &index, 1, switch_pt.storage, switch_pt.storage_size,
                                    switch_pt_name_hash);
    if (parsed == SWITCH_PT_OK && !switch_pt_map_put(&switch_pt.tables.data, index_id, SWITCH_PT_PROTECTED)) {
        parsed = SWITCH_PT_DUPLICATE_ID;
    }
    uint32_t root_kind = 0;
    if (parsed == SWITCH_PT_OK &&
        (!switch_pt_map_get(&switch_pt.tables.data, tree_id, &root_kind) || root_kind != SWITCH_PT_DIRECTORY)) {
        parsed = SWITCH_PT_DIRECTORY_BAD;
    }
    if (parsed != SWITCH_PT_OK) {
        *error = (int)parsed;
        return "index ids";
    }
    return NULL;
}

static void switch_pt_release(void)
{
    if (switch_pt.storage) {
        kfree_data(switch_pt.storage, switch_pt.storage_size);
    }
    if (switch_pt.raw) {
        kfree_data(switch_pt.raw, switch_pt.raw_size);
    }
    if (switch_pt.root) {
        vnode_rele(switch_pt.root);
    }
    bzero(&switch_pt, sizeof(switch_pt));
}

/* Before launchd, once Data is mounted at /private/var: nothing else runs yet, so its path is still the one mounted. */
void switch_pretranslated_capture(void)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t data_root = NULLVP, tree = NULLVP;
    const char *failed = NULL;
    int error = vnode_lookup("/private/var", 0, &data_root, ctx);
    if (error || !vnode_isvroot(data_root) || (vfs_flags(vnode_mount(data_root)) & MNT_RDONLY) ||
        vnode_mount(data_root) == rootvnode->v_mount) {
        failed = "Data mount";
        error = error ? error : EINVAL;
    }
    if (!failed && (error = switch_pt_read_trust())) {
        failed = "trust module";
    }
    if (!failed) {
        switch_pt.system = rootvnode->v_mount;
        switch_pt.data = vnode_mount(data_root);
        error = vnode_lookupat(SWITCH_PT_TREE, SWITCH_PT_LOOKUP, &tree, ctx, data_root);
        if (!error && vnode_mount(tree) != switch_pt.data) {
            error = EXDEV;
        }
        failed = error ? "tree" : switch_pt_build(tree, ctx, &error);
    }
    if (!failed && (error = vnode_ref(tree))) {
        failed = "tree reference";
    }
    if (!failed) {
        switch_pt.root = tree;
    }
    if (tree) {
        vnode_put(tree);
    }
    if (data_root) {
        vnode_put(data_root);
    }
    if (failed) {
        printf("SwitchPretranslated: off (%s: %d); the System files run translated at load time\n", failed, error);
        switch_pt_release();
        return;
    }
    const fsid_t fsid = vfs_statfs(switch_pt.data)->f_fsid;
    printf("SwitchPretranslated: %u files from /private/var/" SWITCH_PT_TREE " (fsid %x:%x)\n",
           switch_pt.tables.index.count, (uint32_t)fsid.val[0], (uint32_t)fsid.val[1]);
    os_atomic_store(&switch_pretranslated_on, true, release);
}

static int switch_pt_copy(uint64_t data_id, vfs_context_t ctx, vnode_t *copy)
{
    struct vnode_attr va;
    uint32_t parent_kind = 0, listed = 0;
    int error = VFS_VGET(switch_pt.data, (ino64_t)data_id, copy, ctx);
    if (error) {
        return error;
    }
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_parentid);
    error = vnode_getattr(*copy, &va, ctx);
    if (!error && (vnode_mount(*copy) != switch_pt.data || !vnode_isreg(*copy) || vnode_isnamedstream(*copy) ||
                   !VATTR_IS_SUPPORTED(&va, va_parentid) || va.va_fileid != data_id ||
                   !switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, &listed) || listed >= SWITCH_PT_PROTECTED ||
                   !switch_pt_map_get(&switch_pt.tables.data, va.va_parentid, &parent_kind) ||
                   parent_kind != SWITCH_PT_DIRECTORY)) {
        error = EPERM;
    }
    if (error) {
        vnode_put(*copy);
        *copy = NULLVP;
    }
    return error;
}

/* The end of a lookup (lookup(), with an iocount on ni_vp): a listed System file becomes its Data copy, named as the
 * System file is (its parent and name), so paths built from it are the System path. */
void switch_pretranslated_redirect(struct nameidata *ndp)
{
    struct componentname *cnp = &ndp->ni_cnd;
    vnode_t vp = ndp->ni_vp;
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || vp == NULLVP || vp->v_type != VREG ||
        vp->v_mount != switch_pt.system || cnp->cn_nameiop != LOOKUP || (cnp->cn_flags & CN_WANTSRSRCFORK) ||
        vnode_isnamedstream(vp) || cnp->cn_namelen <= 0) {
        return;
    }
    const uint32_t length = (uint32_t)cnp->cn_namelen;
    const uint32_t hash = cnp->cn_hash ? cnp->cn_hash : hash_string(cnp->cn_nameptr, cnp->cn_namelen);
    if (!switch_pt_name_listed(&switch_pt.tables, hash, cnp->cn_nameptr, length)) {
        return;
    }
    vfs_context_t ctx = cnp->cn_context ? cnp->cn_context : vfs_context_current();
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    if (vnode_getattr(vp, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid)) {
        return;
    }
    const int64_t file = switch_pt_system_file(&switch_pt.tables, va.va_fileid, hash, cnp->cn_nameptr, length);
    if (file < 0) {
        return;
    }
    const struct switch_pt_file listed = switch_pt_file_at(&switch_pt.tables.index, (uint32_t)file);
    vnode_t copy = NULLVP;
    int error = switch_pt_copy(listed.data_id, ctx, &copy);
    if (error) {
        if (switch_pt_log_allowed()) {
            printf("SwitchPretranslated: %s stays untranslated: %d\n", listed.path, error);
        }
        return;
    }
    if (copy->v_parent != vp->v_parent || copy->v_name == NULL) {
        vnode_t parent = vnode_getparent(vp);
        if (parent != NULLVP) {
            vnode_update_identity(copy, parent, cnp->cn_nameptr, cnp->cn_namelen, hash,
                                  VNODE_UPDATE_PARENT | (copy->v_name ? 0 : VNODE_UPDATE_NAME));
            vnode_put(parent);
        }
    }
    vnode_put(vp);
    ndp->ni_vp = copy;
}

static const char *switch_pt_op_name(enum switch_pt_op op)
{
    static const char *const names[SWITCH_PT_OPS] = {
        [SWITCH_PT_OPEN] = "write open",
        [SWITCH_PT_TRUNCATE] = "truncate",
        [SWITCH_PT_SETATTR] = "attribute change",
        [SWITCH_PT_SETEXTATTR] = "xattr write",
        [SWITCH_PT_DELETEEXTATTR] = "xattr removal",
        [SWITCH_PT_UNLINK] = "unlink",
        [SWITCH_PT_RENAME] = "rename",
        [SWITCH_PT_LINK] = "link",
        [SWITCH_PT_CREATE] = "create",
        [SWITCH_PT_EXCHANGEDATA] = "exchangedata",
        [SWITCH_PT_CLONE] = "clone",
        [SWITCH_PT_COPYFILE] = "copyfile",
        [SWITCH_PT_FSCTL] = "fsctl or ioctl",
        [SWITCH_PT_MMAP] = "shared writable mapping",
        [SWITCH_PT_MOUNT] = "mount",
        [SWITCH_PT_GRAFT] = "graft",
    };
    return op < SWITCH_PT_OPS ? names[op] : "change";
}

/* Membership is decided from the vnode's own identity, not by walking paths. A vnode off the Data mount is never in
 * the tree (one pointer, no I/O): every other volume's hot path stops here, so ordinary files are untouched. For a
 * vnode on the Data mount the file id comes from its in-core node (no device read for a resident vnode); if even that
 * cannot be read the write is refused, since the id can only be missing for a vnode we failed to identify. */
static bool switch_pt_in_tree(vnode_t vp)
{
    if (vp == NULLVP || vp->v_mount != switch_pt.data) {
        return false;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    if (vnode_getattr(vp, &va, vfs_context_kernel()) || !VATTR_IS_SUPPORTED(&va, va_fileid)) {
        return true;
    }
    return switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, NULL);
}

int switch_pretranslated_refuse(enum switch_pt_op op, vnode_t v0, vnode_t v1, vnode_t v2, vnode_t v3, uint32_t intent)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || !switch_pt_denied(op, SWITCH_PT_ANY_VNODE, intent)) {
        return 0;
    }
    const vnode_t vnodes[4] = {v0, v1, v2, v3};
    uint32_t in_tree = 0;
    for (uint32_t i = 0; i < 4 && !switch_pt_denied(op, in_tree, intent); ++i) {
        if (switch_pt_denied(op, SWITCH_PT_VNODE(i), intent) && switch_pt_in_tree(vnodes[i])) {
            in_tree |= SWITCH_PT_VNODE(i);
        }
    }
    if (!switch_pt_denied(op, in_tree, intent)) {
        return 0;
    }
    if (switch_pt_log_allowed()) {
        proc_t p = current_proc();
        printf("SwitchPretranslated: refused %s in /private/var/" SWITCH_PT_TREE " by %s[%d]\n", switch_pt_op_name(op),
               p ? proc_best_name(p) : "kernel", p ? proc_getpid(p) : 0);
    }
    return EPERM;
}

static bool switch_pt_cdhash_pinned(const uint8_t *cdhash)
{
    uint8_t uuid[kUUIDSize];
    return switch_trust_module_uuid(cdhash, uuid) && memcmp(uuid, switch_pt.trust, sizeof(uuid)) == 0;
}

static bool switch_pt_signed_as(vnode_t vp, const uint8_t *cdhash)
{
    bool found = false;
    vnode_lock_spin(vp);
    for (const struct cs_blob *blob = ubc_get_cs_blobs(vp); blob && !found; blob = blob->csb_next) {
        found = memcmp(blob->csb_cdhash, cdhash, SWITCH_PT_CDHASH) == 0;
    }
    vnode_unlock(vp);
    return found;
}

/*
 * The System files the tree stands in for are on the sealed System volume; their Data copies are authenticated by
 * their trust module instead (auth_fs.c). Decided from the vnode alone, never from the path it was found by.
 */
bool switch_pretranslated_authentic(vnode_t vp)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || vp == NULLVP || vp->v_mount != switch_pt.data ||
        !vnode_isreg(vp)) {
        return false;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    if (vnode_getattr(vp, &va, vfs_context_kernel()) || !VATTR_ALL_SUPPORTED(&va)) {
        return false;
    }
    uint32_t file = SWITCH_PT_PROTECTED;
    if (!switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, &file) || file >= SWITCH_PT_PROTECTED) {
        return false;
    }
    const uint8_t *cdhash = switch_pt_file_at(&switch_pt.tables.index, file).cdhash;
    const struct switch_pt_trust_facts facts = {
        .listed = true,
        .regular = true,
        .uid = va.va_uid,
        .gid = va.va_gid,
        .mode = va.va_mode,
        .cdhash_pinned = switch_pt_cdhash_pinned(cdhash),
        .cdhash_matches = switch_pt_signed_as(vp, cdhash),
    };
    const bool authentic = switch_pt_trusted(&facts);
    if (!authentic && switch_pt_log_allowed()) {
        printf("SwitchPretranslated: file %llu is not authenticated (%u:%u %o, CDHash pinned %d matches %d)\n",
               (unsigned long long)va.va_fileid, facts.uid, facts.gid, facts.mode, facts.cdhash_pinned,
               facts.cdhash_matches);
    }
    return authentic;
}

int switch_pretranslated_mmap_check(struct fileglob *fg, int flags, int prot, int maxprot)
{
    if (FILEGLOB_DTYPE(fg) == DTYPE_VNODE && (flags & MAP_SHARED) &&
        ((prot & PROT_WRITE) || (maxprot & VM_PROT_WRITE))) {
        return switch_pretranslated_check(SWITCH_PT_MMAP, (vnode_t)fg_get_data(fg), NULL, NULL, NULL, SWITCH_PT_WRITES);
    }
    return 0;
}
#endif
