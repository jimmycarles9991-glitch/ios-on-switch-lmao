// SPDX-License-Identifier: GPL-2.0-only
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/proc_internal.h>
#include <sys/stat.h>
#include <sys/vm.h>
#include <sys/spawn_internal.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vnode_internal.h>
#include <sys/uio_internal.h>
#include <sys/codesign.h>
#include <sys/ubc_internal.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/cs_blobs.h>
#include <switch/bootstrap.h>
#include <switch/migration_home.h>

#define SWITCH_BOOTSTRAP_BASE "/private/var/Switch/bootstrap/" SWITCH_IPSW_BUILD
#define SWITCH_BOOTSTRAP_ROOT SWITCH_BOOTSTRAP_BASE "/root"
#define SWITCH_BOOTSTRAP_HELPER "/usr/libexec/switch-bootstrap"

static vnode_t switch_bootstrap_root;
static mount_t switch_bootstrap_data;
static uint64_t switch_bootstrap_data_id;
static uint64_t switch_bootstrap_root_id;
static int switch_bootstrap_ready, switch_bootstrap_recovery;
LCK_GRP_DECLARE(switch_bootstrap_lck_grp, "switch-bootstrap");
LCK_MTX_DECLARE(switch_bootstrap_lock, &switch_bootstrap_lck_grp);

static int switch_bootstrap_lookup_path(const char *path, vnode_t directory, vnode_t *vp)
{
    struct nameidata *nd = kalloc_type(struct nameidata, Z_WAITOK);
    if (!nd) return ENOMEM;
    NDINIT(nd, LOOKUP, OP_LOOKUP, NOFOLLOW | (directory ? NOCROSSMOUNT : 0), UIO_SYSSPACE,
           CAST_USER_ADDR_T(path), vfs_context_kernel());
    nd->ni_flag |= NAMEI_NOFOLLOW_ANY;
    if (directory) {
        nd->ni_dvp = directory;
        nd->ni_cnd.cn_flags |= USEDVP;
    }
    const int error = namei(nd);
    if (!error) {
        nd->ni_cnd.cn_flags &= ~USEDVP;
        *vp = nd->ni_vp;
        nameidone(nd);
    }
    kfree_type(struct nameidata, nd);
    return error;
}

static bool switch_bootstrap_attributes(vnode_t vp, bool directory, uint64_t *id, uint64_t *parent)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_nlink);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_parentid);
    VATTR_WANTED(&va, va_acl);
    if (vnode_getattr(vp, &va, vfs_context_kernel())) return false;
    const bool acl = VATTR_IS_SUPPORTED(&va, va_acl) && va.va_acl != NULL;
    if (acl) kauth_acl_free(va.va_acl);
    const bool symlink = !directory && vnode_islnk(vp);
    if (!VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_gid) ||
        !VATTR_IS_SUPPORTED(&va, va_mode) || !VATTR_IS_SUPPORTED(&va, va_nlink) ||
        !VATTR_IS_SUPPORTED(&va, va_fileid) || !VATTR_IS_SUPPORTED(&va, va_parentid) || acl ||
        (directory ? !vnode_isdir(vp) : !vnode_isreg(vp) && !symlink) || vnode_isnamedstream(vp) ||
        !switch_bootstrap_owned(va.va_uid, va.va_gid, symlink ? va.va_mode & ~0022u : va.va_mode,
                                va.va_nlink, directory)) return false;
    *id = va.va_fileid;
    *parent = va.va_parentid;
    return true;
}

bool switch_bootstrap_member(vnode_t vp, bool directory)
{
    vnode_t root = __atomic_load_n(&switch_bootstrap_root, __ATOMIC_ACQUIRE);
    if (!root || !vp || vnode_mount(vp) != switch_bootstrap_data) return false;
    const uint64_t root_id = __atomic_load_n(&switch_bootstrap_root_id, __ATOMIC_RELAXED);
    vnode_t node = vp;
    if (vnode_get(node)) return false;
    bool found = false, complete = false;
    for (unsigned depth = 0; depth < MAXPATHLEN / 2; ++depth) {
        uint64_t id = 0, parent = 0;
        if (!switch_bootstrap_attributes(node, directory, &id, &parent)) { found = false; break; }
        if (node == root && id == root_id) found = directory;
        if (id == switch_bootstrap_data_id) { complete = true; break; }
        if (!parent || parent == id) { found = false; break; }
        vnode_t next = NULLVP;
        if (VFS_VGET(switch_bootstrap_data, (ino64_t)parent, &next, vfs_context_kernel())) { found = false; break; }
        vnode_put(node);
        node = next;
        directory = true;
    }
    vnode_put(node);
    return found && complete;
}

