// SPDX-License-Identifier: GPL-2.0-only
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/proc_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <mach-o/loader.h>
#include <kern/kalloc.h>
#include <switch/app_code.h>
#include <switch/app_code_macho.h>
#include <switch/capabilities.h>

_Static_assert((SWITCH_P_LAPP_CODE & (P_LDELAYTERM | P_LHASTASK | P_LTERM | P_LEXIT | P_LPEXIT |
    P_LTRANSCOMMIT | P_LINTRANSIT | P_LTRANSWAIT | P_LTRACED | P_LSIGEXC | P_LNOATTACH | P_LPPWAIT |
    P_LPTHREADJITALLOWLIST | P_LPTHREADJITFREEZELATE | P_LTRACE_WAIT | P_LLIMCHANGE | P_LLIMWAIT | P_LWAITED |
    P_LINSIGNAL | P_LCUSTOM_STACK | P_LRAGE_VNODES | P_LREGISTER | P_LVMRSRCOWNER | P_LTERM_DECRYPTFAIL |
    P_LTERM_JETSAM | P_LNSPACE_RESOLVER)) == 0, "Switch app-code process flag overlaps XNU");

static int switch_app_code_lookup(const char *path, struct vnode **vp)
{
    struct nameidata *nd = kalloc_type(struct nameidata, Z_WAITOK | Z_NOFAIL);
    NDINIT(nd, LOOKUP, OP_LOOKUP, NOFOLLOW, UIO_SYSSPACE, CAST_USER_ADDR_T(path), vfs_context_kernel());
    nd->ni_flag |= NAMEI_NOFOLLOW_ANY;
    const int error = namei(nd);
    if (!error) {
        *vp = nd->ni_vp;
        nameidone(nd);
    }
    kfree_type(struct nameidata, nd);
    return error;
}

bool switch_app_code_image(struct vnode *vp)
{
    if (!vp || !vnode_isreg(vp)) return false;
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    bool enabled = false;
    if (!vn_getpath(vp, path, &length)) {
        const size_t end = switch_app_code_bundle_end(path);
        if (end && end + sizeof("/" SWITCH_APP_CODE_MARKER) <= MAXPATHLEN) {
            memcpy(path + end, "/" SWITCH_APP_CODE_MARKER, sizeof("/" SWITCH_APP_CODE_MARKER));
            vnode_t marker = NULLVP;
            if (!switch_app_code_lookup(path, &marker)) {
                struct vnode_attr va;
                VATTR_INIT(&va);
                VATTR_WANTED(&va, va_mode);
                VATTR_WANTED(&va, va_data_size);
                VATTR_WANTED(&va, va_nlink);
                if (!vnode_getattr(marker, &va, vfs_context_kernel()) && VATTR_ALL_SUPPORTED(&va)) {
                    enabled = switch_app_code_marker_valid(vnode_isreg(marker), va.va_mode, va.va_data_size,
                                                          va.va_nlink);
                }
                vnode_put(marker);
            }
        }
    }
    zfree(ZV_NAMEI, path);
    return enabled;
}

bool switch_app_code_process(struct proc *p)
{
    return p && (__atomic_load_n(&p->p_lflag, __ATOMIC_RELAXED) & SWITCH_P_LAPP_CODE);
}

void switch_app_code_exec(struct proc *p, bool enabled)
{
    /* An unused local flag preserves the prelinked kexts' proc layout. */
    proc_lock(p);
    if (enabled) p->p_lflag |= SWITCH_P_LAPP_CODE;
    else p->p_lflag &= ~SWITCH_P_LAPP_CODE;
    proc_unlock(p);
}

static int switch_app_code_read(struct vnode *vp, void *data, size_t size, off_t offset)
{
    int resid = 0;
    int error = vn_rdwr(UIO_READ, vp, data, (int)size, offset, UIO_SYSSPACE, 0,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
    return error ? error : resid ? EBADMACHO : 0;
}

int switch_app_code_metadata(struct vnode *vp, void **data, size_t *size)
{
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    int error = vn_getpath(vp, path, &length);
    vnode_t metadata = NULLVP;
    *data = NULL;
    *size = 0;
    if (!error) {
        const size_t used = strlen(path);
        if (used + sizeof(".SwitchEntitlements") > MAXPATHLEN) error = ENAMETOOLONG;
        else memcpy(path + used, ".SwitchEntitlements", sizeof(".SwitchEntitlements"));
    }
    if (!error) error = switch_app_code_lookup(path, &metadata);
    zfree(ZV_NAMEI, path);
    if (error) return error;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_nlink);
    error = vnode_getattr(metadata, &va, vfs_context_kernel());
    if (!error && (!VATTR_ALL_SUPPORTED(&va) || !vnode_isreg(metadata) ||
        (va.va_mode & 07777) != 0600 || va.va_nlink != 1 || !va.va_data_size ||
        va.va_data_size > SWITCH_CAPABILITIES_MAX_SIZE)) error = EINVAL;
    if (!error) {
        *size = (size_t)va.va_data_size;
        *data = kalloc_data(*size, Z_WAITOK);
        if (!*data) error = ENOMEM;
        else error = switch_app_code_read(metadata, *data, *size, 0);
    }
    vnode_put(metadata);
    if (error && *data) {
        kfree_data(*data, *size);
        *data = NULL;
        *size = 0;
    }
    return error;
}

int switch_app_code_arch(struct vnode *vp, int64_t slice, int32_t *type, int32_t *subtype)
{
    struct mach_header_64 header;
    int error = switch_app_code_read(vp, &header, sizeof(header), slice);
    if (!error && (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64)) error = EBADMACHO;
    if (!error) {
        *type = header.cputype;
        *subtype = header.cpusubtype & ~CPU_SUBTYPE_MASK;
    }
    return error;
}

int switch_app_code_coverage(struct vnode *vp, int64_t slice, int64_t *coverage)
{
    off_t file_size = 0;
    int error = vnode_size(vp, &file_size, vfs_context_kernel());
    if (error) return error;
    if (slice < 0 || slice >= file_size || file_size < 8) return EINVAL;
    uint8_t prefix[32];
    error = switch_app_code_read(vp, prefix, 8, 0);
    if (error) return error;
    const size_t fat_bytes = switch_app_code_fat_bytes(prefix, 8);
    if (!fat_bytes || fat_bytes > (uint64_t)file_size) return EBADMACHO;
    uint8_t *fat = kalloc_data(fat_bytes, Z_WAITOK);
    if (!fat) return ENOMEM;
    error = switch_app_code_read(vp, fat, fat_bytes, 0);
    uint64_t slice_size = 0;
    if (!error && !switch_app_code_slice_size(fat, fat_bytes, file_size, slice, &slice_size)) error = EBADMACHO;
    kfree_data(fat, fat_bytes);
    if (error) return error;
    if (slice_size < sizeof(prefix)) return EBADMACHO;
    error = switch_app_code_read(vp, prefix, sizeof(prefix), slice);
    if (error) return error;
    const uint32_t bytes = switch_app_code_u32(prefix + 20);
    if (!bytes || bytes > 256 * 1024 || bytes > slice_size - sizeof(prefix)) return EBADMACHO;
    const size_t size = sizeof(prefix) + bytes;
    uint8_t *commands = kalloc_data(size, Z_WAITOK);
    if (!commands) return ENOMEM;
    error = switch_app_code_read(vp, commands, size, slice);
    uint64_t end = 0;
    if (!error && !switch_app_code_macho_coverage(commands, size, slice_size, &end)) error = EBADMACHO;
    if (!error) *coverage = (int64_t)end;
    kfree_data(commands, size);
    return error;
}
