// SPDX-License-Identifier: GPL-2.0-only
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>
#include <mach/mach_types.h>
#include <sys/imgact.h>
#include <sys/codesign.h>
#include <sys/ubc_internal.h>
#include <sys/systm.h>
#include <kern/cs_blobs.h>
#include <switch/bootstrap.h>

#define SWITCH_BOOTSTRAP_ROOT "/private/var/Switch/bootstrap/" SWITCH_IPSW_BUILD "/root"
#define SWITCH_BOOTSTRAP_APPLICATIONS SWITCH_BOOTSTRAP_ROOT "/Applications"

static bool switch_bootstrap_native_service(proc_t p)
{
    const unsigned int required = CS_VALID | CS_SIGNED;
    if (!p || !p->p_textvp || (proc_getcsflags(p) & required) != required ||
        !csproc_get_platform_binary(p)) return false;
    struct cs_blob *blob = csproc_get_blob(p);
    return blob && blob->csb_reconstituted < 2;
}

static bool switch_bootstrap_app_vnode(vnode_t vp, bool executable)
{
    if (!switch_bootstrap_on_data(vp) || (executable && !vnode_isreg(vp))) return false;
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    const bool matched = !vn_getpath(vp, path, &length) &&
        switch_bootstrap_app_path(path, SWITCH_BOOTSTRAP_ROOT, executable);
    zfree(ZV_NAMEI, path);
    return matched && (executable ? switch_bootstrap_image(vp) :
        switch_bootstrap_member(vp, vnode_isdir(vp)));
}

bool switch_bootstrap_app_access(vnode_t vp, int mode)
{
    return !(mode & (FWRITE | O_TRUNC | O_APPEND | FEXEC)) &&
        switch_bootstrap_on_data(vp) &&
        switch_bootstrap_native_service(current_proc()) &&
        (switch_bootstrap_alias(vp) || switch_bootstrap_app_vnode(vp, false));
}

bool switch_bootstrap_read_access(vnode_t vp, int mode)
{
    if (!switch_bootstrap_on_data(vp) || (mode & (FWRITE | O_TRUNC | O_APPEND))) return false;
    return switch_bootstrap_access(vp, mode) || switch_bootstrap_app_access(vp, mode);
}

static bool switch_bootstrap_command_vnode(vnode_t vp)
{
    if (!switch_bootstrap_image(vp)) return false;
    char *path = zalloc(ZV_NAMEI);
    int length = MAXPATHLEN;
    const bool command = !vn_getpath(vp, path, &length) && switch_bootstrap_tool_path(path, SWITCH_BOOTSTRAP_ROOT);
    zfree(ZV_NAMEI, path);
    return command;
}

static bool switch_bootstrap_exec_file(vnode_t vp)
{
    if (!vp || !vnode_isreg(vp)) return false;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_mode);
    return !vnode_getattr(vp, &va, vfs_context_kernel()) && VATTR_IS_SUPPORTED(&va, va_mode) &&
        !(va.va_mode & (VSUID | VSGID));
}

unsigned int switch_bootstrap_exec_preflight(proc_t p, struct image_params *imgp)
{
    vnode_t target = (imgp->ip_flags & IMGPF_INTERPRET) ? imgp->ip_scriptvp : imgp->ip_vp;
    if (!p || !p->p_textvp || !switch_bootstrap_on_data(target) || !switch_bootstrap_exec_file(imgp->ip_vp)) return 0;
    unsigned int caller = 0;
    if (switch_bootstrap_native_service(p)) {
        char *path = zalloc(ZV_NAMEI);
        int length = MAXPATHLEN;
        if (!vn_getpath(p->p_textvp, path, &length) && !strcmp(path, "/usr/libexec/xpcproxy"))
            caller = SWITCH_BOOTSTRAP_EXEC_LAUNCHER;
        zfree(ZV_NAMEI, path);
    }
    const unsigned int required = CS_VALID | CS_SIGNED;
    if (!caller && (proc_getcsflags(p) & required) == required && csproc_get_platform_binary(p) &&
        (switch_bootstrap_tool_image(p->p_textvp, p->p_textoff) ||
         switch_bootstrap_persona_caller(p) || switch_bootstrap_service_caller(p)))
        caller = SWITCH_BOOTSTRAP_EXEC_CALLER;
    if (!caller) return 0;
    if (imgp->ip_flags & IMGPF_INTERPRET)
        return switch_bootstrap_command_vnode(imgp->ip_scriptvp) &&
            switch_bootstrap_exec_file(imgp->ip_scriptvp) ? caller : 0;
    return switch_bootstrap_command_vnode(imgp->ip_vp) ||
        (caller == SWITCH_BOOTSTRAP_EXEC_LAUNCHER && switch_bootstrap_app_vnode(imgp->ip_vp, true)) ? caller : 0;
}