bool switch_bootstrap_active(void)
{
    return __atomic_load_n(&switch_bootstrap_root, __ATOMIC_RELAXED) != NULLVP;
}

bool switch_bootstrap_on_data(vnode_t vp)
{
    return vp && __atomic_load_n(&switch_bootstrap_root, __ATOMIC_RELAXED) != NULLVP &&
        vnode_mount(vp) == switch_bootstrap_data;
}

bool switch_bootstrap_image(vnode_t vp)
{
    return vp && vnode_isreg(vp) && switch_bootstrap_member(vp, false);
}

bool switch_bootstrap_tool_image(vnode_t vp, int64_t offset)
{
    if (!switch_bootstrap_sandbox_target(vp, offset)) return false;
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    const bool tool = !vn_getpath(vp, path, &length) &&
        switch_bootstrap_tool_path(path, SWITCH_BOOTSTRAP_ROOT);
    zfree(ZV_NAMEI, path);
    return tool;
}

static bool switch_bootstrap_named(vnode_t vp, const char *name)
{
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    const bool matched = vp && !vn_getpath(vp, path, &length) && !strcmp(path, name);
    zfree(ZV_NAMEI, path);
    return matched;
}

bool switch_bootstrap_persona_caller(proc_t p)
{
    const unsigned int required = CS_VALID | CS_SIGNED;
    if (!p || !p->p_textvp || (proc_getcsflags(p) & required) != required || !csproc_get_platform_binary(p) ||
        !IOTaskHasEntitlement(proc_task(p), "com.apple.private.persona-mgmt")) return false;
    struct cs_blob *blob = csproc_get_blob(p);
    const char *identity = blob ? csblob_get_identity(blob) : NULL;
    if (!identity) return false;
    vnode_t vp = p->p_textvp;
    return (proc_getcsflags(p) & CS_ENTITLEMENTS_VALIDATED) &&
        blob->csb_reconstituted == SWITCH_BOOTSTRAP_SIGNATURE &&
        !strcmp(identity, SWITCH_BOOTSTRAP_SILEO_IDENTITY) &&
        switch_bootstrap_named(vp, SWITCH_BOOTSTRAP_ROOT "/Applications/Sileo.app/Sileo") &&
        switch_bootstrap_image(vp);
}

bool switch_bootstrap_service_caller(proc_t p)
{
    const unsigned int required = CS_VALID | CS_SIGNED;
    if (!p || !p->p_textvp || (proc_getcsflags(p) & required) != required ||
        !csproc_get_platform_binary(p) || !switch_bootstrap_named(p->p_textvp, SWITCH_BOOTSTRAP_HELPER)) return false;
    struct cs_blob *blob = csproc_get_blob(p);
    const char *identity = blob ? csblob_get_identity(blob) : NULL;
    return identity && blob->csb_reconstituted < 2 && !strcmp(identity, SWITCH_BOOTSTRAP_HELPER_IDENTITY);
}

