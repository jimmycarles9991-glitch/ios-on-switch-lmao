// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_BOOTSTRAP_H
#define SWITCH_BOOTSTRAP_H

#include <sys/cdefs.h>
#include <switch/bootstrap_rules.h>

struct vnode;
struct proc;
struct cs_blob;
struct _vm_map;
struct _posix_spawn_persona_info;
struct image_params;

__BEGIN_DECLS
void switch_bootstrap_capture(void);
bool switch_bootstrap_member(struct vnode *vp, bool directory);
bool switch_bootstrap_image(struct vnode *vp);
int switch_bootstrap_signature(struct vnode *vp, struct cs_blob *blob, unsigned int *flags, bool *admitted);
int switch_bootstrap_validate(struct vnode *vp);
bool switch_bootstrap_signed(struct vnode *vp);
bool switch_bootstrap_target(struct vnode *vp);
bool switch_bootstrap_process(struct proc *p);
void switch_bootstrap_exec(struct proc *p, bool enabled);
bool switch_bootstrap_access(struct vnode *vp, int mode);
bool switch_bootstrap_alias(struct vnode *vp);
bool switch_bootstrap_app_access(struct vnode *vp, int mode);
bool switch_bootstrap_read_access(struct vnode *vp, int mode);
unsigned int switch_bootstrap_exec_preflight(struct proc *p, struct image_params *imgp);
int switch_bootstrap_exec_check(struct image_params *imgp);
bool switch_bootstrap_app_lookup(struct vnode *directory, const char *name, size_t length);
bool switch_bootstrap_app_preflight(struct vnode *directory, const char *path, size_t length);
bool switch_bootstrap_sandbox_target(struct vnode *vp, int64_t offset);
bool switch_bootstrap_tool_image(struct vnode *vp, int64_t offset);
bool switch_bootstrap_exec_mapping(struct proc *p, struct vnode *vp, uint64_t offset, int prot);
bool switch_bootstrap_persona_caller(struct proc *p);
bool switch_bootstrap_service_caller(struct proc *p);
bool switch_bootstrap_persona_request(const struct _posix_spawn_persona_info *info);
int switch_bootstrap_persona_init(void);
bool switch_bootstrap_lookup(struct vnode *directory, const char *name, size_t length);
bool switch_bootstrap_preflight(struct vnode *directory, const char *path, size_t length);
bool switch_bootstrap_map_enabled(struct _vm_map *map);
void switch_bootstrap_map(struct _vm_map *map);
int switch_bootstrap_dyld_result(struct proc *p, const char *policy, int call, uint64_t argument, int result);
__END_DECLS

#endif
