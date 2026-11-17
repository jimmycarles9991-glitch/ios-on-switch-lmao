// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_APP_CODE_H
#define SWITCH_APP_CODE_H

#include <sys/cdefs.h>
#include <kern/cs_blobs.h>
#include <switch/app_code_rules.h>

#define SWITCH_APP_CODE_CS_FLAGS (CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED)
#define SWITCH_APP_CODE_METADATA 2u

struct proc;
struct vnode;
struct _vm_map;
struct switch_app_code_capabilities {
    char identity[256];
    char team[256];
    void *xml;
    size_t xml_size;
};

__BEGIN_DECLS
bool switch_app_code_image(struct vnode *vp);
bool switch_app_code_process(struct proc *p);
void switch_app_code_exec(struct proc *p, bool enabled);
bool switch_app_code_map_enabled(struct _vm_map *map);
void switch_app_code_map(struct _vm_map *map);
int switch_app_code_coverage(struct vnode *vp, int64_t slice, int64_t *coverage);
int switch_app_code_arch(struct vnode *vp, int64_t slice, int32_t *type, int32_t *subtype);
int switch_app_code_metadata(struct vnode *vp, void **data, size_t *size);
int switch_app_code_capability_decode(const void *data, size_t size, struct switch_app_code_capabilities *result);
int switch_app_code_blob(struct vnode *vp, int64_t slice);
#if XNU_12377_121_6
int switch_app_code_dyld(struct _vm_map *map, const void *commands, size_t size, int64_t slide);
#endif
__END_DECLS

#endif
