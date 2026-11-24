// SPDX-License-Identifier: GPL-2.0-only
/*
 * prefs.c's reader of a binary plist (CoreFoundation CFBinaryPList.c): the trailer's last 32 bytes give the offset
 * table's place and entry size, the object reference size, the object count and the top object; each object starts
 * with a marker byte whose low nibble is its count (0xF: an integer object 0x1k with 2^k bytes of count follows).
 * Every read is bounded by the offset table, which all objects lie before.
 */
#ifndef SWITCH_BOOT_BPLIST_H
#define SWITCH_BOOT_BPLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    kMagic = 8,
    kTrailer = 32
};

static inline uint64_t be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

struct bplist {
    const uint8_t *bytes;
    size_t objects_end; /* the offset table: every object lies before it */
    const uint8_t *offsets;
    size_t offset_size, ref_size;
    uint64_t object_count;
};

/* Object `ref`'s offset; 0 when there is no such object. */
static inline size_t object_at(const struct bplist *b, uint64_t ref)
{
    if (ref >= b->object_count) {
        return 0;
    }
    const uint64_t offset = be(b->offsets + ref * b->offset_size, b->offset_size);
    return offset >= kMagic && offset < b->objects_end ? (size_t)offset : 0;
}

/* The count of the object at `at`: returns the bytes its marker and count take, 0 when unreadable. */
static inline size_t count_at(const struct bplist *b, size_t at, uint64_t *count)
{
    const uint8_t nibble = (uint8_t)(b->bytes[at] & 0xf);
    if (nibble != 0xf) {
        *count = nibble;
        return 1;
    }
    if (at + 2 > b->objects_end || (b->bytes[at + 1] & 0xf0) != 0x10 || (b->bytes[at + 1] & 0xf) > 3) {
        return 0;
    }
    const size_t width = (size_t)1 << (b->bytes[at + 1] & 0xf);
    if (at + 2 + width > b->objects_end) {
        return 0;
    }
    *count = be(b->bytes + at + 2, width);
    return 2 + width;
}

static inline bool key_is(const struct bplist *b, uint64_t ref, const char *key)
{
    const size_t at = object_at(b, ref);
    uint64_t length = 0;
    size_t header = 0;
    if (!at || (b->bytes[at] & 0xf0) != 0x50 || !(header = count_at(b, at, &length))) {
        return false;
    }
    return length == strlen(key) && length <= b->objects_end - at - header &&
           !memcmp(b->bytes + at + header, key, (size_t)length);
}

/* The trailer and the top-level dictionary of a binary plist: false when this is not one it can read. */
struct top_dict {
    uint64_t top, entries;
    size_t at, header;
};

static inline bool read_binary(const uint8_t *plist, size_t length, struct bplist *b, struct top_dict *d)
{
    if (length < kMagic + 1 + kTrailer) {
        return false;
    }
    const uint8_t *trailer = plist + length - kTrailer;
    *b = (struct bplist){
        .bytes = plist,
        .offset_size = trailer[6],
        .ref_size = trailer[7],
        .object_count = be(trailer + 8, 8),
    };
    d->top = be(trailer + 16, 8);
    const uint64_t table = be(trailer + 24, 8);
    if (b->offset_size < 1 || b->offset_size > 8 || b->ref_size < 1 || b->ref_size > 8 || table < kMagic + 1 ||
        table > length - kTrailer || !b->object_count ||
        b->object_count > (length - kTrailer - table) / b->offset_size) {
        return false;
    }
    b->objects_end = (size_t)table;
    b->offsets = plist + table;
    d->at = object_at(b, d->top);
    return d->at && (plist[d->at] & 0xf0) == 0xd0 && (d->header = count_at(b, d->at, &d->entries)) &&
           d->entries <= (b->objects_end - d->at - d->header) / (2 * b->ref_size);
}

/* The first `needle` in text[from, length): its offset, or length when there is none. */
static inline size_t find(const uint8_t *text, size_t length, size_t from, const char *needle, size_t size)
{
    for (size_t at = from; size <= length && at <= length - size; ++at) {
        if (!memcmp(text + at, needle, size)) {
            return at;
        }
    }
    return length;
}

#endif /* SWITCH_BOOT_BPLIST_H */
