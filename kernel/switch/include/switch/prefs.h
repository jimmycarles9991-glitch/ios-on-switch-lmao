// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keys taken out of a preferences plist's top-level dictionary in place (kernel/switch/boot/prefs.c), for the boot-time
 * setup in bsd/kern/kern_exec.c. Free of kernel types so kernel/tests can drive it.
 */
#ifndef SWITCH_PREFS_H
#define SWITCH_PREFS_H

#include <stddef.h>
#include <stdint.h>

enum switch_prefs_result {
    SWITCH_PREFS_UNCHANGED,  /* none of the keys is there */
    SWITCH_PREFS_DROPPED,    /* the keys are gone; *length is the plist's new length */
    SWITCH_PREFS_EMPTY,      /* the keys were all it held: the file can go */
    SWITCH_PREFS_UNREADABLE, /* not a plist this edits */
};

/*
 * A binary plist (bplist00, cfprefsd's format) or an XML one as the setup writes it (<key>name</key><true/>, no
 * whitespace); at most 8 ASCII keys. A binary plist keeps its length: the top-level dictionary shrinks where it is, the
 * dropped entries' objects stay behind unreferenced. Only a DROPPED buffer is a plist to write back.
 */
enum switch_prefs_result switch_prefs_drop_keys(uint8_t *plist, size_t *length, const char *const *keys, size_t count);

#endif /* SWITCH_PREFS_H */
