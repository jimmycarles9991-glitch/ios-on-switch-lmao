// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keys taken out of a preferences plist's top-level dictionary in place, or one added when it is missing
 * (kernel/switch/boot/prefs.c), for the boot-time setup in bsd/kern/kern_exec.c. Free of kernel types so kernel/tests
 * can drive it.
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
    SWITCH_PREFS_ADDED,      /* the key is in, in the output buffer: *out_length is its length */
};

/*
 * A binary plist (bplist00, cfprefsd's format) or an XML one as the setup writes it (<key>name</key><true/>, no
 * whitespace); at most 8 ASCII keys. A binary plist keeps its length: the top-level dictionary shrinks where it is, the
 * dropped entries' objects stay behind unreferenced. Only a DROPPED buffer is a plist to write back.
 */
enum switch_prefs_result switch_prefs_drop_keys(uint8_t *plist, size_t *length, const char *const *keys, size_t count);

/*
 * The top-level dictionary gains `key` (ASCII) = the integer `value` when it does not hold `key` (UNCHANGED when it
 * does, whatever the value), written to `out`, which holds *out_length bytes on entry; switch_prefs_add_room(length)
 * bytes always do, for a plist of at most 64 KiB. A binary plist keeps its objects where they are and gains the key,
 * the value and a copy of the dictionary with the entry added, then a new offset table and trailer (the old dictionary
 * stays behind unreferenced); an XML one gains <key>key</key><integer>value</integer> at the top of the dictionary.
 */
enum switch_prefs_result switch_prefs_add_integer(const uint8_t *plist, size_t length, const char *key, int64_t value,
                                                  uint8_t *out, size_t *out_length);
size_t switch_prefs_add_room(size_t length);

#endif /* SWITCH_PREFS_H */