static int switch_bootstrap_capture_root(void)
{
    vnode_t data = NULLVP, root = NULLVP;
    int error = switch_bootstrap_lookup_path("/private/var", NULLVP, &data);
    if (!error && vnode_mount(data) != switch_bootstrap_data) error = EXDEV;
    if (!error) error = switch_bootstrap_lookup_path("Switch/bootstrap/" SWITCH_IPSW_BUILD "/root", data, &root);
    uint64_t id = 0, parent = 0;
    if (!error && (!switch_bootstrap_attributes(root, true, &id, &parent) ||
        vnode_mount(root) != switch_bootstrap_data)) error = EPERM;
    if (!error) error = vnode_ref(root);
    if (!error) {
        lck_mtx_lock(&switch_bootstrap_lock);
        vnode_t previous = switch_bootstrap_root;
        if (previous == root && switch_bootstrap_root_id == id) {
            lck_mtx_unlock(&switch_bootstrap_lock);
            vnode_rele(root);
        } else {
            __atomic_store_n(&switch_bootstrap_root_id, id, __ATOMIC_RELAXED);
            __atomic_store_n(&switch_bootstrap_root, root, __ATOMIC_RELEASE);
            lck_mtx_unlock(&switch_bootstrap_lock);
            if (previous) vnode_rele(previous);
        }
        if (!switch_bootstrap_member(root, true)) error = EPERM;
    }
    if (root) vnode_put(root);
    if (data) vnode_put(data);
    return error;
}

static bool switch_bootstrap_migration_file(const char *path, uint64_t min, uint64_t max)
{
    vnode_t vp = NULLVP;
    if (switch_bootstrap_lookup_path(path, NULLVP, &vp)) return false;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    const bool valid = vnode_isreg(vp) && vnode_mount(vp) == switch_bootstrap_data &&
        !vnode_getattr(vp, &va, vfs_context_kernel()) && VATTR_IS_SUPPORTED(&va, va_data_size) &&
        va.va_data_size >= min && va.va_data_size <= max;
    vnode_put(vp);
    return valid;
}

static bool switch_bootstrap_migration_active(const char *path)
{
    vnode_t vp = NULLVP;
    if (switch_bootstrap_lookup_path(path, NULLVP, &vp)) return false;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    char prefix[9] = {0};
    int resid = 0;
    const bool valid = vnode_isreg(vp) && vnode_mount(vp) == switch_bootstrap_data &&
        !vnode_getattr(vp, &va, vfs_context_kernel()) && VATTR_IS_SUPPORTED(&va, va_data_size) &&
        (va.va_data_size == 74 || va.va_data_size == 72) &&
        !vn_rdwr(UIO_READ, vp, prefix, (int)sizeof(prefix), 0, UIO_SYSSPACE, IO_NOAUTH,
                 vfs_context_ucred(vfs_context_kernel()), &resid, vfs_context_proc(vfs_context_kernel())) &&
        resid == 0 &&
        ((va.va_data_size == 74 && !memcmp(prefix, "complete ", 9)) ||
         (va.va_data_size == 72 && !memcmp(prefix, "repair ", 7)));
    vnode_put(vp);
    return valid;
}

void switch_bootstrap_capture(void)
{
    uint32_t disabled = 0;
    switch_bootstrap_recovery = PE_parse_boot_argn("switch-no-tweaks", &disabled, sizeof(disabled)) && disabled;
    vnode_t data = NULLVP;
    const char *step = "Data lookup";
    int error = switch_bootstrap_lookup_path("/private/var", NULLVP, &data);
    if (error) goto done;
    step = "Data volume root";
    if (!vnode_isvroot(data)) { error = EPERM; goto done; }
    step = "Data writable ownership";
    if (vfs_flags(vnode_mount(data)) & (MNT_RDONLY | MNT_IGNORE_OWNERSHIP)) { error = EPERM; goto done; }
    struct vnode_attr data_attributes;
    VATTR_INIT(&data_attributes);
    VATTR_WANTED(&data_attributes, va_fileid);
    step = "Data file identity";
    error = vnode_getattr(data, &data_attributes, vfs_context_kernel());
    if (error) goto done;
    if (!VATTR_IS_SUPPORTED(&data_attributes, va_fileid)) { error = ENOTSUP; goto done; }
    switch_bootstrap_data_id = data_attributes.va_fileid;
    switch_bootstrap_data = vnode_mount(data);
done:
    if (data) vnode_put(data);
    if (error) {
        IOLog("SwitchBootstrap: %s failed: %d\n", step, error);
        return;
    }
    const bool packaged = switch_bootstrap_migration_file("/private/var/Switch/overlay/usr/share/Switch/bootstrap/manifest.json", 1, 1024 * 1024) &&
        switch_bootstrap_migration_file("/private/var/Switch/overlay/usr/share/Switch/bootstrap/payload.tar", 512, INT64_MAX);
    const bool pending = packaged &&
        (switch_bootstrap_migration_file(SWITCH_BOOTSTRAP_BASE "/migration.pending", 0, 4096) ||
         !switch_bootstrap_migration_active(SWITCH_BOOTSTRAP_BASE "/migration.state"));
    switch_migration_home_prepare(packaged, pending);
#if DEVELOPMENT || DEBUG
    printf("SwitchBootstrap: captured " SWITCH_IPSW_BUILD " Data volume; recovery %d\n", switch_bootstrap_recovery);
#endif
}