int switch_bootstrap_exec_check(struct image_params *imgp)
{
    if (!imgp->ip_switch_package_exec) return 0;
    if (!switch_bootstrap_exec_file(imgp->ip_vp) || imgp->ip_arch_offset > INT64_MAX ||
        ((imgp->ip_vattr->va_mode | imgp->ip_origvattr->va_mode) & (VSUID | VSGID))) return EPERM;
    struct cs_blob *blob = csvnode_get_blob(imgp->ip_vp, (int64_t)imgp->ip_arch_offset);
    if (!blob || blob->csb_base_offset != (int64_t)imgp->ip_arch_offset) return EPERM;
    const bool interpreted = imgp->ip_flags & IMGPF_INTERPRET;
    if (interpreted != (imgp->ip_scriptvp != NULLVP)) return EPERM;
    if (interpreted && (!switch_bootstrap_command_vnode(imgp->ip_scriptvp) ||
        !switch_bootstrap_exec_file(imgp->ip_scriptvp))) return EPERM;
    if (switch_bootstrap_command_vnode(imgp->ip_vp) ||
        (!interpreted && switch_bootstrap_app_vnode(imgp->ip_vp, true)))
        return switch_bootstrap_sandbox_target(imgp->ip_vp, (int64_t)imgp->ip_arch_offset) ? 0 : EPERM;
    return interpreted && blob->csb_reconstituted < 2 && blob->csb_cd &&
        (csblob_get_flags(blob) & CS_VALID) && csblob_get_platform_binary(blob) ? 0 : EPERM;
}

static bool switch_bootstrap_app_route(const char *path)
{
    const char *const aliases[] = { "/var/jb", "/private/var/jb" };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i) {
        const size_t length = strlen(aliases[i]);
        if (!strncmp(path, aliases[i], length) && (!path[length] || path[length] == '/'))
            return !path[length] || switch_bootstrap_app_path(path + length, "", false);
    }
    if (!strncmp(path, "/var/", sizeof("/var/") - 1)) path += sizeof("/var") - 1;
    else if (!strncmp(path, "/private/var/", sizeof("/private/var/") - 1)) path += sizeof("/private/var") - 1;
    else return !strcmp(path, "/") || !strcmp(path, "/private") || !strcmp(path, "/private/var") || !strcmp(path, "/var");
    const char *applications = &SWITCH_BOOTSTRAP_APPLICATIONS[sizeof("/private/var") - 1];
    const size_t length = strlen(path), limit = strlen(applications);
    if (length <= limit && !memcmp(path, applications, length) &&
        (length == limit || applications[length] == '/')) return true;
    return switch_bootstrap_app_path(path, &SWITCH_BOOTSTRAP_ROOT[sizeof("/private/var") - 1], false);
}

bool switch_bootstrap_app_preflight(vnode_t directory, const char *path, size_t length)
{
    if (!switch_bootstrap_active() || !directory || !path || !length || length >= MAXPATHLEN ||
        !switch_bootstrap_native_service(current_proc())) return false;
    const size_t size = strnlen(path, MAXPATHLEN);
    if (!size || size == MAXPATHLEN) return false;
    if (*path == '/') return switch_bootstrap_app_route(path);
    char *absolute = zalloc(ZV_NAMEI);
    bool allowed = false;
    int capacity = MAXPATHLEN;
    if (!vn_getpath(directory, absolute, &capacity)) {
        const size_t prefix = strlen(absolute);
        if (prefix + size + 2 <= MAXPATHLEN) {
            if (prefix > 1) strlcat(absolute, "/", MAXPATHLEN);
            strlcat(absolute, path, MAXPATHLEN);
            allowed = switch_bootstrap_app_route(absolute);
        }
    }
    zfree(ZV_NAMEI, absolute);
    return allowed;
}

bool switch_bootstrap_app_lookup(vnode_t directory, const char *name, size_t length)
{
    if (!switch_bootstrap_active() || !directory || !name || !length || length >= MAXPATHLEN ||
        (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.') ||
        memchr(name, '/', length) || memchr(name, '\0', length)) return false;
    if (!switch_bootstrap_on_data(directory) &&
        !(length == 7 && !memcmp(name, "private", 7)) && !(length == 3 && !memcmp(name, "var", 3))) return false;
    if (!switch_bootstrap_native_service(current_proc())) return false;
    char *absolute = zalloc(ZV_NAMEI);
    bool allowed = false;
    int capacity = MAXPATHLEN;
    if (!vn_getpath(directory, absolute, &capacity)) {
        const size_t prefix = strlen(absolute);
        if (prefix + length + 2 <= MAXPATHLEN) {
            if (prefix > 1) absolute[prefix] = '/';
            const size_t start = prefix + (prefix > 1);
            memcpy(absolute + start, name, length);
            absolute[start + length] = '\0';
            allowed = switch_bootstrap_app_route(absolute);
        }
    }
    zfree(ZV_NAMEI, absolute);
    return allowed;
}
