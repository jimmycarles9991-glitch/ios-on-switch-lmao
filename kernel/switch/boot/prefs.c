// SPDX-License-Identifier: GPL-2.0-only
/*
 * <switch/prefs.h>. A binary plist's top-level dictionary (CoreFoundation CFBinaryPList.c) is the marker 0xDn (n
 * entries; 0xDF: an integer object 0x1k with 2^k bytes of count follows), its n key references, then its n value
 * references, each the trailer's object reference size, big-endian. Writing only the kept entries where the dictionary
 * is leaves every object, the offset table and the trailer as they were.
 */
#include <switch/prefs.h>

#include <stdbool.h>
#include <string.h>

#define PREFS_MAX_KEYS 8u

enum {
    kMagic = 8,
    kTrailer = 32
};

static uint64_t be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

static void put_be(uint8_t *p, size_t n, uint64_t v)
{
    for (size_t i = n; i-- > 0; v >>= 8) {
        p[i] = (uint8_t)v;
    }
}

struct bplist {
    const uint8_t *bytes;
    size_t objects_end; /* the offset table: every object lies before it */
    const uint8_t *offsets;
    size_t offset_size, ref_size;
    uint64_t object_count;
};

/* Object `ref`'s offset; 0 when there is no such object. */
static size_t object_at(const struct bplist *b, uint64_t ref)
{
    if (ref >= b->object_count) {
        return 0;
    }
    const uint64_t offset = be(b->offsets + ref * b->offset_size, b->offset_size);
    return offset >= kMagic && offset < b->objects_end ? (size_t)offset : 0;
}

/* The count of the object at `at`: returns the bytes its marker and count take, 0 when unreadable. */
static size_t count_at(const struct bplist *b, size_t at, uint64_t *count)
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

static bool key_is(const struct bplist *b, uint64_t ref, const char *key)
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

static enum switch_prefs_result drop_binary(uint8_t *plist, size_t length, const char *const *keys, size_t count)
{
    if (length < kMagic + 1 + kTrailer) {
        return SWITCH_PREFS_UNREADABLE;
    }
    const uint8_t *trailer = plist + length - kTrailer;
    struct bplist b = {
        .bytes = plist,
        .offset_size = trailer[6],
        .ref_size = trailer[7],
        .object_count = be(trailer + 8, 8),
    };
    const uint64_t top = be(trailer + 16, 8), table = be(trailer + 24, 8);
    if (b.offset_size < 1 || b.offset_size > 8 || b.ref_size < 1 || b.ref_size > 8 || table < kMagic + 1 ||
        table > length - kTrailer || !b.object_count || b.object_count > (length - kTrailer - table) / b.offset_size) {
        return SWITCH_PREFS_UNREADABLE;
    }
    b.objects_end = (size_t)table;
    b.offsets = plist + table;

    const size_t at = object_at(&b, top);
    uint64_t entries = 0;
    size_t header = 0;
    if (!at || (plist[at] & 0xf0) != 0xd0 || !(header = count_at(&b, at, &entries)) ||
        entries > (b.objects_end - at - header) / (2 * b.ref_size)) {
        return SWITCH_PREFS_UNREADABLE;
    }
    const size_t rs = b.ref_size, end = at + header + (size_t)entries * 2 * rs;
    /* The edit rewrites the dictionary's bytes: no other object may claim any of them (a damaged offset table). */
    for (uint64_t i = 0; i < b.object_count; ++i) {
        const size_t other = object_at(&b, i);
        if (i != top && other >= at && other < end) {
            return SWITCH_PREFS_UNREADABLE;
        }
    }
    uint64_t dropped[PREFS_MAX_KEYS];
    size_t drops = 0;
    for (uint64_t i = 0; i < entries; ++i) {
        const uint64_t key = be(plist + at + header + i * rs, rs);
        for (size_t k = 0; k < count; ++k) {
            if (key_is(&b, key, keys[k])) {
                if (drops == count) {
                    return SWITCH_PREFS_UNREADABLE; /* a key twice */
                }
                dropped[drops++] = i;
                break;
            }
        }
    }
    if (!drops) {
        return SWITCH_PREFS_UNCHANGED;
    }
    const uint64_t kept = entries - drops;
    if (!kept) {
        return SWITCH_PREFS_EMPTY;
    }

    /* Every kept reference moves to a lower offset or stays: one forward pass reads each before anything lands on it. */
    size_t new_header = 1;
    if (kept < 15) {
        plist[at] = (uint8_t)(0xd0 | kept);
    } else {
        new_header = header;
        put_be(plist + at + 2, header - 2, kept);
    }
    for (size_t half = 0; half < 2; ++half) {
        size_t to = at + new_header + half * (size_t)kept * rs, next = 0;
        for (uint64_t i = 0; i < entries; ++i) {
            if (next < drops && dropped[next] == i) {
                ++next;
                continue;
            }
            memmove(plist + to, plist + at + header + (half * entries + i) * rs, rs);
            to += rs;
        }
    }
    return SWITCH_PREFS_DROPPED;
}

static size_t find(const uint8_t *text, size_t length, size_t from, const char *needle, size_t size)
{
    for (size_t at = from; size <= length && at <= length - size; ++at) {
        if (!memcmp(text + at, needle, size)) {
            return at;
        }
    }
    return length;
}

static enum switch_prefs_result drop_xml(uint8_t *plist, size_t *length, const char *const *keys, size_t count)
{
    static const char *const values[] = {"<true/>", "<false/>"};
    size_t n = *length;
    bool dropped = false;
    for (size_t k = 0; k < count; ++k) {
        char tag[96];
        const size_t name = strlen(keys[k]), size = name + 11;
        if (size > sizeof(tag)) {
            return SWITCH_PREFS_UNREADABLE;
        }
        memcpy(tag, "<key>", 5);
        memcpy(tag + 5, keys[k], name);
        memcpy(tag + 5 + name, "</key>", 6);
        for (size_t at = find(plist, n, 0, tag, size); at < n; at = find(plist, n, at, tag, size)) {
            size_t span = 0;
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]) && !span; ++v) {
                const size_t value = strlen(values[v]);
                if (n - at - size >= value && !memcmp(plist + at + size, values[v], value)) {
                    span = size + value;
                }
            }
            if (!span) {
                return SWITCH_PREFS_UNREADABLE;
            }
            memmove(plist + at, plist + at + span, n - at - span);
            n -= span;
            dropped = true;
        }
    }
    if (!dropped) {
        return SWITCH_PREFS_UNCHANGED;
    }
    if (find(plist, n, 0, "<key>", 5) == n) {
        return SWITCH_PREFS_EMPTY;
    }
    *length = n;
    return SWITCH_PREFS_DROPPED;
}

enum switch_prefs_result switch_prefs_drop_keys(uint8_t *plist, size_t *length, const char *const *keys, size_t count)
{
    if (!plist || !length || !count || count > PREFS_MAX_KEYS) {
        return SWITCH_PREFS_UNREADABLE;
    }
    if (*length >= kMagic && !memcmp(plist, "bplist00", kMagic)) {
        return drop_binary(plist, *length, keys, count);
    }
    const size_t head = *length < 256 ? *length : 256;
    if (find(plist, head, 0, "<plist", 6) < head) {
        return drop_xml(plist, length, keys, count);
    }
    return SWITCH_PREFS_UNREADABLE;
}