mount_t switch_bootstrap_data_mount(void)
{
    return switch_bootstrap_data;
}

bool switch_bootstrap_target(vnode_t vp)
{
    if (!vp || switch_bootstrap_recovery || !os_atomic_load(&switch_bootstrap_ready, acquire)) return false;
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    const bool target = !vn_getpath(vp, path, &length) &&
        !switch_bootstrap_tool_path(path, SWITCH_BOOTSTRAP_ROOT) && switch_bootstrap_injectable(path);
    zfree(ZV_NAMEI, path);
    return target;
}

bool switch_bootstrap_process(proc_t p)
{
    return p && (__atomic_load_n(&p->p_lflag, __ATOMIC_RELAXED) & SWITCH_P_LTWEAKS);
}

void switch_bootstrap_exec(proc_t p, bool enabled)
{
    proc_lock(p);
    if (enabled) p->p_lflag |= SWITCH_P_LTWEAKS;
    else p->p_lflag &= ~SWITCH_P_LTWEAKS;
    proc_unlock(p);
    if (proc_getpid(p) == 1) os_atomic_store(&switch_bootstrap_ready, 0, release);
}

bool switch_bootstrap_alias(vnode_t vp)
{
    uint64_t id = 0, parent = 0;
    if (!switch_bootstrap_on_data(vp) || !vnode_islnk(vp) ||
        !switch_bootstrap_attributes(vp, false, &id, &parent) || parent != switch_bootstrap_data_id ||
        (!switch_bootstrap_named(vp, "/private/var/jb") && !switch_bootstrap_named(vp, "/var/jb"))) return false;
    char target[sizeof(SWITCH_BOOTSTRAP_ROOT)] = {0};
    UIO_STACKBUF(buffer, 1);
    uio_t io = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, buffer, sizeof(buffer));
    if (uio_addiov(io, CAST_USER_ADDR_T(target), sizeof(target))) return false;
    return !VNOP_READLINK(vp, io, vfs_context_kernel()) && uio_resid(io) == 1 &&
        !memcmp(target, SWITCH_BOOTSTRAP_ROOT, sizeof(target) - 1);
}

bool switch_bootstrap_access(vnode_t vp, int mode)
{
    if (!switch_bootstrap_on_data(vp) || (mode & (FWRITE | O_TRUNC | O_APPEND)) ||
        !switch_bootstrap_process(current_proc())) return false;
    if (switch_bootstrap_alias(vp)) return true;
    return switch_bootstrap_member(vp, vnode_isdir(vp));
}

static bool switch_bootstrap_root_component(const char *name, size_t length)
{
    for (const char *part = &SWITCH_BOOTSTRAP_ROOT[1]; *part;) {
        const char *end = strchr(part, '/');
        const size_t count = end ? (size_t)(end - part) : strlen(part);
        if (count == length && !memcmp(part, name, length)) return true;
        if (!end) break;
        part = end + 1;
    }
    return false;
}

