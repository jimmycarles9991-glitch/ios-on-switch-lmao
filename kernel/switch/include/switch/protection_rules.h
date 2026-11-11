// SPDX-License-Identifier: GPL-2.0-only
/*
 * The decisions behind iOS 27's content protection labels (kernel/switch/vfs/protection.c). Plain C, without kernel
 * types, so kernel/tests runs them on the host.
 */
#ifndef SWITCH_PROTECTION_RULES_H
#define SWITCH_PROTECTION_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* sys/content_protection.h's values; protection.c checks that they agree. */
#define SWITCH_PROTECTION_DEFAULT  (-1)
#define SWITCH_PROTECTION_DIR_NONE 0
#define SWITCH_PROTECTION_C        3
#define SWITCH_PROTECTION_D        4

/* xattr_protected's namespace: userland can neither read, set, remove nor (listxattr hides it) see the label. */
#define SWITCH_PROTECTION_XATTR "com.apple.system.switch.protection-class"

/*
 * The class of a vnode without a label. One older than the epoch was made before labelling began and reads as every
 * vnode did then, D. APFS gives each new inode the volume's next object id (apfs_next_obj_id, never reused; a hard link
 * keeps its inode's), so a vnode at or after the epoch was made since, and reads as a new one does on an iPad.
 */
static inline int32_t switch_protection_unlabelled(bool isdir, bool known_id, uint64_t fileid, uint64_t epoch)
{
    if (known_id && fileid < epoch) {
        return SWITCH_PROTECTION_D;
    }
    return isdir ? SWITCH_PROTECTION_DIR_NONE : SWITCH_PROTECTION_C;
}

/*
 * The label a new vnode needs: the class its create asked for, else its labelled directory's (a directory without a
 * class gives a file none). SWITCH_PROTECTION_DEFAULT when it reads right unlabelled.
 */
static inline int32_t switch_protection_created_label(bool isdir, int32_t requested, bool parent_labelled,
                                                      int32_t parent)
{
    int32_t pclass = requested;
    if (pclass == SWITCH_PROTECTION_DEFAULT && parent_labelled && (isdir || parent != SWITCH_PROTECTION_DIR_NONE)) {
        pclass = parent;
    }
    if (pclass == switch_protection_unlabelled(isdir, false, 0, 0)) {
        return SWITCH_PROTECTION_DEFAULT;
    }
    return pclass;
}

/* Takes name out of a listxattr buffer of NUL-terminated names; the buffer's new length. */
static inline size_t switch_protection_hide(char *names, size_t length, const char *name)
{
    const size_t size = strlen(name) + 1;
    for (size_t at = 0; at < length;) {
        const char *end = (const char *)memchr(names + at, '\0', length - at);
        const size_t entry = end != NULL ? (size_t)(end - (names + at)) + 1 : length - at;
        if (entry == size && memcmp(names + at, name, size) == 0) {
            memmove(names + at, names + at + entry, length - at - entry);
            return length - entry;
        }
        at += entry;
    }
    return length;
}

#endif /* SWITCH_PROTECTION_RULES_H */