bool switch_bootstrap_lookup(vnode_t directory, const char *name, size_t length)
{
    if (!switch_bootstrap_active() || !switch_bootstrap_process(current_proc()) || !directory || !name || !length ||
        length >= MAXPATHLEN || memchr(name, '/', length) ||
        (length == 2 && !memcmp(name, "..", 2))) return false;
    if (switch_bootstrap_access(directory, FREAD)) return true;
    uint64_t id = 0, parent = 0;
    if (length == 2 && !memcmp(name, "jb", 2) && vnode_mount(directory) == switch_bootstrap_data &&
        switch_bootstrap_attributes(directory, true, &id, &parent) && id == switch_bootstrap_data_id) return true;
    if (!switch_bootstrap_root_component(name, length)) return false;
    char *path = zalloc(ZV_NAMEI);
    int capacity = MAXPATHLEN;
    bool allowed = false;
    if (!vn_getpath(directory, path, &capacity)) {
        const size_t used = strlen(path);
        if (used + length + 2 <= MAXPATHLEN) {
            if (used > 1) path[used] = '/';
            const size_t start = used + (used > 1);
            memcpy(path + start, name, length);
            path[start + length] = '\0';
            const size_t end = start + length;
            const size_t root_length = sizeof(SWITCH_BOOTSTRAP_ROOT) - 1;
            allowed = end <= root_length && !memcmp(path, SWITCH_BOOTSTRAP_ROOT, end) &&
                (end == root_length || SWITCH_BOOTSTRAP_ROOT[end] == '/');
        }
    }
    zfree(ZV_NAMEI, path);
    return allowed;
}

static bool switch_bootstrap_below(const char *path, const char *root)
{
    const size_t size = strlen(root);
    return !strncmp(path, root, size) && (path[size] == '\0' || path[size] == '/');
}

bool switch_bootstrap_preflight(vnode_t directory, const char *path, size_t length)
{
    if (!switch_bootstrap_active() || !directory || !path || length >= MAXPATHLEN ||
        !switch_bootstrap_process(current_proc())) return false;
    const size_t size = strnlen(path, MAXPATHLEN);
    if (!size || size == MAXPATHLEN) return false;
    const bool absolute_input = *path == '/';
    if (absolute_input && !switch_bootstrap_below(path, SWITCH_BOOTSTRAP_ROOT) &&
        !switch_bootstrap_below(path, "/var/jb") && !switch_bootstrap_below(path, "/private/var/jb")) return false;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        const size_t count = end ? (size_t)(end - part) : strlen(part);
        if (count == 2 && !memcmp(part, "..", 2)) return false;
        part += count;
        if (*part) ++part;
    }
    if (absolute_input) return true;
    char *absolute = zalloc(ZV_NAMEI);
    bool allowed = false;
    int capacity = MAXPATHLEN;
    if (!vn_getpath(directory, absolute, &capacity)) {
        const size_t prefix = strlen(absolute);
        if (prefix + size + 2 <= MAXPATHLEN) {
            if (prefix > 1) strlcat(absolute, "/", MAXPATHLEN);
            strlcat(absolute, path, MAXPATHLEN);
            allowed = switch_bootstrap_below(absolute, SWITCH_BOOTSTRAP_ROOT) ||
                switch_bootstrap_below(absolute, "/var/jb") || switch_bootstrap_below(absolute, "/private/var/jb");
        }
    }
    zfree(ZV_NAMEI, absolute);
    return allowed;
}

static int switch_bootstrap_ready_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
    int ready = os_atomic_load(&switch_bootstrap_ready, acquire);
    int error = SYSCTL_OUT(req, &ready, sizeof(ready));
    if (error || !req->newptr) return error;
    proc_t p = current_proc();
    if (kauth_cred_getuid(kauth_cred_get()) != 0 || !switch_bootstrap_service_caller(p)) return EPERM;
    if ((error = SYSCTL_IN(req, &ready, sizeof(ready)))) return error;
    if (ready != 0 && ready != 1) return EINVAL;
    if (ready && switch_bootstrap_recovery) return EPERM;
    os_atomic_store(&switch_bootstrap_ready, 0, release);
    const int lookup = switch_bootstrap_capture_root();
    if (lookup) return ready ? lookup : 0;
    os_atomic_store(&switch_bootstrap_ready, ready, release);
    return 0;
}

SYSCTL_DECL(_security_mac);
SYSCTL_PROC(_security_mac, OID_AUTO, switch_bootstrap_ready, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            NULL, 0, switch_bootstrap_ready_sysctl, "I", "Bootstrap activation state");
SYSCTL_INT(_security_mac, OID_AUTO, switch_no_tweaks, CTLFLAG_RD | CTLFLAG_LOCKED,
           &switch_bootstrap_recovery, 0, "Tweak recovery boot");
